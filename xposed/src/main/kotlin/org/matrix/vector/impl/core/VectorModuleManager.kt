package org.matrix.vector.impl.core

import android.os.Build
import android.os.Bundle
import android.os.Process
import android.os.SharedMemory
import de.wwb.android.wbosed.api.WbInterface
import de.wwb.android.wbosed.api.WbInterface.ExceptionMode
import de.wwb.android.wbosed.api.WbModule
import de.wwb.android.wbosed.api.WbModuleInterface.HotReloadedParam
import de.wwb.android.wbosed.api.WbModuleInterface.HotReloadingParam
import de.wwb.android.wbosed.api.WbModuleInterface.ModuleLoadedParam
import de.wwb.android.wbosed.service.IWbService
import java.io.File
import java.lang.ref.WeakReference
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.locks.ReentrantLock
import org.matrix.vector.ipc.HotReloadOutcome
import org.matrix.vector.ipc.LoadedModule
import org.matrix.vector.util.Log
import org.matrix.vector.impl.VectorContext
import org.matrix.vector.impl.VectorLifecycleManager
import org.matrix.vector.impl.hooks.VectorHookBuilder
import org.matrix.vector.impl.utils.VectorModuleClassLoader
import org.matrix.vector.nativebridge.HookBridge
import org.matrix.vector.nativebridge.NativeAPI

/**
 * Responsible for loading modules into the target process. Handles ClassLoader isolation and
 * injects the framework context into the module instances.
 */
object VectorModuleManager {

    private const val TAG = "VectorModuleManager"
    // 2026-09-11 用户通知恢复 soLoader：模块 native ELF 由 daemon sealed SharedMemory +
    // NativeAPI.loadNativeLibraries() 走 ElfLoader::LoadLibraryFd() 自装链接。linker 直载分支
    // （true）仅作诊断保留；xff T2 已验证 linker 路径会留 base.apk MAPS_FOREIGN 痕迹。
    private const val USE_RAW_VECTOR_LINKER_NATIVE_LOADER = false

    private fun elapsedUs(startNs: Long): Long = (System.nanoTime() - startNs) / 1_000L

    private inline fun <T> traceStartupStep(
        label: String,
        modulePackageName: String,
        block: () -> T,
    ): T {
        val startNs = System.nanoTime()
        Log.i(
            TAG,
            "[startup-trace] module step begin label=$label module=$modulePackageName",
        )
        return try {
            val result = block()
            Log.i(
                TAG,
                "[startup-trace] module step end label=$label module=$modulePackageName " +
                    "elapsedUs=${elapsedUs(startNs)}",
            )
            result
        } catch (t: Throwable) {
            Log.e(
                TAG,
                "[startup-trace] module step failed label=$label module=$modulePackageName " +
                    "elapsedUs=${elapsedUs(startNs)}",
                t,
            )
            throw t
        }
    }

    private fun magicOf(buffer: java.nio.ByteBuffer): String =
        if (buffer.capacity() >= 4) {
            "%02x %02x %02x %02x"
                .format(
                    buffer.get(0).toInt() and 0xff,
                    buffer.get(1).toInt() and 0xff,
                    buffer.get(2).toInt() and 0xff,
                    buffer.get(3).toInt() and 0xff,
                )
        } else {
            "<too-small:${buffer.capacity()}>"
        }

    private fun traceNativeSharedMemory(modulePackageName: String, libraryName: String, memory: SharedMemory) {
        runCatching {
                val buffer = memory.mapReadOnly()
                try {
                    Log.i(
                        TAG,
                        "Received native SharedMemory $modulePackageName/$libraryName " +
                            "size=${buffer.capacity()} magic=${magicOf(buffer)}",
                    )
                } finally {
                    SharedMemory.unmap(buffer)
                }
            }
            .onFailure { error ->
                Log.e(
                    TAG,
                    "Cannot inspect native SharedMemory $modulePackageName/$libraryName",
                    error,
                )
            }
    }

