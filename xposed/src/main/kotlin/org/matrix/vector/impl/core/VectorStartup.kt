package org.matrix.vector.impl.core

import android.app.ActivityThread
import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.IBinder
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.view.Choreographer
import dalvik.system.DexFile
import java.io.File
import java.lang.reflect.Executable
import java.util.concurrent.atomic.AtomicBoolean
import org.matrix.vector.impl.VectorLifecycleManager
import org.matrix.vector.impl.di.LegacyPackageInfo
import org.matrix.vector.util.Utils
import org.matrix.vector.ipc.IFrameworkService
import org.matrix.vector.impl.di.VectorBootstrap
import org.matrix.vector.impl.hookers.*
import org.matrix.vector.impl.hooks.VectorHookBuilder
import org.matrix.vector.nativebridge.HookBridge

/**
 * Modern framework initialization and bootstrap sequence. Deploys interceptors into the ART runtime
 * and handles early process deoptimization.
 */
object VectorStartup {

    private val noArtDeferredStarted = AtomicBoolean(false)
    private val noArtDeferredDispatched = AtomicBoolean(false)
    private val artDeferredFallbackStarted = AtomicBoolean(false)
    private val artDeferredFallbackDispatched = AtomicBoolean(false)
    private val appAttachLifecycleReplayStarted = AtomicBoolean(false)
    private val appAttachLifecycleReplayDispatched = AtomicBoolean(false)
    private val appAttachCompleted = AtomicBoolean(false)
    private val appPreAttachLoadedApkHooksInstalled = AtomicBoolean(false)
    private val appPostAttachBootstrapCompleted = AtomicBoolean(false)
    @Volatile private var systemProcess = false

    // 临时诊断开关：debug.vector.disable.hook，逗号分隔值
    // 可选：attach / ctor / createappfactory / createcl，分别禁用对应框架 hook 安装
    private fun isHookDisabled(vararg names: String): Boolean {
        val raw = runCatching {
            Class.forName("android.os.SystemProperties")
                .getMethod("get", String::class.java, String::class.java)
                .invoke(null, "debug.vector.disable.hook", "") as String
        }.getOrDefault("")
        if (raw.isEmpty()) return false
        val items = raw.split(",").map { it.trim().lowercase() }
        return names.any { it in items }
    }

    private data class DeferredAppState(
        val activityThread: ActivityThread,
        val packageName: String,
        val processName: String,
        val appInfo: ApplicationInfo,
        val classLoader: ClassLoader,
        val defaultClassLoader: ClassLoader,
        val appComponentFactory: Any?,
        val resDir: String?,
    )

    private fun elapsedUs(startNs: Long): Long = (System.nanoTime() - startNs) / 1_000L

    private inline fun <T> traceStartupStep(label: String, block: () -> T): T {
        val startNs = System.nanoTime()
        Utils.logI("[startup-trace] step begin: $label")
        return try {
            val result = block()
            Utils.logI(
                "[startup-trace] step end: $label elapsedUs=${elapsedUs(startNs)}"
            )
            result
        } catch (t: Throwable) {
            Utils.logE(
                "[startup-trace] step failed: $label elapsedUs=${elapsedUs(startNs)}",
                t,
            )
            throw t
        }
    }

    private inline fun traceStartupHook(
        label: String,
        executable: Executable,
        block: () -> Unit,
    ) {
        // 2026-09-11 (user-approved): logcat 里不再输出反射目标全文（Duck Detector 的
        // LSPosed logcat pattern 会命中 target=android.app.LoadedApk#... 这类可识别字符串，
        // 成为 [WARNING] 的证据面）。保留 label/耗时以维持排障能力。
        val startNs = System.nanoTime()
        Utils.logI("[startup-trace] hook begin: label=$label")
        try {
            block()
            Utils.logI(
                "[startup-trace] hook end: label=$label " +
                    "elapsedUs=${elapsedUs(startNs)}"
            )
        } catch (t: Throwable) {
            Utils.logE(
                "[startup-trace] hook failed: label=$label " +
                    "elapsedUs=${elapsedUs(startNs)}",
                t,
            )
            throw t
        }
    }

