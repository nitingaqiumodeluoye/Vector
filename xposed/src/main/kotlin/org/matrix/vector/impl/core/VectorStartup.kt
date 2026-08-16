package org.matrix.vector.impl.core

import android.app.ActivityThread
import android.content.pm.ApplicationInfo
import android.os.Build
import android.os.IBinder
import android.os.Process
import dalvik.system.DexFile
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import org.matrix.vector.impl.VectorLifecycleManager
import org.matrix.vector.impl.di.LegacyPackageInfo
import org.matrix.vector.util.Utils
import org.matrix.vector.ipc.IFrameworkService
import org.matrix.vector.impl.di.VectorBootstrap
import org.matrix.vector.impl.hookers.*
import org.matrix.vector.impl.hooks.VectorHookBuilder

/**
 * Modern framework initialization and bootstrap sequence. Deploys interceptors into the ART runtime
 * and handles early process deoptimization.
 */
object VectorStartup {

    private val noArtDeferredStarted = AtomicBoolean(false)
    private val noArtDeferredDispatched = AtomicBoolean(false)

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

    @JvmStatic
    fun bootstrap(isSystem: Boolean, systemServerStarted: Boolean) {
        // Crash Dump Interceptor
        Thread::class
            .java
            .declaredMethods
            .firstOrNull { it.name == "dispatchUncaughtException" }
            ?.let { VectorHookBuilder(it).intercept(CrashDumpHooker) }

        // Process-specific Interceptors
        if (isSystem) {
            val zygoteInitClass = Class.forName("com.android.internal.os.ZygoteInit")
            zygoteInitClass.declaredMethods
                .filter { it.name == "handleSystemServerProcess" }
                .forEach { VectorHookBuilder(it).intercept(HandleSystemServerProcessHooker) }
        } else {
            DexFile::class
                .java
                .declaredMethods
                .filter {
                    it.name == "openDexFile" ||
                        it.name == "openInMemoryDexFile" ||
                        it.name == "openInMemoryDexFiles"
                }
                .forEach { VectorHookBuilder(it).intercept(DexTrustHooker) }
        }

        // Application Load Interceptors
        val loadedApkClass = Class.forName("android.app.LoadedApk")
        loadedApkClass.declaredConstructors.forEach {
            // Hook all constructors of LoadedApk to catch early instantiations securely
            VectorHookBuilder(it).intercept(LoadedApkCtorHooker)
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            loadedApkClass.declaredMethods
                .filter { it.name == "createAppFactory" }
                .forEach { VectorHookBuilder(it).intercept(LoadedApkCreateAppFactoryHooker) }
        }

        loadedApkClass.declaredMethods
            .filter { it.name == "createOrUpdateClassLoaderLocked" }
            .forEach { VectorHookBuilder(it).intercept(LoadedApkCreateCLHooker) }

        // ActivityThread Attachment Interceptor
        val activityThreadClass = Class.forName("android.app.ActivityThread")
        activityThreadClass.declaredMethods
            .filter { it.name == "attach" }
            .forEach { VectorHookBuilder(it).intercept(AppAttachHooker) }

        // Late System Server Injection
        if (systemServerStarted) {
            val activityService: IBinder? = android.os.ServiceManager.getService("activity")
            if (activityService != null) {
                val classLoader = activityService.javaClass.classLoader
                if (classLoader != null) {
                    // Manually trigger the routines that the hooks normally would
                    HandleSystemServerProcessHooker.initSystemServer(classLoader, isLate = true)

                    // ActivityThread.attach ran before we were injected, so AppAttachHooker will
                    // never fire here and nothing else instantiates modules. Without this,
                    // dispatchSystemServerLoaded below would notify an empty module set.
                    val activityThread = ActivityThread.currentActivityThread()
                    if (activityThread != null) {
                        VectorBootstrap.withLegacy { it.loadModules(activityThread) }
                    } else {
                        Utils.logW(
                            "Late system server injection: no current ActivityThread, modules cannot be loaded"
                        )
                    }

                    // Say plainly what this is. onSystemServerStarting is documented as "system
                    // server is ready to start critical services", which is no longer true here -
                    // the services are already running. Modules that key off it should treat a
                    // late dispatch as best effort.
                    Utils.logW(
                        "Late system server injection: dispatching onSystemServerStarting after " +
                            "system server has already started; critical services are live"
                    )
                    StartBootstrapServicesHooker.dispatchSystemServerLoaded(classLoader)
                }
            }
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
