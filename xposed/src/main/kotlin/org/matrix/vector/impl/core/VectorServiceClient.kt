package org.matrix.vector.impl.core

import android.os.IBinder
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.Process
import org.matrix.vector.ipc.LoadedModule
import org.matrix.vector.ipc.IProcessChannel
import org.matrix.vector.ipc.IFrameworkService
import org.matrix.vector.util.Log

/**
 * Singleton client for managing IPC communication with the injected manager service. Handles Binder
 * death gracefully and ensures safe remote execution.
 */
object VectorServiceClient : IFrameworkService, IBinder.DeathRecipient {

    private const val TAG = "VectorServiceClient"

    private fun elapsedUs(startNs: Long): Long = (System.nanoTime() - startNs) / 1_000L

    private var service: IFrameworkService? = null
    private val legacyModulesLock = Any()

    @Volatile private var prefetchedLegacyModules: List<LoadedModule>? = null

    var processName: String = ""
        private set

    @Synchronized
    fun init(appService: IFrameworkService?, niceName: String) {
        val binder = appService?.asBinder()
        if (service == null && binder != null) {
            runCatching {
                    service = appService
                    processName = niceName
                    synchronized(legacyModulesLock) { prefetchedLegacyModules = null }
                    binder.linkToDeath(this, 0)
                }
                .onFailure {
                    Log.e(TAG, "Failed to link to death for service in process: $niceName", it)
                    service = null
                }

            // Handed over here rather than after module loading, and carrying no module identity:
            // system_server loads its modules before the daemon's module cache exists, so anything
            // that had to name a module here could not work for it.
            service?.let {
                try {
                    it.attachProcessChannel(VectorProcessChannel)
                } catch (t: Throwable) {
                    Log.e(TAG, "Failed to attach the process channel in process: $niceName", t)
                }
            }
        }
    }

    /**
     * Fetches legacy module metadata before WxShadow changes the app's ART execution state. The
     * result is consumed later by the normal rawVector-aligned bootstrap path; module code is not
     * loaded early.
     */
    fun prefetchLegacyModules() {
        synchronized(legacyModulesLock) {
            if (prefetchedLegacyModules != null) return
        }

        val modules = queryLegacyModules("prefetch") ?: return
        synchronized(legacyModulesLock) {
            if (prefetchedLegacyModules == null) {
                prefetchedLegacyModules = modules
            }
        }
    }

    override fun attachProcessChannel(channel: IProcessChannel?) {
        try {
            service?.attachProcessChannel(channel)
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to attach the process channel", t)
        }
    }

    override fun isLogMuted(): Boolean {
        return runCatching { service?.isLogMuted == true }.getOrDefault(false)
    }

    override fun getLegacyModules(): List<LoadedModule> {
        synchronized(legacyModulesLock) {
            prefetchedLegacyModules?.let { modules ->
                prefetchedLegacyModules = null
                Log.i(
                    TAG,
                    "[startup-trace] getLegacyModules cache hit process=$processName " +
                        "count=${modules.size}",
                )
                return modules
            }
        }

        return queryLegacyModules("bootstrap") ?: emptyList()
    }

    private fun queryLegacyModules(stage: String): List<LoadedModule>? {
        val startNs = System.nanoTime()
        val currentService = service
        if (currentService == null) {
            Log.w(
                TAG,
                "[startup-trace] getLegacyModules client=null stage=$stage " +
                    "process=$processName elapsedUs=${elapsedUs(startNs)}",
            )
            return null
        }

        val thread = Thread.currentThread()
        val isMainThread = Looper.myLooper() === Looper.getMainLooper()
        Log.i(
            TAG,
            "[startup-trace] getLegacyModules binder begin stage=$stage process=$processName " +
                "thread=${thread.name} tid=${Process.myTid()} main=$isMainThread",
        )
        return try {
            val modules = currentService.legacyModules
            Log.i(
                TAG,
                "[startup-trace] getLegacyModules binder end stage=$stage process=$processName " +
                    "count=${modules.size} thread=${thread.name} tid=${Process.myTid()} " +
                    "main=$isMainThread elapsedUs=${elapsedUs(startNs)}",
            )
            modules.toList()
        } catch (t: Throwable) {
            Log.e(
                TAG,
                "[startup-trace] getLegacyModules binder failed stage=$stage process=$processName " +
                    "elapsedUs=${elapsedUs(startNs)}",
                t,
            )
            null
        }
    }

    override fun getModules(): List<LoadedModule> {
        return runCatching { service?.modules }.getOrNull() ?: emptyList()
    }

    override fun getPrefsPath(packageName: String): String? {
        return runCatching { service?.getPrefsPath(packageName) }.getOrNull()
    }

    override fun openManagerApk(): ParcelFileDescriptor? {
        return runCatching { service?.openManagerApk() }.getOrNull()
    }

    override fun requestManagerService(): IBinder? {
        return runCatching { service?.requestManagerService() }.getOrNull()
    }

    override fun asBinder(): IBinder? {
        return service?.asBinder()
    }

    override fun binderDied() {
        service?.asBinder()?.unlinkToDeath(this, 0)
        service = null
        synchronized(legacyModulesLock) { prefetchedLegacyModules = null }
    }
}