    @JvmStatic
    fun init(
        isSystem: Boolean,
        processName: String?,
        appDir: String?,
        service: IFrameworkService?,
    ) {
        VectorServiceClient.init(service, processName ?: "android")
        VectorDeopter.deoptBootMethods()

        // Prefetch legacy module metadata on a background thread. loadLegacyModules() later
        // calls VectorServiceClient.getLegacyModules() synchronously on the app main thread
        // during pre-attach; when the binder thread pool is not yet ready that round-trip can
        // stall launch by ~2.2s. The prefetch runs its own binder call off the main thread and
        // fills the VectorServiceClient cache, so getLegacyModules() returns from cache instead
        // of blocking the main thread. Module code is not loaded early.
        if (service != null) {
            Thread(
                    {
                        runCatching { VectorServiceClient.prefetchLegacyModules() }
                            .onFailure {
                                Utils.logE("Vector legacy module prefetch failed", it)
                            }
                    },
                    "Vector-LegacyModulePrefetch",
                )
                .apply {
                    isDaemon = true
                    start()
                }
        }
    }

    /**
     * Registers a pre-built [LoadedApk] whose constructor ran before the hooks were installed -- the
     * LSPatch rootless target being the one case. Once tracked, [LoadedApkCreateAppFactoryHooker] and
     * [LoadedPackageCreateCLHooker] dispatch its `onPackageLoaded`/`onPackageReady` (and legacy
     * `handleLoadPackage`) exactly once when its class loader is realized, with `onPackageLoaded`
     * running *before* the app's `AppComponentFactory` static initializer.
     */
    @JvmStatic
    fun trackLoadedApk(loadedApk: Any) {
        LoadedApkTracker.activeApks.add(loadedApk)
    }

    @JvmStatic
    fun bootstrap(isSystem: Boolean, systemServerStarted: Boolean) {
        systemProcess = isSystem
        val totalStartNs = System.nanoTime()
        Utils.logI(
            "[startup-trace] VectorStartup.bootstrap begin " +
                "isSystem=$isSystem systemServerStarted=$systemServerStarted"
        )

        try {
            // rawVector installs ActivityThread.attach and LoadedApk hooks before attach runs,
            // then loads modules only after chain.proceed() returns.
            traceStartupStep("installAppAttachInterceptor") { installAppAttachInterceptor() }

            if (isSystem) {
                traceStartupStep("installPostAttachFrameworkInterceptors[system]") {
                    installPostAttachFrameworkInterceptors()
                }

                traceStartupStep("installSystemServerProcessHook") {
                    val zygoteInitClass = Class.forName("com.android.internal.os.ZygoteInit")
                    zygoteInitClass.declaredMethods
                        .filter { it.name == "handleSystemServerProcess" }
                        .forEachIndexed { index, method ->
                            traceStartupHook("ZygoteInit.handleSystemServerProcess[$index]", method) {
                                VectorHookBuilder(method).intercept(HandleSystemServerProcessHooker)
                            }
                        }
                }
            } else {
                traceStartupStep("installPreAttachLoadedApkInterceptors[app]") {
                    installPreAttachLoadedApkInterceptors()
                }
                // Keep only the lightweight bootstrap before attach. rawVector reaches module
                // loading from AppAttachHooker after ActivityThread.attach.proceed(), while the
                // rustFrida pre-init controller assumes a paused child. Vector's app fork path is
                // not guaranteed to remain paused, so the full controller must not run here.
                Utils.logI(
                    "[startup-trace] app pre-attach bootstrap ready: " +
                        "ActivityThread.attach=true " +
                        "LoadedApk=${appPreAttachLoadedApkHooksInstalled.get()} " +
                        "completeArtBootstrap=deferred-until-attach"
                )
            }

            // Late System Server Injection
            if (systemServerStarted) {
                traceStartupStep("lateSystemServerBootstrap") {
                    val activityService: IBinder? = android.os.ServiceManager.getService("activity")
                    if (activityService != null) {
                        val classLoader = activityService.javaClass.classLoader
                        if (classLoader != null) {
                            // Manually trigger the routines that the hooks normally would
                            HandleSystemServerProcessHooker.initSystemServer(classLoader, isLate = true)

                            // ActivityThread.attach ran before we were injected, so AppAttachHooker
                            // will never fire here and nothing else instantiates modules.
                            val activityThread = ActivityThread.currentActivityThread()
                            if (activityThread != null) {
                                VectorBootstrap.withLegacy { it.loadModules(activityThread) }
                            } else {
                                Utils.logW(
                                    "Late system server injection: no current ActivityThread, " +
                                        "modules cannot be loaded"
                                )
                            }

                            // The services are already running; this is a best-effort late
                            // dispatch for modules that use the system-server callback.
                            Utils.logW(
                                "Late system server injection: dispatching " +
                                    "onSystemServerStarting after system server has already " +
                                    "started; critical services are live"
                            )
                            StartBootstrapServicesHooker.dispatchSystemServerLoaded(classLoader)
                        }
                    }
                }
            }
        } finally {
            Utils.logI(
                "[startup-trace] VectorStartup.bootstrap end " +
                    "elapsedUs=${elapsedUs(totalStartNs)}"
            )
        }
    }

