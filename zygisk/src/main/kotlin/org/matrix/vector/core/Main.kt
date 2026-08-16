package org.matrix.vector.core

import android.os.IBinder
import android.os.Process
import org.matrix.vector.ipc.IFrameworkService
import org.matrix.vector.util.Log
import org.matrix.vector.util.Utils
import org.matrix.vector.BuildConfig
import org.matrix.vector.GrapheneDclHooker
import org.matrix.vector.ParasiticManagerHooker
import org.matrix.vector.ParasiticManagerSystemHooker
import org.matrix.vector.Startup
import org.matrix.vector.impl.core.VectorServiceClient
import org.matrix.vector.impl.hookers.HandleSystemServerProcessHooker
import java.util.concurrent.atomic.AtomicBoolean

/** Main entry point for the Java-side loader, invoked via JNI from the Vector Zygisk module. */
object Main {

    private val systemServerHookRetryStarted = AtomicBoolean(false)

    private fun canLoad(classLoader: ClassLoader, className: String): Boolean =
        runCatching {
                Class.forName(className, false, classLoader)
                true
            }
            .getOrDefault(false)

    private fun findSystemServerClassLoader(): ClassLoader? {
        val candidates = linkedSetOf<ClassLoader>()
        Thread.currentThread().contextClassLoader?.let { candidates.add(it) }
        Thread.getAllStackTraces().keys.forEach { thread ->
            thread.contextClassLoader?.let { candidates.add(it) }
        }
        ClassLoader.getSystemClassLoader()?.let { candidates.add(it) }
        runCatching { android.app.ActivityThread.currentActivityThread()?.systemContext?.classLoader }
            .getOrNull()
            ?.let { candidates.add(it) }
        runCatching { android.os.ServiceManager.getService("activity")?.javaClass?.classLoader }
            .getOrNull()
            ?.let { candidates.add(it) }

        return candidates.firstOrNull { classLoader ->
            canLoad(classLoader, "com.android.server.SystemServer") ||
                canLoad(classLoader, "com.android.server.wm.ActivityTaskSupervisor") ||
                canLoad(classLoader, "com.android.server.wm.ActivityStackSupervisor") ||
                canLoad(classLoader, "com.android.server.am.ActivityStackSupervisor")
        }
    }

    private fun scheduleSystemServerHookRetry(isLateInject: Boolean) {
        if (!systemServerHookRetryStarted.compareAndSet(false, true)) return

        Thread {
                for (attempt in 1..120) {
                    val classLoader = findSystemServerClassLoader()
                    if (classLoader != null) {
                        runCatching {
                                HandleSystemServerProcessHooker.initSystemServer(
                                    classLoader,
                                    isLate = isLateInject,
                                )
                                Utils.logD(
                                    "Deferred system_server hook initialization completed " +
                                        "(attempt=$attempt, late=$isLateInject, " +
                                        "classLoader=${classLoader.javaClass.name})"
                                )
                            }
                            .onFailure {
                                Utils.logE(
                                    "Deferred system_server hook initialization failed " +
                                        "(attempt=$attempt)",
                                    it,
                                )
                            }
                        return@Thread
                    }

                    runCatching { Thread.sleep(500) }
                }

                Utils.logW("Timed out waiting for system_server ClassLoader for manager redirection")
            }
            .apply {
                name = "VectorSystemServerHookInit"
                isDaemon = true
                start()
            }
    }

    private fun initSystemServerHooksEagerly(isLateInject: Boolean) {
        val classLoader =
            findSystemServerClassLoader()
                ?: run {
                    Utils.logW("Unable to find system_server ClassLoader for manager redirection")
                    scheduleSystemServerHookRetry(isLateInject)
                    return
                }
        runCatching {
                HandleSystemServerProcessHooker.initSystemServer(
                    classLoader,
                    isLate = isLateInject,
                )
                Utils.logD(
                    "Eager system_server hook initialization completed " +
                        "(late=$isLateInject, classLoader=${classLoader.javaClass.name})"
                )
            }
            .onFailure { Utils.logE("Eager system_server hook initialization failed", it) }
    }

    /**
     * Shared initialization logic for both System Server and Application processes.
     *
     * @param isSystem True if this is the system_server process.
     * @param isLateInject True if Zygisk APIs are not invoked via hooks
     * @param niceName The process name (e.g., package name or "system").
     * @param appDir The application's data directory.
     * @param binder The Binder token associated with the application service.
     */
    @JvmStatic
    fun forkCommon(
        isSystem: Boolean,
        isLateInject: Boolean,
        niceName: String,
        appDir: String?,
        binder: IBinder,
        artBootstrap: Boolean,
    ) {
        // Initialize system-specific resolution hooks if in system_server
        if (isSystem) {
            ParasiticManagerSystemHooker.start()
            // Exempt the manager host from GrapheneOS DCL restriction; no-op on other systems.
            GrapheneDclHooker.start()
        }

        // Initialize Xposed bridge components
        val appService = IFrameworkService.Stub.asInterface(binder)
        Startup.initXposed(isSystem, niceName, appDir, appService)

        // Configure logging levels from the service client
        runCatching { Log.muted = VectorServiceClient.isLogMuted }
            .onFailure { t -> Utils.logE("Failed to configure logs from service", t) }

        if (isSystem) {
            initSystemServerHooksEagerly(isLateInject)
        }

        // Check if this process is the designated Vector Manager.
        if (niceName == BuildConfig.ManagerPackageName) {
            ParasiticManagerHooker.isParasitic = Process.myUid() == BuildConfig.HostPackageUid
            val type = if (ParasiticManagerHooker.isParasitic) "parasitic" else "user-installed"
            if (ParasiticManagerHooker.start()) {
                Utils.logI("Manager ($type) loaded into host, skipping standard bootstrap.")
                return
            }
        }

        // Standard Xposed module loading for third-party apps
        Utils.logV("Loading Vector/Xposed for $niceName (UID: ${Process.myUid()})")
        if (artBootstrap) {
            Startup.bootstrapXposed(isSystem && isLateInject)
        } else {
            Startup.bootstrapXposedNoArt()
        }
    }
}
