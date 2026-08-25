package org.matrix.vector.daemon.ipc

import android.os.IBinder
import android.os.Parcel
import android.os.ParcelFileDescriptor
import android.os.Process
import android.os.RemoteException
import android.os.SharedMemory
import android.system.OsConstants
import android.util.Log
import de.wwb.android.wbosed.service.HookedProcess
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import org.matrix.vector.ipc.LoadedModule
import org.matrix.vector.ipc.IProcessChannel
import org.matrix.vector.ipc.IFrameworkService
import org.matrix.vector.ipc.ModuleCode
import org.matrix.vector.daemon.data.ConfigCache
import org.matrix.vector.daemon.data.FileSystem
import org.matrix.vector.daemon.system.FIRST_APPLICATION_UID
import org.matrix.vector.daemon.system.PER_USER_RANGE
import org.matrix.vector.daemon.utils.InstallerVerifier
import org.matrix.vector.daemon.utils.ObfuscationManager

private const val TAG = "VectorFrameworkService"

private fun elapsedUs(startNs: Long): Long = (System.nanoTime() - startNs) / 1_000L

// Hardcoded transaction code from BridgeService
const val BRIDGE_TRANSACTION_CODE =
    ('_'.code shl 24) or ('V'.code shl 16) or ('E'.code shl 8) or 'C'.code
const val DEX_TRANSACTION_CODE =
    ('_'.code shl 24) or ('D'.code shl 16) or ('E'.code shl 8) or 'X'.code
const val OBFUSCATION_MAP_TRANSACTION_CODE =
    ('_'.code shl 24) or ('O'.code shl 16) or ('B'.code shl 8) or 'F'.code
private const val FRAMEWORK_DESCRIPTOR = "org.matrix.vector.ipc.IFrameworkService"
private const val GET_LEGACY_MODULES_TRANSACTION = IBinder.FIRST_CALL_TRANSACTION + 1
private const val GET_MODULES_TRANSACTION = IBinder.FIRST_CALL_TRANSACTION + 2

/**
 * What an injected process asks the framework for — this project's `IFrameworkService`.
 *
 * Also the daemon's register of which process is running which module, because answering
 * `getModules` is what makes a process a hot reload target for each module returned. See
 * `IFrameworkService.aidl` for who may call what and how a caller is authenticated.
 *
 * Was called `ApplicationService`, which named neither the interface it implements nor anything it
 * does: nothing here is about an `Application`.
 */
object FrameworkService : IFrameworkService.Stub() {

  data class ProcessKey(val uid: Int, val pid: Int)

  private val processes = ConcurrentHashMap<ProcessKey, ProcessInfo>()

  /** One module generation loaded into one process: what a hot reload request addresses. */
  class HotReloadTarget(
      val id: Long,
      val modulePackageName: String,
      val processName: String,
      val uid: Int,
      val pid: Int,
      @Volatile var loadedVersionCode: Long,
      val hotReloadable: Boolean,
  ) {
    val state = AtomicInteger(HookedProcess.TARGET_STATE_UP_TO_DATE)
  }

  private val hotReloadTargets = ConcurrentHashMap<Long, HotReloadTarget>()

  // Ids are framework-assigned and never reused, as HookedProcess.targetId requires.
  private val nextHotReloadTargetId = AtomicLong(1)

  private class ProcessInfo(val key: ProcessKey, val processName: String, val heartBeat: IBinder) :
      IBinder.DeathRecipient {
    val targetIds = ConcurrentHashMap<String, Long>()

    @Volatile var hotReloadBinder: IProcessChannel? = null

    init {
      heartBeat.linkToDeath(this, 0)
      processes[key] = this
    }

    override fun binderDied() {
      heartBeat.unlinkToDeath(this, 0)
      processes.remove(key)
      targetIds.values.forEach { hotReloadTargets.remove(it) }
    }
  }

