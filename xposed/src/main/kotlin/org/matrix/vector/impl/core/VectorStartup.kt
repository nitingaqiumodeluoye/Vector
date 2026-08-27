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

    private const val WXSHADOW_MODULE_LOAD_COOLDOWN_MS = 750L

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

    private fun traceAnonymousExecMaps(stage: String) {
        runCatching {
                val mappings =
                    File("/proc/self/maps").useLines { lines ->
                        lines.filter { it.contains(" r-xp 00000000 00:00 0") }.toList()
                    }
                Utils.logI(
                    "[exec-map-bootstrap] stage=$stage pid=${Process.myPid()} " +
                        "count=${mappings.size}"
                )
                mappings.forEach { Utils.logI("[exec-map-bootstrap] stage=$stage map=$it") }
            }
            .onFailure { Utils.logE("[exec-map-bootstrap] stage=$stage failed", it) }
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

    private fun describeExecutable(executable: Executable): String =
        buildString {
            append(executable.declaringClass.name)
            append('#')
            append(executable.name)
            append('(')
            append(executable.parameterTypes.joinToString(",") { it.name })
            append(')')
        }

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
        val target = describeExecutable(executable)
        val startNs = System.nanoTime()
        Utils.logI("[startup-trace] hook begin: label=$label target=$target")
        try {
            block()
            Utils.logI(
                "[startup-trace] hook end: label=$label target=$target " +
                    "elapsedUs=${elapsedUs(startNs)}"
            )
        } catch (t: Throwable) {
            Utils.logE(
                "[startup-trace] hook failed: label=$label target=$target " +
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
    }

    /**
     * Registers a pre-built [LoadedApk] whose constructor ran before the hooks were installed -- the
     * LSPatch rootless target being the one case. Once tracked, [LoadedApkCreateAppFactoryHooker] and
     * [LoadedApkCreateCLHooker] dispatch its `onPackageLoaded`/`onPackageReady` (and legacy
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
            loadedApkConstructors.forEachIndexed { index, constructor ->
                // Hook all constructors of LoadedApk to catch early instantiations securely
                traceStartupHook("LoadedApkCtorHooker[$phase][$index]", constructor) {
                    VectorHookBuilder(constructor).intercept(LoadedApkCtorHooker)
                }
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
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

            val createClassLoaderMethods =
                traceStartupStep(
                    "reflect.LoadedApk.createOrUpdateClassLoaderLocked[$phase]"
                ) {
                    loadedApkClass.declaredMethods
                        .filter { it.name == "createOrUpdateClassLoaderLocked" }
                }
            createClassLoaderMethods.forEachIndexed { index, method ->
                traceStartupHook("LoadedApkCreateCLHooker[$phase][$index]", method) {
                    VectorHookBuilder(method).intercept(LoadedApkCreateCLHooker)
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
        installPostAttachFrameworkInterceptors()
        Utils.logI("[startup-trace] Vector post-attach framework bootstrap completed")
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
        traceAnonymousExecMaps("thread-start")

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

        traceAnonymousExecMaps("app-attach-lifecycle-replay-before-dispatch")
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
        traceAnonymousExecMaps("classloader-ready-before-dispatch")
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
        traceAnonymousExecMaps("art-fallback-classloader-ready-before-dispatch")
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
            // WxShadow keeps the ART controller lean around startup; the real thing we wait for
            // is ClassLinker/GC leaving the controller-install window, i.e. the first UI turn.
            // The source comment says the deferred callback "runs after the first UI turn", so
            // drive the dispatch from that signal instead of a blind fixed cooldown: dispatch as
            // soon as the first UI frame renders (ART has already run a full UI turn by then), and
            // keep WXSHADOW_MODULE_LOAD_COOLDOWN_MS only as a hard cap for processes that never
            // render a frame (background/service processes without an Activity). Whichever fires
            // first wins; the AtomicBoolean guarantees dispatchLifecycle runs exactly once.
            val dispatched = AtomicBoolean(false)
            val defer = Runnable {
                if (dispatched.compareAndSet(false, true)) {
                    dispatchLifecycle()
                }
            }

            val mainHandler = Handler(Looper.getMainLooper())
            val firstFramePosted = runCatching {
                Choreographer.getInstance().postFrameCallback { _ ->
                    Utils.logI(
                        "WxShadow module lifecycle triggered early on first UI frame " +
                            "(before the ${WXSHADOW_MODULE_LOAD_COOLDOWN_MS}ms cap)"
                    )
                    mainHandler.post(defer)
                }
            }
            if (firstFramePosted.isFailure) {
                Utils.logE(
                    "WxShadow Choreographer first-frame dispatch unavailable",
                    firstFramePosted.exceptionOrNull(),
                )
            }

            // Hard cap: a process with no renderable Activity (e.g. a :remote/service process)
            // never produces a first frame, so fall back to the fixed cooldown.
            mainHandler.postDelayed(defer, WXSHADOW_MODULE_LOAD_COOLDOWN_MS)
            Utils.logI(
                "WxShadow deferred module lifecycle by <=${WXSHADOW_MODULE_LOAD_COOLDOWN_MS}ms " +
                    "(signal-driven: first UI frame, hard cap)"
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
                ?: loadedApk?.getFieldValue<ClassLoader>("mDefaultClassLoader")
                ?: ActivityThread.currentApplication()?.classLoader
                ?: return null

        val defaultClassLoader =
            loadedApk?.getFieldValue<ClassLoader>("mDefaultClassLoader") ?: classLoader
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
