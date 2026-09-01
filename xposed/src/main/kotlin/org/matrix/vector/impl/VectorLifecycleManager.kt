package org.matrix.vector.impl

import android.content.pm.ApplicationInfo
import android.os.Build
import androidx.annotation.RequiresApi
import de.wwb.android.wbosed.api.WbModule
import de.wwb.android.wbosed.api.WbModuleInterface.*
import java.util.concurrent.ConcurrentHashMap
import org.matrix.vector.util.Log

/** Manages the dispatching of modern lifecycle events to loaded modules. */
object VectorLifecycleManager {

    private const val TAG = "VectorLifecycle"

    // The framework's only strong reference to entry instances, and what detach() removes. Any
    // dispatch added later, hot reload included, must iterate this rather than keep its own list.
    val activeModules: MutableSet<WbModule> = ConcurrentHashMap.newKeySet()
    private val loadedPackages: MutableSet<String> = ConcurrentHashMap.newKeySet()
    private val readyPackages: MutableSet<String> = ConcurrentHashMap.newKeySet()

    fun detach(module: WbModule) {
        if (activeModules.remove(module)) {
            Log.d(TAG, "Detached entry ${module.javaClass.name}")
        }
    }

    fun isActive(module: WbModule): Boolean = activeModules.contains(module)

    /**
     * The API declares `onPackageLoaded` as API 29 and up, so this carries the same requirement
     * rather than leaving the callers to remember it. `LoadedApkCreateAppFactoryHooker` is the only
     * one, and it already dispatches from inside a Q check.
     */
    @RequiresApi(Build.VERSION_CODES.Q)
    fun dispatchPackageLoaded(
        packageName: String,
        appInfo: ApplicationInfo,
        isFirst: Boolean,
        defaultClassLoader: ClassLoader,
    ) {
        if (!loadedPackages.add(packageName)) {
            Log.d(TAG, "Skip duplicate onPackageLoaded for $packageName")
            return
        }
        val param =
            object : PackageLoadedParam {
                override fun getPackageName(): String = packageName

                override fun getApplicationInfo(): ApplicationInfo = appInfo

                override fun isFirstPackage(): Boolean = isFirst

                override fun getDefaultClassLoader(): ClassLoader = defaultClassLoader
            }

        activeModules.forEach { module ->
            runCatching { module.onPackageLoaded(param) }
                .onFailure {
                    Log.e(
                        TAG,
                        "Error in onPackageLoaded for ${module.moduleApplicationInfo.packageName}",
                        it,
                    )
                }
        }
    }

    fun dispatchPackageReady(
        packageName: String,
        appInfo: ApplicationInfo,
        isFirst: Boolean,
        defaultClassLoader: ClassLoader,
        classLoader: ClassLoader,
        appComponentFactory: Any?, // Abstracted for API compatibility
    ) {
        if (!readyPackages.add(packageName)) {
            Log.d(TAG, "Skip duplicate onPackageReady for $packageName")
            return
        }
        val param =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && appComponentFactory != null) {
                PackageReadyParamImplP(
                    packageName,
                    appInfo,
                    isFirst,
                    defaultClassLoader,
                    classLoader,
                    appComponentFactory,
                )
            } else {
                // Fallback for API < 28 (or if factory is null).
                object : PackageReadyParam {
                    override fun getPackageName() = packageName

                    override fun getApplicationInfo() = appInfo

                    override fun isFirstPackage() = isFirst

                    override fun getDefaultClassLoader() = defaultClassLoader

                    override fun getClassLoader() = classLoader

                    override fun getAppComponentFactory(): android.app.AppComponentFactory {
                        throw UnsupportedOperationException(
                            "AppComponentFactory is not available on this API level"
                        )
                    }
                }
            }

        Log.i(TAG, "packageReady begin package=$packageName first=$isFirst")
        activeModules.forEach { module ->
            runCatching {
                    Log.i(TAG, "packageReady callback begin package=$packageName module=${module.javaClass.name}")
                    module.onPackageReady(param)
                    Log.i(TAG, "packageReady callback end package=$packageName module=${module.javaClass.name}")
                }
                .onFailure {
                    Log.e(
                        TAG,
                        "Error in onPackageReady for ${module.moduleApplicationInfo.packageName}",
                        it,
                    )
                }
        }
        Log.i(TAG, "packageReady end package=$packageName")
    }

    fun dispatchSystemServerStarting(classLoader: ClassLoader) {
        val param =
            object : SystemServerStartingParam {
                override fun getClassLoader(): ClassLoader = classLoader
            }

        activeModules.forEach { module ->
            runCatching { module.onSystemServerStarting(param) }
                .onFailure {
                    Log.e(
                        TAG,
                        "Error in onSystemServerStarting for ${module.moduleApplicationInfo.packageName}",
                        it,
                    )
                }
        }
    }
}

// Isolate the class so the Verifier doesn't crash on Android 8.1 and below
@RequiresApi(Build.VERSION_CODES.P)
private class PackageReadyParamImplP(
    private val packageName: String,
    private val appInfo: ApplicationInfo,
    private val isFirst: Boolean,
    private val defaultClassLoader: ClassLoader,
    private val classLoader: ClassLoader,
    private val appComponentFactory: Any,
) : PackageReadyParam {
    override fun getPackageName() = packageName

    override fun getApplicationInfo() = appInfo

    override fun isFirstPackage() = isFirst

    override fun getDefaultClassLoader() = defaultClassLoader

    override fun getClassLoader() = classLoader

    override fun getAppComponentFactory(): android.app.AppComponentFactory {
        return appComponentFactory as android.app.AppComponentFactory
    }
}
