package com.example.duckusb

/**
 * Shared constants for the hook and the UI. Both live toggles default ON.
 *
 * Ported from DuckUSB (github.com/Bouteillepleine/DuckUSB). Only PKG changed; every other value
 * is upstream's, and the provenance notes for them are kept because they encode measured
 * failures rather than preferences.
 */
object Config {
    const val PKG = "com.example.duckusb"
    const val PREFS_NAME = "duck_usb_prefs"

    /**
     * Master pause. ANDed over every other gate: while true, no hook body does anything.
     * It does NOT uninstall hooks or unload the native library — those are decided at process
     * load — so it is a pause, not a module off-switch. Vector's own module switch is the real
     * one. Pushed live over the binder, so pausing takes effect on system_server immediately.
     */
    const val KEY_PAUSED = "paused"

    /** Spoof adb_enabled / adb_wifi_enabled / development_settings_enabled to scoped apps. */
    const val KEY_SPOOF = "spoof_enabled"

    /**
     * Framework mode — EXPERIMENTAL, default OFF. A single hook in System Framework
     * (system_server) on the settings provider's call() spoofs the adb Settings keys for every
     * app at the server-side chokepoint. ⚠️ Upstream measured that hooking
     * ContentProvider$Transport.call in system_server BOOTLOOPS some ROMs (OP15 / Android 16),
     * which is why this module hooks only the concrete SettingsProvider's own call() and stays
     * a manual opt-in.
     */
    const val KEY_FRAMEWORK_MODE = "framework_mode_enabled"

    /**
     * Per-app Settings spoof (default ON): the client-side hook on the static
     * Settings.Global/Secure getters, installed in each scoped app. The safe, proven path.
     * Restart the app to apply.
     */
    const val KEY_CLIENT_FALLBACK = "client_settings_fallback"

    /**
     * Verbose diagnostics: one log line per injection (package / process / uid / which guards
     * matched). Invaluable when a hook silently fails to install, but noisy across a boot, so
     * it stays OFF unless you are investigating.
     */
    const val KEY_VERBOSE_LOG = "verbose_log"

    /** Hide the persistent "USB debugging enabled" notification. */
    const val KEY_HIDE_NOTIF = "hide_notif_enabled"

    /**
     * Clear ApplicationInfo.FLAG_DEBUGGABLE in scoped apps (default ON). Port of the
     * rustFrida rf-xff-all-spoof.js (2) semantics: hook ContextImpl.getApplicationInfo and
     * Application.attach so a detector reading `applicationInfo.flags` sees the bit already
     * cleared, making `isAppDebuggable == false` and dropping the "debuggable app can be
     * attached" branch of the verdict. The real APK manifest is never touched.
     */
    const val KEY_DEBUGGABLE_SPOOF = "debuggable_spoof_enabled"

    /**
     * Device-state spoof (default ON): make scoped apps read a *less* suspicious device than the
     * one they are running on — a lock-screen password exists and a SIM is ready.
     *
     * Separate from [KEY_SPOOF] on purpose. That one is about USB debugging; these are checks a
     * detector runs alongside it, on APIs that have nothing to do with USB. They share the same
     * "app the user scoped is an app they want lied to" premise but not the same switch, because
     * they can be wanted independently — e.g. spoofing the SIM state to a scoped app that
     * legitimately uses telephony would be a bug, not a feature.
     */
    const val KEY_DEVICE_STATE_SPOOF = "device_state_spoof_enabled"

    /** Every boolean setting worth carrying across the move to remote preferences. */
    val BOOLEAN_KEYS: List<String> =
        listOf(KEY_PAUSED, KEY_SPOOF, KEY_FRAMEWORK_MODE, KEY_CLIENT_FALLBACK, KEY_VERBOSE_LOG,
            KEY_HIDE_NOTIF, KEY_DEBUGGABLE_SPOOF, KEY_DEVICE_STATE_SPOOF)

    /**
     * The value every spoofed boolean Settings key reads as, on both read paths.
     *
     * Both the static getters (layer A) and the provider transport (layer A5) return the value
     * as a String, because that is what the settings provider stores; an int-returning getter
     * parses it. Keeping one constant means the two layers can never disagree — a detector that
     * cross-checks them (as riskdetector does: [Settings] vs [Content Provider] vs
     * [Content Call]) sees a single consistent answer.
     */
    const val SETTINGS_OFF = "0"

    /**
     * OS plumbing that must never be lied to, by either the settings spoof or the property
     * spoof. com.android.mtp runs at an app uid, so the "uid < 10000" sparing does not reach
     * it. Telling the MTP service sys.usb.state=mtp / init.svc.adbd=stopped is how USB ends up
     * stuck on charge-only. com.oplus.ota is here for a different reason: its
     * EntryActivity.onCreateOptionsMenu removes the "Local install" overflow entry outright
     * when development_settings_enabled reads 0, so spoofing it hides a feature the user needs.
     */
    val SPARE_PACKAGES: Set<String> = setOf(
        "com.android.mtp",
        "com.android.externalstorage",
        "com.android.storagemanager",
        "com.android.sharedstoragebackup",
        "com.oneplus.filemanager",
        "com.oplus.filemanager",
        "com.oplus.ota",
    )