    private fun traceAnonymousExecMaps(stage: String, modulePackageName: String) {
        // Diagnostic-only: reading /proc/self/maps fires a page-table walk + takes the process
        // mmap semaphore on every call. This was invoked up to 14x per module load on the fork/
        // pre-attach hot path, and on a cold VM (fresh TLB/icache + first-touch shadow pages) it
        // degenerates into a multi-second fault storm (perf: handle_pte_fault +
        // do_el0_ia_bp_hardening + seq_path + mmap_lock contention). Disabled by default.
        return
    }

    // Entries are weak on purpose: activeModules owns the only strong reference, and detach()
    // removes it. A reload holds a local strong list for the cycle instead.
    private class Generation(
        val classLoader: ClassLoader,
        val context: VectorContext,
        entries: List<WbModule>,
        val isSystemServer: Boolean,
        val processName: String,
    ) {
        private val entryRefs = entries.map { WeakReference(it) }

        fun liveEntries(): List<WbModule> =
            entryRefs.mapNotNull { it.get() }.filter { VectorLifecycleManager.isActive(it) }
    }

    private val generations = ConcurrentHashMap<String, Generation>()

    // Reloads are serialized per module within this process; the daemon serializes per target.
    private val reloadLocks = ConcurrentHashMap<String, ReentrantLock>()

    /** The class loader a loaded module runs in, or null if no generation is loaded for it. */
    fun getModuleClassLoader(packageName: String): ClassLoader? = generations[packageName]?.classLoader

    /** Packages of the modules currently loaded in this process. */
    fun loadedModulePackages(): Set<String> = generations.keys.toSet()

    /**
     * Loads a module APK, instantiates its entry classes, and binds them to the Vector framework.
     */
    fun loadModule(module: LoadedModule, isSystemServer: Boolean, processName: String): Boolean =
        traceStartupStep("loadModule", module.packageName) {
        if (USE_RAW_VECTOR_LINKER_NATIVE_LOADER) {
            if (module.code.moduleLibraryNames.isNotEmpty()) {
                Log.w(
                    TAG,
                    "rawVector linker native loader enabled for ${module.packageName}; " +
                        "skipping Vector soLoader SharedMemory load.",
                )
            }
            module.code.preLoadedNativeLibraries.forEach { memory ->
                runCatching { memory.close() }
            }
        }
        // NOTE: the soLoader native load moved into buildGeneration(), after loadApk() has created
        // the module ClassLoader. See loadModuleNativeLibraries() for the rawVector timing reason.

        traceAnonymousExecMaps("renderModule:before-build", module.packageName)
        val (generation, entries) =
            buildGeneration(module, isSystemServer, processName) ?: return false
        traceAnonymousExecMaps("renderModule:after-build", module.packageName)

        entries.forEach { VectorLifecycleManager.activeModules.add(it) }
        generations[module.packageName] = generation

        val param =
            object : ModuleLoadedParam {
                override fun isSystemServer(): Boolean = isSystemServer

                override fun getProcessName(): String = processName
            }
        entries.forEach { entry ->
            traceAnonymousExecMaps("onModuleLoaded:before:${entry.javaClass.name}", module.packageName)
            runCatching { entry.onModuleLoaded(param) }
                .onFailure { e ->
                    Log.e(TAG, "Error in onModuleLoaded for ${entry.javaClass.name}", e)
                }
            traceAnonymousExecMaps("onModuleLoaded:after:${entry.javaClass.name}", module.packageName)
        }

        Log.d(TAG, "Rendered module ${module.packageName} successfully.")
        true
    }