  private fun recordHotReloadTargets(info: ProcessInfo, modules: List<LoadedModule>) {
    for (module in modules) {
      info.targetIds.computeIfAbsent(module.packageName) {
        val id = nextHotReloadTargetId.getAndIncrement()
        hotReloadTargets[id] =
            HotReloadTarget(
                id = id,
                modulePackageName = module.packageName,
                processName = info.processName,
                uid = info.key.uid,
                pid = info.key.pid,
                loadedVersionCode = module.versionCode,
                // Hot reload is specified only for modules with exactly one Java entry class.
                hotReloadable = module.code.moduleClassNames.size == 1,
            )
        id
      }
    }
  }

  /**
   * Whether [userId]'s copy of the module may address [target].
   *
   * One module is one package and one APK, but the copies installed for two users are two apps with
   * two uids and two sets of preferences, and neither has any business reloading the other's
   * processes. `ConfigCache` draws the same line when it decides where a module may be injected at
   * all; this is that boundary applied to reloading what is already there.
   *
   * The carve-out is for the AID_* uids below [FIRST_APPLICATION_UID], and it has to be: a module in
   * any user may take the framework into its scope, and `ModuleDatabase.setModuleScope` stores that
   * row against user 0 whoever asked for it, because system_server is one process for the whole
   * device. Nothing downstream can therefore tell which user requested it - so every user holding
   * the module is equally entitled to the one generation loaded there.
   *
   * Not `uid < PER_USER_RANGE`, which is what this said first: that admits the whole of user 0,
   * every app process on a single-user device, and leaves the check doing nothing at all.
   */
  private fun addressableBy(target: HotReloadTarget, userId: Int): Boolean =
      target.uid < FIRST_APPLICATION_UID || target.uid / PER_USER_RANGE == userId

  // Not filtered to hot-reloadable targets: the AIDL documents this as hooked processes, and one
  // that cannot be reloaded answers UNSUPPORTED rather than disappearing.
  fun getHotReloadTargets(modulePackageName: String, userId: Int): List<HookedProcess> {
    val installedVersion = ConfigCache.state.modules[modulePackageName]?.versionCode
    return hotReloadTargets.values
        .filter { it.modulePackageName == modulePackageName && addressableBy(it, userId) }
        .map { target ->
          HookedProcess().apply {
            targetId = target.id
            uid = target.uid
            pid = target.pid
            processName = target.processName
            state = reportedState(target, installedVersion)
            loadedVersionCode = target.loadedVersionCode
          }
        }
  }

  // RELOADING and FAILED describe the last attempt and outrank a version comparison.
  private fun reportedState(target: HotReloadTarget, installedVersion: Long?): Int {
    val state = target.state.get()
    if (state != HookedProcess.TARGET_STATE_UP_TO_DATE) return state
    // Zero means unknown, not old; claiming STALE would never be satisfiable by a reload.
    if (target.loadedVersionCode == 0L) return state
    return if (installedVersion != null && installedVersion != target.loadedVersionCode) {
      HookedProcess.TARGET_STATE_STALE
    } else {
      state
    }
  }

  // system_server records its targets before PMS exists, so they start without a version.
  fun backfillLoadedVersions() {
    hotReloadTargets.values
        .filter { it.loadedVersionCode == 0L }
        .forEach { target ->
          ConfigCache.state.modules[target.modulePackageName]
              ?.versionCode
              ?.takeIf { it != 0L }
              ?.let { target.loadedVersionCode = it }
        }
  }

  fun forgetHotReloadTargets(modulePackageName: String) {
    hotReloadTargets.values.removeIf { it.modulePackageName == modulePackageName }
    processes.values.forEach { it.targetIds.remove(modulePackageName) }
  }

  fun staleHotReloadTargets(modulePackageName: String): List<HotReloadTarget> {
    val installedVersion = ConfigCache.state.modules[modulePackageName]?.versionCode ?: return emptyList()
    return hotReloadTargets.values.filter {
      it.modulePackageName == modulePackageName &&
          it.hotReloadable &&
          it.loadedVersionCode != 0L &&
          it.loadedVersionCode != installedVersion
    }
  }

  fun getHotReloadTarget(targetId: Long, modulePackageName: String, userId: Int): HotReloadTarget? =
      hotReloadTargets[targetId]?.takeIf {
        it.modulePackageName == modulePackageName && addressableBy(it, userId)
      }