    private fun installAppAttachInterceptor() {
        if (isHookDisabled("attach")) {
            Utils.logW("[startup-trace] ActivityThread.attach hook DISABLED by debug.vector.disable.hook")
            return
        }
        val activityThreadClass = Class.forName("android.app.ActivityThread")
        val methods = activityThreadClass.declaredMethods.filter { it.name == "attach" }
        methods.forEachIndexed { index, method ->
            traceStartupHook("ActivityThread.attach[$index]", method) {
                VectorHookBuilder(method).intercept(AppAttachHooker)
            }
        }
    }

    private fun installPreAttachLoadedApkInterceptors() {
        installLoadedApkInterceptors("app-pre-attach")
        appPreAttachLoadedApkHooksInstalled.set(true)
    }

    private fun installPostAttachFrameworkInterceptors() {
        traceStartupStep("installPostAttachFrameworkInterceptors.total") {
            val crashMethod =
                traceStartupStep("reflect.Thread.dispatchUncaughtException") {
                    Thread::class
                        .java
                        .declaredMethods
                        .firstOrNull { it.name == "dispatchUncaughtException" }
                }
            if (crashMethod != null) {
                traceStartupHook("CrashDumpHooker", crashMethod) {
                    VectorHookBuilder(crashMethod).intercept(CrashDumpHooker)
                }
            } else {
                Utils.logW("[startup-trace] hook target missing: Thread.dispatchUncaughtException")
            }

            if (!systemProcess) {
                val processName =
                    runCatching { ActivityThread.currentProcessName() }.getOrNull()
                        ?: runCatching { android.app.Application.getProcessName() }.getOrNull()
                if (processName?.startsWith("org.zywx") == true) {
                    Utils.logW(
                        "[startup-trace] DexTrustHooker skipped for isolation package=$processName"
                    )
                    return@traceStartupStep
                }
                val dexMethods =
                    traceStartupStep("reflect.DexFile.openDexFile") {
                        DexFile::class
                            .java
                            .declaredMethods
                            .filter {
                                it.name == "openDexFile" ||
                                    it.name == "openInMemoryDexFile" ||
                                    it.name == "openInMemoryDexFiles"
                            }
                    }
                dexMethods.forEachIndexed { index, method ->
                    traceStartupHook("DexTrustHooker[$index]", method) {
                        VectorHookBuilder(method).intercept(DexTrustHooker)
                    }
                }
            }

            if (systemProcess) {
                installLoadedApkInterceptors("system-post-attach")
            }
        }
    }