    /**
     * Loads a module's native ELF images through Vector's soLoader. The images' `native_init`
     * entrypoints are NOT invoked here: they are deferred and fired later via
     * [NativeAPI.firePendingNativeInits] from a module-class context, because under rawVector
     * native_init resolved the module's own Java classes through the module ClassLoader (it ran
     * on the module's own System.loadLibrary stack), while a direct call from this framework
     * frame would make ART's FindClass resolve the framework.
     *
     * Returns false when a library fails to load, which fails the whole generation.
     */
    private fun loadModuleNativeLibraries(module: LoadedModule): Boolean {
        val nativeNames =
            module.code.moduleLibraryNames
                .map { "${module.packageName}!/${it}" }
                .toTypedArray()
        val nativeImages = module.code.preLoadedNativeLibraries.toTypedArray()
        if (nativeNames.isEmpty() && nativeImages.isEmpty()) return true
        nativeImages.forEachIndexed { index, memory ->
            traceNativeSharedMemory(
                module.packageName,
                module.code.moduleLibraryNames.getOrNull(index) ?: "<missing-name:$index>",
                memory,
            )
        }
        val loaded =
            runCatching {
                    try {
                        NativeAPI.loadNativeLibraries(nativeNames, nativeImages)
                    } finally {
                        nativeImages.forEach { it.close() }
                    }
                }
                .getOrElse {
                    Log.e(TAG, "Failed to load native libraries for ${module.packageName}", it)
                    return false
                }
        if (loaded.size != nativeNames.size || loaded.any { !it }) {
            Log.e(TAG, "One or more native libraries failed for ${module.packageName}")
            return false
        }
        Log.i(
            TAG,
            "Loaded native libraries for ${module.packageName}: " +
                nativeNames.indices.joinToString { index -> "${nativeNames[index]}=${loaded[index]}" },
        )
        return true
    }