  /**
   * Whether the process behind [target] is still the registered one.
   *
   * The heartbeat's DeathRecipient is what actually knows a process died. The exception a
   * transaction throws does not: a frozen but perfectly alive target fails a transaction the same
   * way a dead one does, and reporting that as PROCESS_DIED would be a lie the module app has no
   * way to check.
   */
  fun isProcessRegistered(target: HotReloadTarget): Boolean =
      processes.containsKey(ProcessKey(target.uid, target.pid))

  // Reloads are serialized per target, so check and transition must be one atomic step.
  fun beginHotReload(target: HotReloadTarget): Boolean {
    while (true) {
      val current = target.state.get()
      if (current == HookedProcess.TARGET_STATE_RELOADING) return false
      if (target.state.compareAndSet(current, HookedProcess.TARGET_STATE_RELOADING)) return true
    }
  }

  fun endHotReload(target: HotReloadTarget, state: Int, loadedVersionCode: Long? = null) {
    loadedVersionCode?.let { target.loadedVersionCode = it }
    target.state.set(state)
  }

  fun getHotReloadBinder(target: HotReloadTarget): IProcessChannel? =
      processes[ProcessKey(target.uid, target.pid)]?.hotReloadBinder

  override fun attachProcessChannel(channel: IProcessChannel) {
    // Synchronous on purpose: a oneway transaction arrives with getCallingPid() == 0, and this
    // registry is keyed on (uid, pid). See the note on the AIDL.
    val info = ensureRegistered()
    info.hotReloadBinder = channel
    Log.d(TAG, "Process channel attached for ${info.processName} (pid=${info.key.pid})")
  }