    private fun installLoadedApkInterceptors(phase: String) {
        if (isHookDisabled("ctor") && isHookDisabled("createappfactory") && isHookDisabled("createcl")) {
            Utils.logW("[startup-trace] all LoadedApk hooks DISABLED by debug.vector.disable.hook phase=$phase")
            return
        }
        traceStartupStep("installLoadedApkInterceptors[$phase].total") {
            // Application Load Interceptors
            val loadedApkClass =
                traceStartupStep("reflect.LoadedApk.class[$phase]") {
                    Class.forName("android.app.LoadedApk")
                }
            val loadedApkConstructors =
                traceStartupStep("reflect.LoadedApk.constructors[$phase]") {
                    loadedApkClass.declaredConstructors.toList()
                }
            if (!isHookDisabled("ctor")) loadedApkConstructors.forEachIndexed { index, constructor ->
                // Hook all constructors of LoadedApk to catch early instantiations securely.
                // R1 oscillation (2026-09-11, user-approved): framework hooks return to the default
                // process-wide WxShadow stealth, matching rustFrida prepare_hook_target semantics
                // (WxShadow => sflag=1). The original livelock was mitigated by re-enabling
                // same-page LDR literal relocation (hook_engine_mem.c disable_wxshadow_ldr_reloc=0);
                // any residual r/x flip cost is the accepted lean-tradeoff of that decision.
                // Duck-detector alignment: never mprotect-write boot-framework.oat code pages
                // (that produced the 8 kB shared-dirty [DANGER]). Hooker label renamed to a
                // LoadedApk synonym to avoid framework hooker names in logcat traces.
                traceStartupHook("LoadedPackageCtorHooker[$phase][$index]", constructor) {
                    VectorHookBuilder(constructor).intercept(LoadedPackageCtorHooker)
                }
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && !isHookDisabled("createappfactory")) {
                val createAppFactoryMethods =
                    traceStartupStep("reflect.LoadedApk.createAppFactory[$phase]") {
                        loadedApkClass.declaredMethods.filter { it.name == "createAppFactory" }
                    }
                createAppFactoryMethods.forEachIndexed { index, method ->
                    traceStartupHook(
                        "LoadedApkCreateAppFactoryHooker[$phase][$index]",
                        method,
                    ) {
                        VectorHookBuilder(method).intercept(LoadedApkCreateAppFactoryHooker)
                    }
                }
            }

            val createClassLoaderMethods: List<java.lang.reflect.Method> =
                if (isHookDisabled("createcl")) emptyList()
                else traceStartupStep(
                    "reflect.LoadedApk.createOrUpdateClassLoaderLocked[$phase]"
                ) {
                    loadedApkClass.declaredMethods
                        .filter { it.name == "createOrUpdateClassLoaderLocked" }
                }
            createClassLoaderMethods.forEachIndexed { index, method ->
                traceStartupHook("LoadedPackageCreateCLHooker[$phase][$index]", method) {
                    VectorHookBuilder(method).intercept(LoadedPackageCreateCLHooker)
                }
            }

            Utils.logI(
                "[startup-trace] LoadedApk hooks installed phase=$phase " +
                    "constructors=${loadedApkConstructors.size} " +
                    "createAppFactory=${if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) "available" else "skipped"} " +
                    "createClassLoader=${createClassLoaderMethods.size}"
            )
        }
    }

    @JvmStatic
    fun markAppAttachCompleted() {
        appAttachCompleted.set(true)
    }

    @JvmStatic
    fun isAppAttachCompleted(): Boolean = systemProcess || appAttachCompleted.get()

    @JvmStatic
    fun arePreAttachLoadedApkHooksInstalled(): Boolean =
        systemProcess || appPreAttachLoadedApkHooksInstalled.get()

    @JvmStatic
    fun completeAppBootstrapAfterAttach() {
        if (systemProcess || !appPostAttachBootstrapCompleted.compareAndSet(false, true)) return
        // 2026-09-04 实测：DexTrustHooker（DexFile#openDexFile）等 hook 的 Install() 反射段
        // （Class.getName/getParameterTypes 等 JNI→Java 调用）会与全进程 blocking GC / safepoint
        // 请求互等，实测阻塞主线程 0.4~5.4s（MIUIScout ANR 栈：main 卡在 Class.getName ←
        // HookBridge.hookMethod(Native Method)；同窗口 Signal Catcher "Waiting for a blocking
        // GC ObjectsAllocated"，GC 结束与主线程恢复同一毫秒）。这些 hook 是 Vector 自有监控
        // （CrashDump/DexTrust），无 rawVector 时序契约约束；art hook 安装与执行线程无关
        // （entrypoint 是进程级状态，engine 自带锁保证并发安全），移到后台线程避免冻结主
        // 线程。CAS 已保证 exactly-once。
        Thread(
                {
                    installPostAttachFrameworkInterceptors()
                    Utils.logI("[startup-trace] Vector post-attach framework bootstrap completed")
                },
                "Vector-PostAttachBootstrap",
            )
            .apply {
                isDaemon = true
                start()
            }
    }