    // Publishes nothing, so a reload can fail before the old generation is touched.
    private fun buildGeneration(
        module: LoadedModule,
        isSystemServer: Boolean,
        processName: String,
    ): Pair<Generation, List<WbModule>>? {
        try {
            Log.d(TAG, "Render module ${module.packageName}")
            traceAnonymousExecMaps("buildGeneration:start", module.packageName)

            val librarySearchPath =
                if (USE_RAW_VECTOR_LINKER_NATIVE_LOADER) {
                    buildString {
                        if (isSystemServer) {
                            module.code.nativeLibraryDir?.let {
                                append(it).append(File.pathSeparator)
                            }
                        }
                        val abis =
                            if (Process.is64Bit()) Build.SUPPORTED_64_BIT_ABIS
                            else Build.SUPPORTED_32_BIT_ABIS
                        for (abi in abis) {
                            append(module.apkPath)
                                .append("!/lib/")
                                .append(abi)
                                .append(File.pathSeparator)
                        }
                    }
                } else {
                    // Module native libraries are loaded separately from sealed SharedMemory by
                    // Vector's soLoader. Keep the BaseDexClassLoader search path empty so
                    // System.loadLibrary() and APK zip paths cannot reach Android's linker.
                    ""
                }
            if (USE_RAW_VECTOR_LINKER_NATIVE_LOADER) {
                Log.w(
                    TAG,
                    "rawVector linker librarySearchPath for ${module.packageName}: $librarySearchPath",
                )
            }

            // VectorModuleClassLoader directly links BaseDexClassLoader's hidden ByteBuffer
            // constructor. The exemption must be installed before the first active reference to
            // that class, because ART may link its super call before entering loadApk(). rustFrida
            // performs the equivalent VMRuntime call while initializing its Java support; Vector
            // narrows it to BaseDexClassLoader so unrelated hidden APIs remain denied.
            check(
                Build.VERSION.SDK_INT < Build.VERSION_CODES.P ||
                    HookBridge.exemptModuleClassLoaderHiddenApi()
            ) {
                "Unable to enable the in-memory module ClassLoader constructor"
            }
            traceAnonymousExecMaps("buildGeneration:after-hidden-api", module.packageName)

            // Create the isolated ClassLoader for the module
            val initLoader = WbModule::class.java.classLoader
            traceAnonymousExecMaps("renderApk:before", module.packageName)
            // Append Vector's NativeInitFire carrier dex so the carrier class is defined by the
            // MODULE ClassLoader itself: firePendingNativeInits binds and invokes its declared
            // native method to give deferred native_init entrypoints the module FindClass context
            // (rawVector semantics). If allocation fails, degrade to the framework-context
            // fallback (carrierClass = null).
            val carrierMemory = if (module.code.moduleLibraryNames.isNotEmpty()) {
                NativeInitFireDex.newSharedMemory()
            } else {
                null
            }
            val moduleDexes = buildList {
                addAll(module.code.preLoadedDexes)
                carrierMemory?.let { add(it) }
            }
            val moduleClassLoader =
                VectorModuleClassLoader.loadApk(
                    module.apkPath,
                    moduleDexes,
                    librarySearchPath,
                    initLoader,
                    blockLegacyApi = module.code.targetApiVersion >= 102,
                )
            traceAnonymousExecMaps("renderApk:after", module.packageName)

            // Security/Integrity Check: Ensure the module isn't bundling its own API classes
            if (
                moduleClassLoader.loadClass(WbModule::class.java.name).classLoader !==
                    initLoader
            ) {
                Log.e(TAG, "The Xposed API classes are compiled into ${module.packageName}")
                return null
            }

            // Load the module's native ELF images (soLoader) and run its native_init after the
            // module ClassLoader exists, so native_init's FindClass resolves the module's own Java
            // classes. rawVector timing: the module itself dlopens its library from its Java code,
            // so the call stack ClassLoader is the module's; here the module ClassLoader is set as
            // the thread context ClassLoader around the call instead.
            traceAnonymousExecMaps("loadModuleNativeLibraries:before", module.packageName)
            if (!loadModuleNativeLibraries(module)) return null
            traceAnonymousExecMaps("loadModuleNativeLibraries:after", module.packageName)

            // Fire the deferred native_init entrypoints from a module-class context so that
            // FindClass inside them resolves the module's own Java classes (rawVector timing:
            // module classes are available by now; the entry classes below may not exist yet).
            if (carrierMemory != null) {
                Log.i(
                    TAG,
                    "Firing deferred native_init entrypoints for ${module.packageName}",
                )
                val carrierClass =
                    runCatching { moduleClassLoader.loadClass(NativeInitFireDex.CLASS_NAME) }
                        .onFailure { e ->
                            Log.e(TAG, "Cannot load the NativeInitFire carrier class", e)
                        }
                        .getOrNull()
                val fired = NativeAPI.firePendingNativeInits(carrierClass)
                if (!fired) {
                    Log.e(
                        TAG,
                        "Failed to fire deferred native_init entrypoints for ${module.packageName}",
                    )
                    return null
                }
            } else if (module.code.moduleLibraryNames.isNotEmpty()) {
                Log.w(
                    TAG,
                    "Carrier dex unavailable for ${module.packageName}; native_init will stay " +
                        "deferred until a module-class context is available",
                )
            }

            // Create the Context that will be injected into the module
            val vectorContext =
                VectorContext(
                    packageName = module.packageName,
                    applicationInfo = module.applicationInfo,
                    service = module.service, // Our IPC client
                    defaultExceptionMode =
                        if (module.code.exceptionPassthrough) ExceptionMode.PASSTHROUGH
                        else ExceptionMode.PROTECTIVE,
                )

            if (USE_RAW_VECTOR_LINKER_NATIVE_LOADER) {
                module.code.moduleLibraryNames.forEach { libraryName ->
                    Log.w(
                        TAG,
                        "rawVector linker registering native entrypoint $libraryName for ${module.packageName}",
                    )
                    NativeAPI.recordNativeEntrypoint(libraryName)
                }
            }

            // Instantiate the module entry classes
            val entries = mutableListOf<WbModule>()
            for (className in module.code.moduleClassNames) {
                runCatching {
                        traceAnonymousExecMaps("renderClass:before:$className", module.packageName)
                        val moduleClass = moduleClassLoader.loadClass(className)
                        traceAnonymousExecMaps("renderClass:after:$className", module.packageName)
                        Log.v(TAG, "Render class $moduleClass")

                        if (!WbModule::class.java.isAssignableFrom(moduleClass)) {
                            Log.e(TAG, "Class does not extend WbModule, skipping.")
                            return@runCatching
                        }

                        val constructor = moduleClass.getDeclaredConstructor()
                        constructor.isAccessible = true
                        traceAnonymousExecMaps("constructor:before:$className", module.packageName)
                        val moduleInstance = constructor.newInstance() as WbModule
                        traceAnonymousExecMaps("constructor:after:$className", module.packageName)

                        // detach() is per entry: only the instance that calls it stops.
                        moduleInstance.attachFramework(vectorContext) {
                            VectorLifecycleManager.detach(moduleInstance)
                        }
                        traceAnonymousExecMaps("attachFramework:after:$className", module.packageName)

                        entries.add(moduleInstance)
                    }
                    .onFailure { e -> Log.e(TAG, "Failed to instantiate class $className", e) }
            }

            // A generation with nothing in it is not a generation. Every entry class can fail to
            // instantiate - a constructor that throws, a class that does not extend WbModule -
            // and each of those is logged and skipped above, which used to leave an empty list that
            // every caller then treated as success: the initial load reported the module loaded,
            // and a hot reload committed the empty generation, never called onHotReloaded, never
            // unhooked the old hooks, and answered SUCCEEDED while the process went on running the
            // previous generation. The module was then wedged, because the committed generation had
            // no live entry for any later reload to hand over to.
            if (entries.isEmpty()) {
                Log.e(TAG, "No entry class of ${module.packageName} could be instantiated")
                return null
            }

            if (module.code.moduleLibraryNames.isNotEmpty() &&
                module.code.preLoadedNativeLibraries.isEmpty()) {
                Log.w(
                    TAG,
                    "Module ${module.packageName} has native entries but no soLoader images",
                )
            }

            val generation =
                Generation(moduleClassLoader, vectorContext, entries, isSystemServer, processName)
            return generation to entries
        } catch (e: Throwable) {
            Log.e(TAG, "Fatal error rendering module ${module.packageName}", e)
            return null
        }
    }