  override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
    when (code) {
      GET_LEGACY_MODULES_TRANSACTION -> {
        data.enforceInterface(FRAMEWORK_DESCRIPTOR)
        reply?.writeNoException()
        val startNs = System.nanoTime()
        val callerUid = getCallingUid()
        val callerPid = getCallingPid()
        Log.i(
            TAG,
            "[startup-trace] legacyModules transact begin uid=$callerUid pid=$callerPid",
        )
        val modules = getLegacyModules()
        val queryUs = elapsedUs(startNs)
        val writeStartNs = System.nanoTime()
        reply?.writeTypedList(modules)
        val writeUs = elapsedUs(writeStartNs)
        Log.i(
            TAG,
            "[startup-trace] legacyModules transact end uid=$callerUid pid=$callerPid " +
                "count=${modules.size} queryUs=$queryUs writeUs=$writeUs " +
                "totalUs=${elapsedUs(startNs)}",
        )
        return true
      }
      GET_MODULES_TRANSACTION -> {
        data.enforceInterface(FRAMEWORK_DESCRIPTOR)
        reply?.writeNoException()
        reply?.writeTypedList(getModules())
        return true
      }
      DEX_TRANSACTION_CODE -> {
        val shm = FileSystem.getPreloadDex(ConfigCache.state.isDexObfuscateEnabled) ?: return false
        reply?.writeNoException()
        reply?.let { shm.writeToParcel(it, 0) }
        reply?.writeLong(shm.size.toLong())
        return true
      }
      OBFUSCATION_MAP_TRANSACTION_CODE -> {
        val obfuscation = ConfigCache.state.isDexObfuscateEnabled
        val signatures = ObfuscationManager.getSignatures()
        reply?.writeNoException()
        reply?.writeInt(signatures.size * 2)
        for ((key, value) in signatures) {
          reply?.writeString(key)
          reply?.writeString(if (obfuscation) value else key)
        }
        return true
      }
    }
    return super.onTransact(code, data, reply, flags)
  }

  fun registerHeartBeat(uid: Int, pid: Int, processName: String, heartBeat: IBinder): Boolean {
    return runCatching {
          ProcessInfo(ProcessKey(uid, pid), processName, heartBeat)
          true
        }
        .getOrDefault(false)
  }

  fun hasRegister(uid: Int, pid: Int): Boolean = processes.containsKey(ProcessKey(uid, pid))

  private fun ensureRegistered(): ProcessInfo {
    val key = ProcessKey(getCallingUid(), getCallingPid())
    val info = processes[key]
    if (info == null) {
      Log.w(TAG, "Unauthorized IPC call from uid=${key.uid} pid=${key.pid}")
      throw RemoteException("Not registered")
    }
    return info
  }

  private fun magicOf(buffer: java.nio.ByteBuffer): String =
      if (buffer.capacity() >= 4) {
        "%02x %02x %02x %02x"
            .format(
                buffer.get(0).toInt() and 0xff,
                buffer.get(1).toInt() and 0xff,
                buffer.get(2).toInt() and 0xff,
                buffer.get(3).toInt() and 0xff)
      } else {
        "<too-small:${buffer.capacity()}>"
      }

  private fun cloneSharedMemory(
      memory: SharedMemory,
      label: String? = null,
      protectFlags: Int = OsConstants.PROT_READ,
  ): SharedMemory {
    val source = memory.mapReadOnly()
    var clone: SharedMemory? = null
    try {
      val copy = SharedMemory.create(null, source.capacity())
      clone = copy
      val destination = copy.mapReadWrite()
      try {
        destination.put(source.duplicate())
      } finally {
        SharedMemory.unmap(destination)
      }
      check(copy.setProtect(protectFlags)) { "Unable to seal module SharedMemory clone" }
      label?.let {
        val check = copy.mapReadOnly()
        try {
          Log.i(
              TAG,
              "Cloned module SharedMemory $it size=${source.capacity()} " +
                  "sourceMagic=${magicOf(source)} cloneMagic=${magicOf(check)}")
        } finally {
          SharedMemory.unmap(check)
        }
      }
      clone = null
      return copy
    } finally {
      SharedMemory.unmap(source)
      clone?.close()
    }
  }

  private fun cloneSharedMemoryList(
      memories: List<SharedMemory>?,
      labelPrefix: String? = null,
      protectFlags: Int = OsConstants.PROT_READ,
  ): List<SharedMemory> {
    if (memories.isNullOrEmpty()) return emptyList()
    val clones = ArrayList<SharedMemory>(memories.size)
    try {
      memories.forEachIndexed { index, memory ->
        clones.add(cloneSharedMemory(memory, labelPrefix?.let { "$it[$index]" }, protectFlags))
      }
      return clones
    } catch (error: Throwable) {
      clones.forEach { memory -> runCatching { memory.close() } }
      throw error
    }
  }

  private fun cloneModuleForReply(module: LoadedModule): LoadedModule {
    val code = module.code
    val clonedCode =
        ModuleCode().apply {
          preLoadedDexes = cloneSharedMemoryList(code.preLoadedDexes)
          moduleClassNames = code.moduleClassNames?.toList().orEmpty()
          moduleLibraryNames = code.moduleLibraryNames?.toList().orEmpty()
          preLoadedNativeLibraries =
              cloneSharedMemoryList(
                  code.preLoadedNativeLibraries,
                  "${module.packageName}:native",
                  OsConstants.PROT_READ or OsConstants.PROT_EXEC)
          legacy = code.legacy
          targetApiVersion = code.targetApiVersion
          autoHotReload = code.autoHotReload
          exceptionPassthrough = code.exceptionPassthrough
          nativeLibraryDir = code.nativeLibraryDir
        }
    return LoadedModule().apply {
      packageName = module.packageName
      appId = module.appId
      versionCode = module.versionCode
      apkPath = module.apkPath
      this.code = clonedCode
      applicationInfo = module.applicationInfo
      service = module.service
    }
  }

  private fun cloneModulesForReply(modules: List<LoadedModule>): List<LoadedModule> =
      modules.map { module ->
        runCatching { cloneModuleForReply(module) }
            .onFailure { Log.e(TAG, "Failed to clone module ${module.packageName} for IPC", it) }
            .getOrThrow()
      }

  private fun getAllModules(): List<LoadedModule> {
    val startNs = System.nanoTime()
    val callerUid = getCallingUid()
    val callerPid = getCallingPid()
    val registerStartNs = System.nanoTime()
    val info = ensureRegistered()
    val registerUs = elapsedUs(registerStartNs)
    if (info.key.uid == Process.SYSTEM_UID && info.processName == "system") {
      val systemStartNs = System.nanoTime()
      val modules = ConfigCache.getModulesForSystemServer()
      Log.i(
          TAG,
          "[startup-trace] legacyModules getAll system_server uid=$callerUid pid=$callerPid " +
              "registerUs=$registerUs systemConfigUs=${elapsedUs(systemStartNs)} " +
              "count=${modules.size} totalUs=${elapsedUs(startNs)}")
      return modules
    }
    val managerStartNs = System.nanoTime()
    val isManager = ManagerService.isRunningManager(callerPid, info.key.uid)
    val managerUs = elapsedUs(managerStartNs)
    if (isManager) {
      Log.i(
          TAG,
          "[startup-trace] legacyModules getAll manager uid=$callerUid pid=$callerPid " +
              "registerUs=$registerUs managerCheckUs=$managerUs count=0 " +
              "totalUs=${elapsedUs(startNs)}")
      return emptyList()
    }
    val configStartNs = System.nanoTime()
    val modules = ConfigCache.getModulesForProcess(info.processName, info.key.uid)
    Log.i(
        TAG,
        "[startup-trace] legacyModules getAll process=${info.processName} " +
            "uid=${info.key.uid} pid=${info.key.pid} registerUs=$registerUs " +
            "managerCheckUs=$managerUs configUs=${elapsedUs(configStartNs)} " +
            "count=${modules.size} totalUs=${elapsedUs(startNs)}")
    return modules
  }

  override fun getModules(): List<LoadedModule> {
    val info = ensureRegistered()
    val modules = getAllModules().filter { !it.code.legacy }
    recordHotReloadTargets(info, modules)
    return cloneModulesForReply(modules)
  }

  override fun getLegacyModules(): List<LoadedModule> {
    val startNs = System.nanoTime()
    val allStartNs = System.nanoTime()
    val allModules = getAllModules()
    val allUs = elapsedUs(allStartNs)
    val filterStartNs = System.nanoTime()
    val legacyModules = allModules.filter { it.code.legacy }
    val filterUs = elapsedUs(filterStartNs)
    val cloneStartNs = System.nanoTime()
    val result = cloneModulesForReply(legacyModules)
    val cloneUs = elapsedUs(cloneStartNs)
    Log.i(
        TAG,
        "[startup-trace] legacyModules query end allCount=${allModules.size} " +
            "legacyCount=${legacyModules.size} resultCount=${result.size} allUs=$allUs " +
            "filterUs=$filterUs cloneUs=$cloneUs totalUs=${elapsedUs(startNs)}")
    return result
  }

  override fun isLogMuted(): Boolean = !ManagerService.isVerboseLogEnabled()

  override fun getPrefsPath(packageName: String): String {
    val info = ensureRegistered()
    return ConfigCache.getPrefsPath(packageName, info.key.uid)
  }

  override fun openManagerApk(): ParcelFileDescriptor? {
    ensureRegistered()
    return runCatching {
          // Verify the APK signature before serving it
          InstallerVerifier.verifyInstallerSignature(FileSystem.managerApkPath.toString())
          ParcelFileDescriptor.open(
              FileSystem.managerApkPath.toFile(), ParcelFileDescriptor.MODE_READ_ONLY)
        }
        .onFailure { Log.e(TAG, "Failed to open or verify manager APK", it) }
        .getOrNull()
  }

  override fun requestManagerService(): IBinder? {
    val info = ensureRegistered()
    val pid = info.key.pid
    val uid = info.key.uid
    // postStartManager compares the caller against the pid the daemon launched the manager into,
    // so this reports a decision already taken rather than making one. It is its own call because it
    // answers a different question from the one that opens the APK, not because it costs anything.
    if (!ManagerService.postStartManager(pid) && !ConfigCache.isManager(uid)) return null
    return ManagerService.obtainManagerBinder(info.heartBeat, pid, uid)
  }
}