    /**
     * rawVector installs LoadedApk hooks before attach, so the first package lifecycle is emitted
     * from the real LoadedApk call path. This replay remains only for late/no-ART fallback paths
     * where the pre-attach hook installation could not happen.
     */
    @JvmStatic
    fun replayFirstPackageLifecycleAfterAttach(reason: String) {
        if (systemProcess || !appAttachLifecycleReplayStarted.compareAndSet(false, true)) return

        resolveDeferredAppState()?.let { state ->
            dispatchAppAttachLifecycleReplay(state, reason, 0)
            return
        }

        Thread(
                {
                    runCatching { runAppAttachLifecycleReplay(reason) }
                        .onFailure { Utils.logE("App attach lifecycle replay failed", it) }
                },
                "Vector-AppAttachLifecycleReplay",
            )
            .apply {
                isDaemon = true
                start()
            }
    }

    @JvmStatic
    fun bootstrapNoArtDeferred() {
        if (!noArtDeferredStarted.compareAndSet(false, true)) return

        // rustFrida reference:
        // - quickjs-hook/src/jsapi/java/java_boot.js Java._installGateHook defers the gate while
        //   the raw-clone child is pre-attach, avoiding ART routing setup in the start_timeout
        //   window.
        // - quickjs-hook/src/jsapi/java/reflect.rs reprobe_classloader_once_with_env polls
        //   ActivityThread.currentActivityThread().getApplication().getClassLoader().
        Thread(
                {
                    runCatching { runNoArtDeferredBootstrap() }
                        .onFailure { Utils.logE("No-ART deferred bootstrap failed", it) }
                },
                "Vector-NoArtBootstrap",
            )
            .apply {
                isDaemon = true
                start()
            }
    }

    @JvmStatic
    fun bootstrapArtDeferredFallback() {
        if (systemProcess || !artDeferredFallbackStarted.compareAndSet(false, true)) return

        // rawVector normally reaches module loading from ActivityThread.attach before
        // bindApplication creates the first LoadedApk/ClassLoader. If Vector's lightweight
        // pre-attach hook misses that boundary, the rawVector LoadedApk hooks were also not
        // installed in time, so actively replay the first package lifecycle once the app
        // ClassLoader is observable.
        Thread(
                {
                    runCatching { runArtDeferredFallback() }
                        .onFailure { Utils.logE("ART deferred bootstrap fallback failed", it) }
                },
                "Vector-ArtBootstrapFallback",
            )
            .apply {
                isDaemon = true
                start()
            }
    }

    private fun runNoArtDeferredBootstrap() {
        for (attempt in 0 until 200) {
            resolveDeferredAppState()?.let { state ->
                if (noArtDeferredDispatched.compareAndSet(false, true)) {
                    dispatchNoArtDeferred(state, attempt)
                }
                return
            }
            Thread.sleep(25L)
        }
        Utils.logW("No-ART deferred bootstrap timeout: app ClassLoader not ready")
    }

    private fun runArtDeferredFallback() {
        for (attempt in 0 until 200) {
            if (appAttachCompleted.get() || appPostAttachBootstrapCompleted.get()) {
                Utils.logI("ART deferred bootstrap fallback skipped: ActivityThread.attach completed")
                return
            }

            resolveDeferredAppState()?.let { state ->
                if (!artDeferredFallbackDispatched.compareAndSet(false, true)) return
                if (!appAttachCompleted.compareAndSet(false, true)) {
                    Utils.logI(
                        "ART deferred bootstrap fallback skipped for ${state.packageName}: " +
                            "ActivityThread.attach completed while fallback was dispatching"
                    )
                    return
                }
                dispatchArtDeferredFallback(state, attempt)
                return
            }
            Thread.sleep(25L)
        }

        if (!appAttachCompleted.get()) {
            Utils.logW(
                "ART deferred bootstrap fallback timeout: ActivityThread.attach did not complete " +
                    "and app ClassLoader was not ready"
            )
        }
    }