    fun hotReload(
        modulePackageName: String?,
        extras: Bundle?,
        newModule: LoadedModule?,
    ): HotReloadOutcome {
        val packageName =
            modulePackageName ?: return unsupported("Hot reload was requested without a module")
        val lock = reloadLocks.computeIfAbsent(packageName) { ReentrantLock() }
        if (!lock.tryLock()) {
            return outcome(
                IWbService.HOT_RELOAD_IN_PROGRESS,
                "A reload of $packageName is already running in this process",
            )
        }
        return try {
            runHotReload(packageName, extras, newModule)
        } catch (t: Throwable) {
            Log.e(TAG, "Hot reload of $packageName failed", t)
            failed(describe(t))
        } finally {
            lock.unlock()
        }
    }

    private fun runHotReload(
        packageName: String,
        extras: Bundle?,
        newModule: LoadedModule?,
    ): HotReloadOutcome {
        if (newModule == null) {
            return unsupported("No new generation of $packageName was supplied")
        }
        val old =
            generations[packageName]
                ?: return unsupported(
                    "$packageName is not loaded in ${VectorServiceClient.processName}"
                )
        if (newModule.code.moduleClassNames.size != 1) {
            return unsupported("$packageName does not declare exactly one Java entry class")
        }

        // Keeps the old generation reachable until onHotReloaded has finished.
        val oldEntries = old.liveEntries()
        if (oldEntries.isEmpty()) {
            // Not a refusal: a null message means onHotReloading returned false, and nothing ran.
            Log.w(TAG, "No attached entry of $packageName can accept a hot reload")
            return unsupported("Every entry of $packageName has detached in this process")
        }

        val built =
            buildGeneration(newModule, old.isSystemServer, old.processName)
                ?: return unsupported("Cannot build a new generation of $packageName")
        val (newGeneration, newEntries) = built

        // Before the callback, so registrations from inside it fail while unhook and replace work.
        // Under the hook registry's lock for this module, because a registration on another thread
        // that has already passed its own check must either finish before the freeze - and so be in
        // the list the successor is handed - or see the freeze and fail. Held for the flag write
        // only: module code never runs inside it.
        synchronized(VectorHookBuilder.lockOf(packageName)) { old.context.freeze() }

        var savedState: Any? = null
        val reloadingParam =
            object : HotReloadingParam {
                override fun getExtras(): Bundle? = extras

                override fun setSavedInstanceState(outState: Any?) {
                    rejectOldGenerationState(outState, old.classLoader)
                    savedState = outState
                }
            }

        val accepted =
            try {
                // One refusal cancels the reload for the whole module.
                oldEntries.all { it.onHotReloading(reloadingParam) }
            } catch (t: Throwable) {
                old.context.unfreeze()
                Log.e(TAG, "onHotReloading of $packageName threw", t)
                return failed(describe(t))
            }
        if (!accepted) {
            old.context.unfreeze()
            Log.d(TAG, "$packageName refused the hot reload")
            return refusal()
        }

        // Captured after the freeze and after old code had its chance to unhook.
        val oldHandles = VectorHookBuilder.snapshotHandles(packageName)

        oldEntries.forEach { VectorLifecycleManager.activeModules.remove(it) }
        // Active before the callback, so an entry detaching from inside it is honoured.
        newEntries.forEach { VectorLifecycleManager.activeModules.add(it) }

        val reloadedParam =
            object : HotReloadedParam {
                override fun isSystemServer(): Boolean = old.isSystemServer

                override fun getProcessName(): String = old.processName

                override fun getExtras(): Bundle? = extras

                override fun getSavedInstanceState(): Any? = savedState

                override fun getOldHookHandles(): List<WbInterface.HookHandle> = oldHandles
            }

        // Committed before the callback runs, because the interface releases the old generation
        // "after this callback returns or throws" - there is no rollback. A throw here leaves the
        // process running new code that has migrated some of its hooks and not others, and says so
        // through the result; rolling back could not undo the replaceHook calls the new code had
        // already made anyway, and would restore old entries whose hooks now run new hookers.
        generations[packageName] = newGeneration

        var failure: Throwable? = null
        // The default onHotReloaded already unhooks these; doing both would double-unhook.
        newEntries
            .filter { VectorLifecycleManager.isActive(it) }
            .forEach {
                if (failure != null) return@forEach
                runCatching { it.onHotReloaded(reloadedParam) }.onFailure { t -> failure = t }
            }

        // The last framework-owned reference to the old generation goes with this frame: its map
        // entry is gone, its entries are out of activeModules, and oldEntries dies on return.
        failure?.let {
            Log.e(TAG, "onHotReloaded of $packageName threw", it)
            return failed(describe(it), generationChanged = true)
        }

        Log.d(TAG, "Hot reloaded $packageName")
        return outcome(IWbService.HOT_RELOAD_SUCCEEDED, null, generationChanged = true)
    }