    /**
     * Property overrides served to scoped apps.
     *
     * These are READ-side lies only. The module hooks __system_property_get / _read /
     * _read_callback, android.os.SystemProperties.native_get*, and — for the one path no
     * in-process hook can reach, an exec'd getprop — ProcessBuilder.start()'s output
     * (A3 in DuckUsbModule). It never calls
     * __system_property_set, SystemProperties.set or setprop anywhere. The real property store
     * is never modified, so nothing here can influence boot or the USB gadget.
     */
    val PROP_OVERRIDES: Map<String, String> = mapOf(
        // sys.usb.ffs.ready is deliberately NOT spoofed. It is the USB function-filesystem
        // ready flag — machinery the USB stack acts on, not telemetry detectors read. Claiming
        // "0" contributed to killing the gadget outright (no MTP and no adb, charge-only).
        // Detectors look at sys.usb.state / sys.usb.config / init.svc.adbd; none need ffs.ready.
        "sys.usb.config" to "mtp",
        "sys.usb.state" to "mtp",
        "init.svc.adbd" to "stopped",
        // persist.sys.usb.config IS spoofed, on purpose.
        //
        // Measured 2026-09-12 with Duck Detector 526: spoofing it makes the detector report
        //                       Reflection  getprop  Native libc
        //  spoofed   ->        mtp         adb      mtp         => "Diverged" / 1 source mismatch
        //  unspoofed ->        adb                          => all three agree on adb
        // i.e. an exec'd process (getprop) reads the real property store, which no in-process
        // hook can reach, so this key exposes the layer whenever a detector cross-checks
        // sources.
        //
        // It is kept anyway — and the out-of-process path it exposed is now CLOSED by the
        // getprop exec spoof (A3 in DuckUsbModule): ProcessBuilder.start() is hooked, and a
        // getprop child's stdout is rewritten with these same overrides before the caller
        // parses it (both the full dump and the single-key forms). Reflection, native libc and
        // exec'd getprop therefore all read the value below, and the "Source mismatches: 1 /
        // Diverged" signal measured on 526 is gone.
        //
        // The three keys above carry the identical exposure; this detector simply does not
        // cross-check them. A read path that neither runs in this process nor spawns through
        // ProcessBuilder — a detector's own native fork/exec, or a direct /dev/__properties__
        // mmap (unavailable to untrusted apps on Android 12) — would still see the truth.
        "persist.sys.usb.config" to "mtp",
    )

    /**
     * 预热：把本对象里三个惰性集合（`listOf`/`setOf`/`mapOf`）先摸一遍，迫使 `Config.<clinit>`
     * 在「类加载器还没被换掉」的时刻完成。
     *
     * 必要性（设备实测，hunter 进程）：主线程被卡 8.784s，ART 原文是
     * `Verification of java.util.List kotlin.collections.CollectionsKt__CollectionsKt.emptyList()
     *  took 8.784s (0.57 bytecodes/s)`，栈为
     * `LoadedApk.createOrUpdateClassLoaderLocked` ← duckusb loader-hook(`z3.callback`/`x2.proceed`)
     * ← `DuckUsbModule.onPackageReady(:297)` ← `Config.<clinit>(Config.kt:63)` ← `listOf`
     * ← `ClassLoader.loadClass`。即这些集合的首次校验恰好发生在 loader 正在被替换的中途，
     * 此时每一次类解析都要穿本模块自己的 hook 包装（Java 帧 + JNI 重入），几千次解析累计成
     * 8.8 秒 → 输入分发超时 ANR + MIUI scout HANG。
     *
     * 在 `onModuleLoaded`（loader hook 生效之前）先调用一次，校验就以正常速度完成（毫秒级）。
     */
    fun warmUp() {
        BOOLEAN_KEYS.size
        SPARE_PACKAGES.size
        PROP_OVERRIDES.size
        // 同一类问题：onPackageReady 里用到的 Kotlin 标准库类若在这里首次加载，
        // 校验/加载就会发生在 loader 替换中途（实测 scout HANG：DexFile.defineClassNative
        // ← s3.loadClass ← ClassLoader.loadClass ← StringsKt.substringBefore$default
        // ← DuckUsbModule.onPackageReady）。提前摸一次，让它们走 onModuleLoaded 的正常路径。
        "warmUp:probe".substringBefore(':')
        "warmUp:probe".substringAfter(':')
        "warmUp:probe".startsWith("warm")
        // 各 hook 入口里用到的工具类（boolean 开关、进程名判断）。
        android.os.Process.myUid()
    }
}