    private fun runAppAttachLifecycleReplay(reason: String) {
        for (attempt in 1 until 200) {
            resolveDeferredAppState()?.let { state ->
                dispatchAppAttachLifecycleReplay(state, reason, attempt)
                return
            }
            Thread.sleep(25L)
        }

        Utils.logW(
            "App attach lifecycle replay timeout: app ClassLoader not ready reason=$reason"
        )
    }

    private fun dispatchAppAttachLifecycleReplay(
        state: DeferredAppState,
        reason: String,
        attempt: Int,
    ) {
        if (!appAttachLifecycleReplayDispatched.compareAndSet(false, true)) return

        Utils.logW(
            "App attach lifecycle replay ready for ${state.packageName}/" +
                "${state.processName} after ${attempt * 25}ms reason=$reason; " +
                "replaying rawVector first-package lifecycle"
        )

        VectorBootstrap.withLegacy { delegate ->
            if (!delegate.isResourceHookingDisabled) {
                delegate.setPackageNameForResDir(state.packageName, state.resDir)
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            VectorLifecycleManager.dispatchPackageLoaded(
                state.packageName,
                state.appInfo,
                true,
                state.defaultClassLoader,
            )
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            VectorLifecycleManager.dispatchPackageReady(
                state.packageName,
                state.appInfo,
                true,
                state.defaultClassLoader,
                state.classLoader,
                state.appComponentFactory,
            )
        }

        VectorBootstrap.withLegacy { delegate ->
            delegate.onPackageLoaded(
                LegacyPackageInfo(
                    state.packageName,
                    state.processName,
                    state.classLoader,
                    state.appInfo,
                    true,
                )
            )
        }

        Utils.logI(
            "App attach lifecycle replay dispatched package lifecycle for " +
                "${state.packageName}/${state.processName}"
        )
    }

    private fun dispatchNoArtDeferred(state: DeferredAppState, attempt: Int) {
        Utils.logI(
            "No-ART deferred bootstrap ready for ${state.packageName}/" +
                "${state.processName} after ${attempt * 25}ms"
        )

        VectorBootstrap.withLegacy { delegate ->
            delegate.loadLegacyModules()
            if (!delegate.isResourceHookingDisabled) {
                delegate.setPackageNameForResDir(state.packageName, state.resDir)
            }
            delegate.loadModules(state.activityThread)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            VectorLifecycleManager.dispatchPackageLoaded(
                state.packageName,
                state.appInfo,
                true,
                state.defaultClassLoader,
            )
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            VectorLifecycleManager.dispatchPackageReady(
                state.packageName,
                state.appInfo,
                true,
                state.defaultClassLoader,
                state.classLoader,
                state.appComponentFactory,
            )
        }

        VectorBootstrap.withLegacy { delegate ->
            delegate.onPackageLoaded(
                LegacyPackageInfo(
                    state.packageName,
                    state.processName,
                    state.classLoader,
                    state.appInfo,
                    true,
                )
            )
        }
    }

    private fun dispatchArtDeferredFallback(state: DeferredAppState, attempt: Int) {
        Utils.logW(
            "ART deferred bootstrap fallback ready for ${state.packageName}/" +
                "${state.processName} after ${attempt * 25}ms; replaying rawVector first-package " +
                "module load and lifecycle"
        )

        // Keep rawVector's ART-controller ordering: the controller must be complete before
        // ActivityThread hooks are installed. Only module lifecycle dispatch is delayed below.
        if (!HookBridge.completeArtBootstrap()) {
            Utils.logE("WxShadow ART fallback controller initialization failed")
        }

        val dispatchLifecycle = {
            // DexFile/ClassLinker hooks can take seconds under WxShadow. Keep them out of the
            // first ActivityThread launch transaction; the delayed callback runs after the first
            // UI turn and still precedes module lifecycle publication.
            completeAppBootstrapAfterAttach()

            VectorBootstrap.withLegacy { delegate ->
                if (!delegate.isResourceHookingDisabled) {
                    delegate.setPackageNameForResDir(state.packageName, state.resDir)
                }
                delegate.loadModules(state.activityThread)
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                VectorLifecycleManager.dispatchPackageLoaded(
                    state.packageName,
                    state.appInfo,
                    true,
                    state.defaultClassLoader,
                )
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                VectorLifecycleManager.dispatchPackageReady(
                    state.packageName,
                    state.appInfo,
                    true,
                    state.defaultClassLoader,
                    state.classLoader,
                    state.appComponentFactory,
                )
            }

            VectorBootstrap.withLegacy { delegate ->
                delegate.onPackageLoaded(
                    LegacyPackageInfo(
                        state.packageName,
                        state.processName,
                        state.classLoader,
                        state.appInfo,
                        true,
                    )
                )
            }

            Utils.logI(
                "ART deferred bootstrap fallback dispatched package lifecycle for " +
                    "${state.packageName}/${state.processName}"
            )
        }

        if (HookBridge.isWxShadowStealthMode()) {
            // 本函数仅在 ActivityThread.attach hook 错过 rawVector 边界时兜底（boot oat 重编译
            // 等环境原因；main deopt 恢复后不应再出现）。一旦走到这里，越早派发越好：等待首
            // 帧或固定 750ms 会把模块生命周期永远推到应用代码之后（应用在 bindApplication 里
            // 就完成 JNI 初始化），违反 rawVector 契约。投递到主线程消息队列以与
            // bindApplication 串行（mPackages 等非线程安全状态不能并发访问）；主线程当期消息
            // 处理完后立即执行，不额外等待。exactly-once 由 AtomicBoolean 保证。
            val dispatched = AtomicBoolean(false)
            Handler(Looper.getMainLooper()).post {
                if (dispatched.compareAndSet(false, true)) {
                    dispatchLifecycle()
                }
            }
            Utils.logI(
                "WxShadow deferred module lifecycle queued on main looper " +
                    "(attach-boundary replay; dispatch as soon as the current message completes)"
            )
        } else {
            dispatchLifecycle()
        }
    }

    private fun resolveDeferredAppState(): DeferredAppState? {
        val activityThread = ActivityThread.currentActivityThread() ?: return null
        val boundApplication = activityThread.getFieldValue<Any>("mBoundApplication")
        val loadedApk = boundApplication?.getFieldValue<Any>("info")

        val appInfo =
            boundApplication?.getFieldValue<ApplicationInfo>("appInfo")
                ?: loadedApk?.getFieldValue<ApplicationInfo>("mApplicationInfo")
                ?: ActivityThread.currentApplication()?.applicationInfo
                ?: return null

        val classLoader =
            loadedApk?.getFieldValue<ClassLoader>("mClassLoader")
                ?: ActivityThread.currentApplication()?.classLoader
                ?: return null

        // mDefaultClassLoader 在 Android 12 MIUI 是 hidden API（blocked+denied），
        // 反射访问每条都会打印 "Accessing hidden field LoadedApk;->mDefaultClassLoader
        // (blocked, reflection, denied)" 并返回 null（实测 xff denylist 进程 200 次循环
        // 刷屏 100+ 条）。该字段在 API 29+ 已 deprecated 且语义等同 mClassLoader，
        // 直接复用 mClassLoader，避免无意义的 denied 反射风暴。
        val defaultClassLoader = classLoader
        val appComponentFactory =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                loadedApk?.getFieldValue<Any>("mAppComponentFactory")
            } else {
                null
            }

        val packageName =
            runCatching { ActivityThread.currentPackageName() }.getOrNull()
                ?: appInfo.packageName
        val processName =
            runCatching { ActivityThread.currentProcessName() }.getOrNull()
                ?: packageName
        val resDir = loadedApk?.getFieldValue<String>("mResDir")

        return DeferredAppState(
            activityThread,
            packageName,
            processName,
            appInfo,
            classLoader,
            defaultClassLoader,
            appComponentFactory,
            resDir,
        )
    }
}

private inline fun <reified T> Any.getFieldValue(name: String): T? {
    var clazz: Class<*>? = this.javaClass
    while (clazz != null) {
        try {
            val field = clazz.getDeclaredField(name)
            field.isAccessible = true
            return field.get(this) as? T
        } catch (_: NoSuchFieldException) {
            clazz = clazz.superclass
        } catch (_: Throwable) {
            return null
        }
    }
    return null
}