    /**
     * Rejects saved state that the old generation created, which would otherwise keep the retired
     * classloader reachable through the new one. A shallow scan, as the API describes it: a
     * diagnostic aid rather than an object graph verifier.
     */
    private fun rejectOldGenerationState(state: Any?, oldClassLoader: ClassLoader) {
        if (state == null) return
        reject(state, oldClassLoader)
        when (state) {
            is Array<*> -> state.forEach { it?.let { e -> reject(e, oldClassLoader) } }
            is Collection<*> -> state.forEach { it?.let { e -> reject(e, oldClassLoader) } }
            is Map<*, *> ->
                state.forEach { (k, v) ->
                    k?.let { reject(it, oldClassLoader) }
                    v?.let { reject(it, oldClassLoader) }
                }
        }
    }

    private fun reject(value: Any, oldClassLoader: ClassLoader) {
        if (definedBy(value.javaClass, oldClassLoader)) {
            throw IllegalArgumentException(
                "Saved instance state contains ${value.javaClass.name}, which was created under " +
                    "the old module classloader"
            )
        }
    }

    private fun definedBy(clazz: Class<*>, classLoader: ClassLoader): Boolean {
        var loader: ClassLoader? =
            (if (clazz.isArray) clazz.componentType else clazz)?.classLoader
        while (loader != null) {
            if (loader === classLoader) return true
            loader = loader.parent
        }
        return false
    }

    private fun outcome(
        status: Int,
        message: String?,
        refused: Boolean = false,
        generationChanged: Boolean = false,
    ) =
        HotReloadOutcome().apply {
            this.status = status
            this.message = message
            this.refused = refused
            this.generationChanged = generationChanged
        }

    private fun unsupported(message: String) =
        outcome(IWbService.HOT_RELOAD_UNSUPPORTED, message)

    private fun failed(message: String, generationChanged: Boolean = false) =
        outcome(IWbService.HOT_RELOAD_FAILED, message, generationChanged = generationChanged)

    private fun refusal() = outcome(IWbService.HOT_RELOAD_FAILED, null, refused = true)

    private fun describe(t: Throwable) = "${t.javaClass.name}: ${t.message ?: "no message"}"
}
