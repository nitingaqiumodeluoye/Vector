package com.example.duckusb

import android.app.Application
import android.app.Notification
import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.ApplicationInfo
import android.content.res.Resources
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.Binder
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import com.example.duckusb.service.Bridge
import com.example.duckusb.service.DuckService
import de.wwb.android.wbosed.api.WbInterface
import de.wwb.android.wbosed.api.WbModule
import de.wwb.android.wbosed.api.WbModuleInterface.ModuleLoadedParam
import de.wwb.android.wbosed.api.WbModuleInterface.PackageReadyParam
import de.wwb.android.wbosed.api.WbModuleInterface.SystemServerStartingParam
import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream
import java.lang.Process
import java.lang.ProcessBuilder
import java.lang.reflect.Method
import java.util.concurrent.TimeUnit

/**
 * DuckUSB — two independent tricks, ported from
 * [DuckUSB](https://github.com/Bouteillepleine/DuckUSB) (LSPosed / libxposed API 101) to
 * Vector's [WbModule] API.
 *
 *  A) SETTINGS SPOOF (per detector app). Makes scoped apps read USB debugging,
 *     wireless debugging and Developer Options as OFF while they stay really ON,
 *     by hooking the static getters on Settings.Global / Settings.Secure. Runs in
 *     every scoped app EXCEPT the core OS packages (so adbd / the Settings toggle
 *     itself are never lied to).
 *
 *  A2) PROPERTY SPOOF (automatic). In the same scoped app processes, sys.usb.* and
 *     init.svc.adbd are spoofed too, via SystemProperties and a native libc hook. Not
 *     user-switchable: scoping an app already states the intent, and it must never reach a
 *     system process — see the UID guard in [onPackageReady].
 *
 *  A3) GETPROP EXEC SPOOF (automatic, same scope). /system/bin/getprop is a symlink to
 *     toolbox: an INDEPENDENT process that mmaps the property area with its own libc, so no
 *     in-process hook — Java or native — can change what it prints. What CAN be intercepted
 *     is the spawn itself, which happens here. ProcessBuilder.start() is hooked; for a getprop
 *     command the real child still runs, but the Process handed back is wrapped so the child's
 *     stdout carries [Config.PROP_OVERRIDES] before the caller parses it. This closes the
 *     cross-source "Diverged" signal measured with Duck Detector 526 (reflection mtp /
 *     getprop adb / native libc mtp).
 *
 *  A7) USB RUNTIME STATE SPOOF (automatic, same scope). The sticky
 *     `android.hardware.usb.action.USB_STATE` broadcast carries the gadget's applied state
 *     (`connected` / `configured` / `adb`). It is the one ADB evidence that is neither a
 *     Setting nor a property, so none of the layers above can reach it and a detector reading
 *     it still sees adb=true. registerReceiver(...) is hooked and the returned Intent is
 *     rebuilt with the adb flag cleared (MTP is kept, so the story stays consistent with our
 *     own sys.usb.state=mtp). Never touches system_server or adbd itself.
 *
 *  B) NOTIFICATION SUPPRESSOR (System Framework / System UI). Hides the persistent
 *     "USB debugging enabled" notification. That notification is posted by system_server, so
 *     this half deliberately DOES run in system_server and com.android.systemui — the ones the
 *     spoof half skips.
 *
 * Scope: `module.prop` sets `staticScope=false`, so `scope.list` (system, android,
 * com.android.systemui) holds *recommendations* only — the daemon ignores it and every app is
 * listed in the manager until you tick it. Tick `system` for framework mode or the notification
 * suppressor; ticking the entry named `android` does not inject into system_server. See README.md.
 *
 * ── What changed in the port ───────────────────────────────────────────────────────────────
 * Only the API surface. Vector's modern API is the same design as libxposed's, so every entry
 * point maps one for one:
 *
 *   io.github.libxposed.api.XposedModule            -> de.wwb.android.wbosed.api.WbModule
 *   io.github.libxposed.api.XposedInterface.Hooker  -> de.wwb.android.wbosed.api.WbInterface.Hooker
 *   XposedModuleInterface.ModuleLoadedParam         -> WbModuleInterface.ModuleLoadedParam
 *   XposedModuleInterface.PackageReadyParam         -> WbModuleInterface.PackageReadyParam
 *   XposedModuleInterface.SystemServerStartingParam -> WbModuleInterface.SystemServerStartingParam
 *
 * `hook(...).intercept(...)`, `Chain.proceed()`, `getRemotePreferences(...)`, `log(...)` and the
 * `Chain.args` / `Chain.thisObject` accessors are identical in both, so the hook bodies are
 * untouched. The one behavioural difference worth knowing: Vector's `param.processName` on
 * ModuleLoadedParam is the same value upstream reports, and `isSystemServer` likewise.
 *
 * The upstream class-level commentary on why the guards below exist is kept verbatim where it
 * still applies, because those guards encode measured failures rather than preferences.
 */
class DuckUsbModule : WbModule() {

    companion object {
        private const val TAG = "DuckUSB"

        /** Setting keys we force to the "disabled" value. */
        private val SPOOF_KEYS = setOf(
            "adb_enabled",                  // Settings.Global.ADB_ENABLED — USB debugging
            "adb_wifi_enabled",             // wireless / ADB-over-Wi-Fi
            "development_settings_enabled"  // Developer Options master toggle
        )

        /**
         * Core *processes* we never spoof inside. Guarding on package name alone is not enough:
         * OEM plugins load into com.android.systemui under their own package names at an app
         * uid (10178 on OPlus), which no package-name list catches.
         */
        private val SKIP_SPOOF_PROCESSES = setOf(
            "android",
            "system",
            "com.android.systemui",
            "com.android.settings",
            "com.android.shell",
            "com.android.phone",
        )

        /** Core packages the SETTINGS SPOOF never touches (the notif suppressor still may). */
        private val SKIP_SPOOF_PACKAGES = setOf(
            "android",
            "com.android.settings",
            "com.android.systemui",
            "com.android.shell",
            "com.android.phone"
        )

        /**
         * Settings 欺骗的 hook 面，故意与 rustFrida 验证脚本 `rf-xff-all-spoof.js` 同形：
         * 只挂检测方真正调用的**具体 overload**，不挂隐藏漏斗，也不挂 getFloat / getLong。
         *
         * 脚本原文（能过 XFF 且从不卡）：
         * ```js
         * var G = Java.use("android.provider.Settings$Global");
         * G.getInt.overload("android.content.ContentResolver", "java.lang.String", "int").impl =
         *     function (cr, name, def) {
         *         if (String(name) === "adb_enabled") return 0;   // 命中直接返回，不调原方法
         *         return this.$orig(cr, name, def);
         *     };
         * ```
         *
         * 为什么不能挂 getFloat（本轮 hunter 卡死的直接原因，实测 2026-09-15）：
         * 挂上 getFloat 后主线程栈为
         *   sun.misc.FloatingDecimal.readJavaFormatString ← java.lang.Float.parseFloat
         *   ← android.provider.Settings$Global.getFloat(Settings.java:15399)
         *   ← HookBridge.invokeOriginalMethod ← … ← settingsHooker(chain.proceed())
         *   ← Settings$Global.getFloat(Native method)
         *   ← Material progressindicator.onWindowVisibilityChanged
         * 而 `settings get global animator_duration_scale` 的真值是正常的 "1.0"，
         * state=R Runnable / stm=3652ms（系统时间为主 = 反复缺页，不是纯 CPU 死循环），
         * 即值没问题，是「经 hook 的 passthrough 调原方法」这条路径把原方法跑飞了。
         * 脚本一个 getFloat 都没挂，所以不存在这个症状。
         *
         * 为什么不挂隐藏漏斗 `getStringForUser`：漏斗的可挂性按进程漂移
         * （实测 hunter 首次安装 `Global=0`、后续 `Global=1`；xff 全程 `Global=0`），
         * 一旦漏掉就是整个类的读取未被拦（Settings 与属性源矛盾 → 被检测）。
         */
        private val SETTINGS_TARGETS: List<Triple<String, String, Array<Class<*>>>> = buildList {
            // The 2-arg getInt(cr, name) is hooked on purpose and must stay: MIUI/AOSP never route
            // it through the 3-arg overload, so a 3-arg-only hook is installed but never fires.
            // Verified on MIUI 13 / Android 12: with only the 3-arg overload installed,
            // settingsHooker logged zero hits while the target read adb_enabled as 1 (FOUND).
            for (cls in arrayOf("android.provider.Settings\$Global", "android.provider.Settings\$Secure")) {
                add(Triple(cls, "getInt",
                    arrayOf(ContentResolver::class.java, String::class.java)))
                add(Triple(cls, "getInt",
                    arrayOf(ContentResolver::class.java, String::class.java, Integer.TYPE)))
                add(Triple(cls, "getString",
                    arrayOf(ContentResolver::class.java, String::class.java)))
                add(Triple(cls, "getString",
                    arrayOf(ContentResolver::class.java, String::class.java, String::class.java)))
            }
        }

        /** First application UID; anything below (root/system/shell) is never lied to. */
        private const val FIRST_APP_UID = 10000

        /** SettingsProvider.call methods that fetch a value we may want to spoof. */
        private val GET_METHODS = setOf("GET_global", "GET_secure")

        /** The concrete settings provider we hook (never the generic ContentProvider$Transport). */
        private const val SETTINGS_PROVIDER = "com.android.providers.settings.SettingsProvider"

        /** Authority is the portable identity; the class name is only a fallback. */
        private const val SETTINGS_AUTHORITY = "settings"

        /** Bundle keys used by the settings-provider call protocol. */
        private const val CALL_VALUE = "value"                 // Settings.NameValueTable.VALUE
        private const val CALL_GENERATION_INDEX = "_generation_index" // CALL_METHOD_GENERATION_INDEX_KEY

        /**
         * A7) `android.hardware.usb.action.USB_STATE` — the sticky broadcast whose extras carry
         * the USB gadget's *applied* state. Spelled as a literal instead of
         * `UsbManager.ACTION_USB_STATE` so nothing here depends on a hidden field, and so the
         * extra names below can be listed next to it.
         */
        private const val USB_STATE_ACTION = "android.hardware.usb.action.USB_STATE"

        /** Extra names used by `UsbDeviceManager.makeUsbStateIntent()` (see the A7 note below). */
        private const val USB_EXTRA_CONNECTED = "connected"
        private const val USB_EXTRA_CONFIGURED = "configured"
        private const val USB_EXTRA_ADB = "adb"
        private const val USB_EXTRA_MTP = "mtp"

        /**
         * The adb function-filesystem flag: `1` while the adb function is linked into the
         * gadget. Read by A7 only as a *veto* (see [usbAdbFunctionLinked]). Unreadable from an
         * untrusted app on some ROMs, which is exactly why a read failure must not veto.
         */
        private const val SYSFS_FFS_ADB = "/sys/android_usb/android0/ffs/adb"

        /** Notification channels the ADB notifications live on (AOSP). */
        private val ADB_CHANNELS = setOf("DEVELOPER", "DEVELOPER_IMPORTANT")

        /** `TelephonyManager.SIM_STATE_READY` — the value that reads as "a SIM is present". */
        private const val SIM_STATE_READY = 5
    }

    // ------------------------------------------------------------------ framework plumbing

    /**
     * Remote preferences, written by the UI through the framework service.
     *
     * This is what replaces XSharedPreferences and its world-readable file. It is not merely a
     * different spelling: the old file had to be readable by every hooked process including
     * system_server, and a module that "looks enabled but does nothing" is exactly what an
     * unreadable prefs file produces. Vector brokers the value and pushes updates rather than
     * making every hook body re-read a file — which is why there is no `reload()` below.
     *
     * Lazy because [getRemotePreferences] requires the framework to be attached, which happens
     * after construction.
     */
    private val prefs: SharedPreferences by lazy { getRemotePreferences(Config.PREFS_NAME) }

    /** Process name of the process this module instance lives in; set in [onModuleLoaded]. */
    private var processName: String = ""

    /** Verbose per-injection logging; off unless troubleshooting. */
    private fun verboseOn() = prefs.getBoolean(Config.KEY_VERBOSE_LOG, false)

    /** Device-state spoof (keyguard / telephony). Independent of the USB-debugging toggle. */
    private fun deviceStateSpoofOn() = prefs.getBoolean(Config.KEY_DEVICE_STATE_SPOOF, true)

    /** Master pause. Checked by every hook body; the service copy wins when it is live. */
    private fun pausedOn() = prefs.getBoolean(Config.KEY_PAUSED, false)

    private fun spoofOn() = prefs.getBoolean(Config.KEY_SPOOF, true)

    private fun hideNotifOn() = prefs.getBoolean(Config.KEY_HIDE_NOTIF, true)

    /**
     * FLAG_DEBUGGABLE 清位（rustFrida rf-xff-all-spoof.js (2) 的 Vector 等效实现）。
     * 只作用于 spoof 分支：同 scope、同 uid/process/package 守门，system 进程永不受影响。
     */
    private fun debuggableSpoofOn() = prefs.getBoolean(Config.KEY_DEBUGGABLE_SPOOF, true)

    /**
     * Framework mode is EXPERIMENTAL and OFF by default: upstream measured that hooking the
     * settings provider transport in system_server bootloops some ROMs. The per-app client hook
     * is the safe default.
     */
    private fun frameworkModeOn() = prefs.getBoolean(Config.KEY_FRAMEWORK_MODE, false)

    private fun clientFallbackOn() = prefs.getBoolean(Config.KEY_CLIENT_FALLBACK, true)

    private fun logI(msg: String) = log(Log.INFO, TAG, msg)

    private fun logE(msg: String, t: Throwable? = null) =
        if (t == null) log(Log.ERROR, TAG, msg) else log(Log.ERROR, TAG, msg, t)

    /** `XposedHelpers.findClassIfExists` has no modern counterpart; this is what it did. */
    private fun findClass(name: String, cl: ClassLoader): Class<*>? =
        try { Class.forName(name, false, cl) } catch (_: Throwable) { null }

    /**
     * Stand-in for `XposedBridge.hookAllMethods`: hook every declared overload of [name].
     * Returns how many were hooked, which is the only honest way to tell "the class was there
     * but the method was not" from "it worked".
     */
    private fun hookAll(clazz: Class<*>, name: String, hooker: WbInterface.Hooker): Int {
        var count = 0
        for (m in clazz.declaredMethods) {
            if (m.name != name) continue
            try {
                hook(m).intercept(hooker)
                count++
            } catch (t: Throwable) {
                logE("hook failed ${clazz.name}.$name", t)
            }
        }
        return count
    }

    // ------------------------------------------------------------------ entry points

    override fun onModuleLoaded(param: ModuleLoadedParam) {
        processName = param.processName
        // 必须先于一切 hook 安装：Config 的三个惰性集合（listOf/setOf/mapOf）首次校验若发生
        // 在 loader 替换中途，会被本模块自己的 hook 拖成 8.78s（0.57 bytecodes/s，实测
        // hunter 主线程被卡死 → 输入超时 ANR + MIUI scout HANG）。此处预热让校验以正常速度完成。
        Config.warmUp()
        if (verboseOn()) {
            logI("loaded proc=${param.processName} systemServer=${param.isSystemServer} " +
                "uid=${android.os.Process.myUid()} framework=$frameworkName($frameworkVersionCode) API $apiVersion")
        }
    }

    /**
     * system_server. Under the legacy API this was `pkg == "android"`, reached through a
     * package-name comparison that was never dependable; the framework states it outright now.
     */
    override fun onSystemServerStarting(param: SystemServerStartingParam) {
        val cl = param.classLoader
        // Installation follows the toggle: hooks are placed only for the feature that asked
        // for them, so "framework mode only" does not also leave two hooks on
        // NotificationManagerService.
        if (hideNotifOn()) {
            installNotificationSuppressor(cl)
            installSystemServerSuppressor(cl)
        }
        // A0) Framework mode (default off): one server-side hook covers the Settings spoof for
        //     EVERY app — no per-app scope. Gated per-caller by UID so shell/system still see
        //     the truth.
        if (frameworkModeOn()) installFrameworkSettingsSpoof(cl)
    }

    /**
     * Every non-system_server package. [PackageReadyParam] rather than `onPackageLoaded` on
     * purpose: it hands over the app's real classloader (the one a custom AppComponentFactory
     * may have swapped in), and it is not gated on API 29 the way `onPackageLoaded` is, so
     * this keeps working down to the module's minSdk.
     */
    override fun onPackageReady(param: PackageReadyParam) {
        // 兑底（onModuleLoaded 未跑的路径）：确保 warmUp 先于下面的 loader 重建逻辑。
        Config.warmUp()
        val pkg = param.packageName
        val cl = param.classLoader
        val myUid = android.os.Process.myUid()

        if (verboseOn()) {
            logI("package pkg=$pkg proc=$processName uid=$myUid " +
                "skipList=${pkg in SKIP_SPOOF_PACKAGES} systemUid=${myUid % 100000 < FIRST_APP_UID}")
        }

        // B) Notification suppressor — System UI is the other place the ADB notification can
        //    surface. (system_server is handled in onSystemServerStarting.)
        if (pkg == "com.android.systemui" && hideNotifOn()) installNotificationSuppressor(cl)

        // A) Settings spoof (per-app, client side) — the default layer, off only when framework
        //    mode is on. Never in the core OS packages.
        //
        // Guard on UID, not just package name. The property spoof claims sys.usb.state=mtp and
        // init.svc.adbd=stopped — the USB stack's actual control surface, not detection
        // cosmetics — so reaching a system process with it kills the gadget outright (no MTP
        // *and* no adb, charge-only). Any uid < 10000 is OS, never spoof it. Three independent
        // guards, because each alone has a proven gap:
        //   uid       — catches every system-uid injection
        //   process   — catches OEM keyguard plugins riding com.android.systemui at an app uid
        //   package   — the original list, kept for the plain per-app cases
        val isSystemProcess = myUid % 100000 < FIRST_APP_UID
        val inCoreProcess = processName.ifEmpty { pkg } in SKIP_SPOOF_PROCESSES
        // ...and never in DuckUSB itself. The framework loads a module into its own process
        // whether or not you scope it, so without this the UI spoofs itself: the readings card,
        // which exists to report the REAL device state, reads its own lie. The native half is
        // worse than dishonest — the libc inline hook is what SIGILLs the UI on some ROMs
        // (upstream issue #2).
        val isSelf = pkg == Config.PKG || processName.substringBefore(':') == Config.PKG
        if (pkg !in SKIP_SPOOF_PACKAGES && !isSystemProcess && !inCoreProcess && !isSelf) {
            if (clientFallbackOn()) installSettingsSpoof(cl)
            // A2) Property spoof — same scope. Closes the gap where a detector reads the raw
            //     sys.usb.* / init.svc.adbd props instead of the Settings provider.
            // Never spoof properties to the OS's own file-transfer plumbing, even if the user
            // scopes com.android.mtp directly: it runs at an app uid, so the guard above cannot
            // catch it.
            if (pkg in Config.SPARE_PACKAGES) {
                if (verboseOn()) logI("property spoof SPARED for $pkg (OS file-transfer plumbing)")
            } else {
                installSystemPropertiesSpoof(cl)
                installNativePropSpoof()
                // A4) FLAG_DEBUGGABLE 清位(rustFrida 脚本 (2) 的等效实现,同 scope/guard)。
                installDebuggableSpoof(param)
                // A3) Covers the one read path the two layers above cannot reach.
                installGetpropExecSpoof(cl)
                // A7) USB 运行时粘性广播 —— 唯一不由 Settings/属性表达的 ADB 证据。
                installUsbStateSpoof(cl)
            }
            // A5) Provider transport. Layer A hangs the static Settings getters, but a detector
            //     can name the provider instead and never touch them: query() on
            //     content://settings/global, or call(uri, "GET_global", key, Bundle). Both are
            //     covered here, client-side, so no framework-mode hook is required.
            installProviderReadSpoof(cl)
            // A6) Device-state reads the same detector cross-checks (keyguard / telephony).
            installDeviceStateSpoof(cl)
        }
    }

    // ============================ A) SETTINGS SPOOF ============================

    private fun installSettingsSpoof(cl: ClassLoader) {
        var total = 0
        val detail = StringBuilder()
        for ((className, methodName, params) in SETTINGS_TARGETS) {
            val short = className.substringAfterLast('$')
            val settings = findClass(className, cl)
            if (settings == null) {
                detail.append(" ").append(short).append("=NOT_FOUND")
                continue
            }
            val target = try {
                settings.getDeclaredMethod(methodName, *params)
            } catch (_: Throwable) {
                null
            }
            if (target == null) {
                // 具体 overload 不存在（厂商改动）——只影响这一项，不再牵连整类。
                detail.append(" ").append(short).append('.').append(methodName)
                    .append('/').append(params.size).append("=ABSENT")
                continue
            }
            var ok = 0
            try {
                hook(target).intercept(settingsHooker)
                ok = 1
            } catch (t: Throwable) {
                logE("hook failed ${className}.${methodName}/${params.size}", t)
            }
            total += ok
            detail.append(" ").append(short).append('.').append(methodName)
                .append('/').append(params.size).append('=').append(ok)
        }
        // 不受 verbose 门控：安装计数是「overload 不存在」与「hook 未装上」的唯一分界线。
        logI("Settings spoof: hooks installed=$total detail:$detail")
    }

    /**
     * One interceptor replaces the old before/after pair: returning a value without calling
     * [WbInterface.Chain.proceed] is what `param.result = …` in `beforeHookedMethod` meant.
     */
    private val settingsHooker = WbInterface.Hooker { chain ->
        // Cheap key check FIRST — this runs on every Settings getter, so do the work only for
        // the few keys we actually spoof.
        val key = chain.args.firstOrNull { it is String } as? String
        if (key == null || key !in SPOOF_KEYS || pausedOn() || !spoofOn()) {
            if (key != null && key in SPOOF_KEYS) logI("settingsHooker PASS-THROUGH key=$key " +
                    "paused=${pausedOn()} spoof=${spoofOn()}")
            chain.proceed()
        } else {
            val rv = (chain.executable as? Method)?.returnType?.name ?: "?"
            logI("settingsHooker SPOOF key=$key ret=$rv")
            when ((chain.executable as? Method)?.returnType) {
                java.lang.Long.TYPE -> 0L
                java.lang.Float.TYPE -> 0f
                java.lang.Integer.TYPE -> 0
                String::class.java -> "0"
                else -> 0
            }
        }
    }

    // ================= A0) FRAMEWORK-MODE SETTINGS SPOOF (system_server) =================

    /** The system_server-side service; null until SettingsProvider attaches. */
    @Volatile
    private var service: DuckService? = null

    /** One-shot guard so we hook SettingsProvider.call only once. */
    private var settingsCallHooked = false

    /**
     * Server-side settings spoof, done the SAFE way: hook ContentProvider.attachInfo, wait for
     * the concrete SettingsProvider to attach, then hook ONLY its call(). We deliberately do NOT
     * hook the generic ContentProvider$Transport.call — that fires on every provider IPC in
     * system_server on the hottest boot path and BOOTLOOPS some ROMs (verified upstream on
     * OP15 / Android 16). Gated per-caller by UID so shell/system keep the real state and adb
     * stays functional.
     */
    private fun installFrameworkSettingsSpoof(cl: ClassLoader) {
        val cp = findClass("android.content.ContentProvider", cl)
        if (cp == null) {
            logE("ContentProvider not found; framework mode disabled")
            return
        }
        val attachInfo = try {
            cp.getDeclaredMethod("attachInfo",
                Context::class.java, android.content.pm.ProviderInfo::class.java)
        } catch (t: Throwable) {
            logE("ContentProvider.attachInfo not found; framework mode disabled", t)
            return
        }
        try {
            hook(attachInfo).intercept { chain ->
                val result = chain.proceed()
                try {
                    val provider = chain.thisObject
                    // Match on the declared authority, not the class name. A ROM that
                    // subclasses or renames SettingsProvider would fail an equality check and
                    // the hook would silently never install — the same look-enabled /
                    // do-nothing failure that is so hard to diagnose from the UI.
                    // Authority can be a ";"-separated list.
                    val info = chain.args.getOrNull(1) as? android.content.pm.ProviderInfo
                    val isSettings =
                        info?.authority?.split(";")?.any { it.trim() == SETTINGS_AUTHORITY } == true ||
                        provider?.javaClass?.name == SETTINGS_PROVIDER
                    if (isSettings && provider != null) {
                        // attachInfo(Context, ProviderInfo) hands us the system Context
                        // directly — no reflection on a private mContext field needed.
                        (chain.args.getOrNull(0) as? Context)?.let { ctx ->
                            if (service == null) service = DuckService(ctx).apply {
                                spoofSettings = spoofOn()
                                hideNotif = hideNotifOn()
                            }
                        }
                        hookSettingsProviderCall(provider.javaClass)
                    }
                } catch (_: Throwable) {}
                result
            }
            logI("framework mode armed — hooked ContentProvider.attachInfo (awaiting SettingsProvider)")
        } catch (t: Throwable) {
            logE("framework settings spoof (attachInfo) failed", t)
        }
    }

    /** Reflectively hook only SettingsProvider.call (>=3 params). One-shot. */
    private fun hookSettingsProviderCall(spClass: Class<*>) {
        if (settingsCallHooked) return
        var count = 0
        for (m in spClass.declaredMethods) {
            try {
                if (m.name == "call" && m.parameterTypes.size >= 3) {
                    hook(m).intercept(frameworkCallHooker)
                    count++
                }
            } catch (_: Throwable) {}
        }
        if (count > 0) settingsCallHooked = true
        service?.hookCount = count
        service?.installedAtRealtimeMs = SystemClock.elapsedRealtime()
        logI("framework settings spoof installed: SettingsProvider.call hooks=$count")
    }

    private val frameworkCallHooker = WbInterface.Hooker { chain ->
        // Capture the remote caller's UID while the binder identity is still set on entry.
        // The legacy version needed a ThreadLocal to carry this from beforeHookedMethod to
        // afterHookedMethod; one interceptor spans both, so the ThreadLocal is gone.
        val uid = try { Binder.getCallingUid() } catch (_: Throwable) { null }

        // Binder bridge: hand the service to our own app, riding this same hook so the
        // framework half adds no extra hook surface. On any mismatch we fall through
        // untouched, so the call is indistinguishable from stock (unknown method -> null).
        val bridged: Bundle? = try {
            val svc = service
            val args = chain.args
            if (svc != null && uid != null && args.size >= 2 &&
                args[0] == Bridge.METHOD && args[1] == Bridge.ARG &&
                uid % 100000 == svc.callerAppId
            ) Bundle().apply { putBinder(Bridge.KEY_BINDER, svc) } else null
        } catch (_: Throwable) { null }

        if (bridged != null) {
            bridged
        } else {
            val result = chain.proceed()
            try {
                spoofSettingsCall(uid, chain.args, result)
            } catch (_: Throwable) {}
            result
        }
    }

    /** The "after" half of the framework call hook: rewrite the value the provider returned. */
    private fun spoofSettingsCall(uid: Int?, args: List<Any?>, result: Any?) {
        if (uid == null) return
        // Only real apps get lied to; root/system/shell (uid<10000 in any user) see truth.
        if (uid % 100000 < FIRST_APP_UID) return

        // ...and never our own UI. The readings card exists to report the REAL device state,
        // and the client-side self-guard cannot stop a lie told inside system_server: with
        // framework mode on, DuckUSB read its own adb_enabled as 0 on a device where it is 1.
        // Spoofing ourselves buys nothing — detectors are other apps — and costs the one screen
        // meant to tell the user the truth. appId -1 means unknown, so spoof.
        service?.callerAppId?.let { if (it >= 0 && uid % 100000 == it) return }

        // The setting key always immediately follows the GET_* method arg, whatever the
        // SettingsProvider.call signature is on this Android version.
        var key: String? = null
        for (i in 0 until args.size - 1) {
            val a = args[i]
            if (a is String && a in GET_METHODS) {
                key = args[i + 1] as? String
                break
            }
        }
        if (key == null || key !in SPOOF_KEYS) return
        // Never lie to the OS's own file-transfer plumbing: com.android.mtp and friends sit at
        // app uids, so the uid<10000 rule above does not cover them, and spoofing them at boot
        // leaves USB stuck on charge-only.
        if (service?.isSpared(uid) == true) return
        if (service?.paused ?: pausedOn()) return
        // Live config from the service when it's up (a volatile read), else the prefs path.
        if (!(service?.spoofSettings ?: spoofOn())) return

        val bundle = result as? Bundle ?: return
        if (bundle.containsKey(CALL_VALUE)) {
            bundle.putString(CALL_VALUE, "0")
            service?.note(uid, key)
            // Make the client NameValueCache treat this as uncacheable (-1) so our hook runs on
            // every read instead of a stale real value being served from cache.
            bundle.putInt(CALL_GENERATION_INDEX, -1)
        }
    }

    // ===================== A2) PROPERTY SPOOF (Java layer) =====================

    /**
     * Hook android.os.SystemProperties.native_get* so apps using SystemProperties.get()/
     * getInt()/getBoolean() see our overrides. Checked live against the toggle.
     */
    private fun installSystemPropertiesSpoof(cl: ClassLoader) {
        val sp = findClass("android.os.SystemProperties", cl) ?: return
        for (m in arrayOf("native_get", "native_get_int", "native_get_long", "native_get_boolean")) {
            hookAll(sp, m, systemPropertiesHooker)
        }
    }

    private val systemPropertiesHooker = WbInterface.Hooker { chain ->
        // Cheap key check FIRST — SystemProperties.get() is extremely hot.
        val key = chain.args.firstOrNull() as? String
        val value = if (key == null) null else Config.PROP_OVERRIDES[key]
        // native_get returns String; the int/long/boolean variants need a parseable value.
        // Our USB props ("mtp") aren't numeric, so only substitute when it fits the type.
        val spoofed: Any? = if (value == null || pausedOn()) null else
            when ((chain.executable as? Method)?.returnType) {
                String::class.java -> value
                java.lang.Integer.TYPE -> value.toIntOrNull()
                java.lang.Long.TYPE -> value.toLongOrNull()
                java.lang.Boolean.TYPE -> when (value) { "1", "true" -> true; "0", "false" -> false; else -> null }
                else -> null
            }
        spoofed ?: chain.proceed()
    }

    // ===================== A2) PROPERTY SPOOF (native libc layer) =====================

    /**
     * Install the libc __system_property_get / __system_property_read(_callback) hooks
     * (libvector_duckusb.so). The hooks themselves are installed by native_init during the
     * module's load; this call only (re)publishes the override map. Installed once per process;
     * an empty map (spoof off) makes the hooks pass through.
     */
    private fun installNativePropSpoof() {
        try {
            // The native hook installs once per process and cannot be re-gated live, so pause
            // is applied at load: an empty map makes the libc hooks pass through.
            NativeProps.install(if (pausedOn()) emptyMap() else Config.PROP_OVERRIDES)
        } catch (t: Throwable) {
            logE("native prop hook install failed", t)
        }
    }

    // ============ A4) FLAG_DEBUGGABLE SPOOF (rustFrida script equivalent) ============

    /**
     * Clear ApplicationInfo.FLAG_DEBUGGABLE in scoped apps — the Vector port of
     * rf-xff-all-spoof.js section (2) (clearDbg + ContextImpl.getApplicationInfo hook +
     * Application.attach hook + a one-shot pass over the current application).
     *
     * A detector reading `context.applicationInfo.flags` decides whether an "attach to
     * debuggable app" branch of its verdict is reachable. With the bit cleared in the object
     * the framework hands out, the branch reads false and the verdict falls to the safe path,
     * without the real APK's manifest being touched.
     *
     * Installed inside the spoof branch of [onPackageReady] (same uid / process / package
     * guards as the settings + property spoofs); gated live by [debuggableSpoofOn] and the
     * master pause, matching every other hook here.
     */
    private fun installDebuggableSpoof(param: PackageReadyParam) {
        val cl = param.classLoader

        // 1) 主路径（Vector 专有，优于脚本）：框架把该包的 ApplicationInfo 实例直接交给我们。
        //    它就是 LoadedApk.mApplicationInfo —— 进程内 context.getApplicationInfo() 最终都
        //    返回同一个对象，清一次即永久生效，不需要 hook 任何 getter。
        clearDebuggableFlag("PackageReady.applicationInfo",
            try { param.applicationInfo } catch (_: Throwable) { null })

        // 2) 兜底一：Application.attach 时再清一次（部分 ROM 在 attach 里重新赋值）。
        findClass("android.app.Application", cl)?.let { hookAll(it, "attach", appAttachHooker) }

        // 3) 兜底二：ContextImpl.getApplicationInfo。该 @hide 类在部分环境被隐藏 API 名单
        //    挡住（findClass 返回 null 属预期），拿得到就多兜一层，拿不到不影响主路径。
        val cimpl = findClass("android.content.ContextImpl", cl)
        if (cimpl != null) {
            hookAll(cimpl, "getApplicationInfo", debuggableHooker)
        } else if (verboseOn()) {
            logI("debuggable spoof: ContextImpl unavailable (hidden-API), relying on param + attach")
        }
        logI("debuggable spoof installed (param primary; attach/ContextImpl fallback)")
    }

    /** Reusable clear body: logs only when the bit actually changes. */
    private fun clearDebuggableFlag(tag: String, ai: ApplicationInfo?) {
        if (ai == null) return
        if (pausedOn() || !debuggableSpoofOn()) return
        val before = ai.flags
        if (before and ApplicationInfo.FLAG_DEBUGGABLE != 0) {
            ai.flags = before and ApplicationInfo.FLAG_DEBUGGABLE.inv()
            logI("debuggableSpoof[$tag]: flags 0x${before.toString(16)} → 0x${ai.flags.toString(16)}")
        }
    }

    private val debuggableHooker = WbInterface.Hooker { chain ->
        // proceed first so we mutate the SAME ApplicationInfo the caller will receive.
        val result = try { chain.proceed() } catch (t: Throwable) {
            logE("debuggableHooker proceed failed", t); throw t
        }
        try {
            clearDebuggableFlag("ContextImpl.getApplicationInfo", result as? ApplicationInfo)
        } catch (t: Throwable) { logE("debuggableHooker clear failed", t) }
        result
    }

    private val appAttachHooker = WbInterface.Hooker { chain ->
        val result = try { chain.proceed() } catch (t: Throwable) {
            logE("appAttachHooker proceed failed", t); throw t
        }
        try {
            val self = chain.thisObject as? Application
            if (self != null) clearDebuggableFlag("Application.attach", self.applicationInfo)
        } catch (t: Throwable) { logE("appAttachHooker clear failed", t) }
        result
    }

    // ================= A3) GETPROP EXEC SPOOF (out-of-process reads) =================

    /** Line shape of a getprop dump entry: `[persist.sys.usb.config]: [adb]`. */
    private val GETPROP_LINE = Regex("""^\[(.+?)]\s*:\s*\[(.*)]\s*$""")

    /**
     * Hook java.lang.ProcessBuilder.start() — the single Java-level chokepoint for every exec
     * in this process (Runtime.exec included). Only getprop commands are wrapped; every other
     * command returns the real Process untouched, so the detector's other probes (ls, mount,
     * getenforce, which su, logcat) are byte-for-byte unaffected.
     *
     * Two command shapes exist in the wild and both are handled:
     *   ["getprop"]        full dump, lines of `[key]: [value]`  (SystemPropertyReadUtils,
     *                      PlayIntegrityPropertyReadUtils)
     *   ["getprop", key]   single value, bare text                 (CustomRomPropertyReader)
     *
     * No recursion: proceed() runs the original start() once and we only wrap its result; the
     * wrapper never spawns anything.
     */
    private fun installGetpropExecSpoof(cl: ClassLoader) {
        val pb = findClass("java.lang.ProcessBuilder", cl)
        if (pb == null) {
            logE("ProcessBuilder not found; getprop exec spoof disabled")
            return
        }
        val n = hookAll(pb, "start", getpropExecHooker)
        if (verboseOn()) logI("getprop exec spoof armed: ProcessBuilder.start hooks=$n")
    }

    private val getpropExecHooker = WbInterface.Hooker { chain ->
        val pb = chain.thisObject as? ProcessBuilder ?: return@Hooker chain.proceed()
        // command() is a plain list read, no fork — safe to call inside the hook.
        val cmd = try { pb.command() } catch (_: Throwable) { null }
            ?: return@Hooker chain.proceed()
        // Cheap basename check FIRST: this hook sits on the exec chokepoint of the whole
        // process, so the common case (not getprop) must be two string compares.
        val argv0 = cmd.firstOrNull() as? String
        if (argv0 == null || argv0.substringAfterLast('/') != "getprop") {
            return@Hooker chain.proceed()
        }

        val real = chain.proceed() as? Process ?: return@Hooker chain.proceed()
        // Live gate, same policy as the property layers: master pause only, and an unhooked
        // pause means the REAL values flow, which keeps all sources consistent while paused.
        if (pausedOn()) return@Hooker real

        when {
            cmd.size == 1 -> {
                if (verboseOn()) logI("getprop dump wrapped (${Config.PROP_OVERRIDES.size} keys)")
                FilteredProcess(real) { dump -> filterGetpropDump(dump) }
            }
            cmd.size == 2 -> {
                val fake = (cmd[1] as? String)?.let { Config.PROP_OVERRIDES[it] }
                if (fake == null) {
                    // Key we don't override (e.g. ro.build.version.sdk): real Process, zero
                    // wrapper overhead, output byte-identical.
                    real
                } else {
                    if (verboseOn()) logI("getprop single wrapped: ${cmd[1]}")
                    FilteredProcess(real) { fake + "\n" }
                }
            }
            // Flags or exotic forms we don't model: pass through untouched.
            else -> real
        }
    }

    /**
     * Rewrite the `[key]: [value]` lines of a getprop dump for the keys we override. Every
     * other line — including its order and formatting — passes through unchanged, so the
     * parsed hit count and the rest of the report stay exactly as plausible as before.
     */
    private fun filterGetpropDump(dump: String): String =
        dump.lineSequence().joinToString("\n") { line ->
            val m = GETPROP_LINE.matchEntire(line)
            val fake = m?.let { Config.PROP_OVERRIDES[it.groupValues[1]] }
            if (m == null || fake == null) line else "[${m.groupValues[1]}]: [$fake]"
        }

    /**
     * A Process view over the real getprop child whose stdout is the transformed text.
     * Everything else delegates 1:1 — exit code, waitFor(timeout), destroy()/destroyForcibly(),
     * isAlive, pid — so the caller's timeout and cleanup semantics are indistinguishable from
     * stock.
     */
    private class FilteredProcess(
        private val real: Process,
        transform: (String) -> String,
    ) : Process() {
        // Drained lazily on first read: the callers read the stream before waitFor(), so this
        // is indistinguishable from reading the real stream. getprop output is far below the
        // 64 KiB pipe buffer, so even a waitFor()-first caller cannot deadlock the child.
        private val stdout: InputStream by lazy {
            val raw = real.inputStream.use { it.readBytes() }.toString(Charsets.UTF_8)
            ByteArrayInputStream(transform(raw).toByteArray(Charsets.UTF_8))
        }

        override fun getOutputStream(): OutputStream = real.outputStream
        override fun getInputStream(): InputStream = stdout
        override fun getErrorStream(): InputStream = real.errorStream
        override fun waitFor(): Int = real.waitFor()
        override fun waitFor(timeout: Long, unit: TimeUnit): Boolean =
            real.waitFor(timeout, unit)
        override fun exitValue(): Int = real.exitValue()
        override fun destroy() { real.destroy() }
        // Java signature returns Process, not void — Kotlin override must match.
        override fun destroyForcibly(): Process = real.destroyForcibly()
        override fun isAlive(): Boolean = real.isAlive
        // NOTE: java.lang.Process on this compile classpath predates pid()/onExit()/toHandle()
        // /supportsNormalTermination(), so they are neither overridden nor implemented here. No
        // caller in scope uses them (the detector reads the stream, waits and destroys); a
        // future caller would need reflection-based delegation, not new overrides.
    }

    // ============ A5) PROVIDER READ SPOOF (query + call, client side) ============

    /**
     * riskdetector reaches Settings through the provider transport directly, bypassing the
     * static getters layer A hooks entirely:
     *
     *   [Content Provider] `content://settings/global`, 6-arg query:
     *       resolver.query(uri, null, null, null, null, null)  → scans the `name`/`value`
     *       rows for "adb_enabled"
     *   [Content Call] `content://settings/global`, "GET_global":
     *       resolver.call(uri, "GET_global", "adb_enabled", Bundle()) → reads Bundle "value"
     *
     * Layer A never fires for either — the getters are simply not called. The framework-mode
     * hook (A0, SettingsProvider.call) covers the call form but lives in system_server behind an
     * opt-in and is off by default; it does not cover the query form at all. These two hooks
     * close both per-app, with no server-side requirement.
     *
     * `query` has many overloads and both the 5-arg and the 6-arg (CancellationSignal) forms are
     * reachable, so every declared overload is hooked and the body filters by URI. The
     * CancellationSignal package is `android.os` — `android.database.CancellationSignal` does not
     * exist, and naming it silently matches nothing.
     */
    private fun installProviderReadSpoof(cl: ClassLoader) {
        val cr = findClass("android.content.ContentResolver", cl)
        if (cr == null) {
            logE("ContentResolver not found; provider read spoof disabled")
            return
        }
        val nq = hookAll(cr, "query", providerQueryHooker)
        val nc = hookAll(cr, "call", providerCallHooker)
        logI("provider read spoof: ContentResolver.query hooks=$nq call hooks=$nc")
    }

    private val providerQueryHooker = WbInterface.Hooker { chain ->
        val result = chain.proceed()
        if (pausedOn() || !spoofOn()) return@Hooker result
        try {
            val uri = chain.args.firstOrNull { it is Uri } as? Uri
            rewriteSettingsCursor(uri, result as? Cursor) ?: result
        } catch (t: Throwable) {
            logE("providerQueryHooker failed", t)
            result
        }
    }

    /**
     * Rebuild a settings cursor with our values substituted. Only a `content://settings*` URI
     * carrying the provider's `name`/`value` shape is touched; anything else — including a cursor
     * without those columns — is returned untouched, so an unrelated query stays byte-identical.
     *
     * When nothing matched, the original cursor is rewound to -1 and returned as-is, so its
     * identity and laziness are preserved; only a real hit pays for the materialised copy.
     */
    private fun rewriteSettingsCursor(uri: Uri?, cur: Cursor?): Cursor? {
        if (cur == null) return null
        val u = uri?.toString() ?: return cur
        if (!u.contains("settings")) return cur
        val cols = cur.columnNames ?: return cur
        val iName = cur.getColumnIndex("name")
        val iVal = cur.getColumnIndex("value")
        if (iName < 0 || iVal < 0) return cur

        val rows = ArrayList<Array<Any?>>()
        var hits = 0
        // The cursor arrives fresh (position -1); rewind explicitly because a caller may have
        // handed us one it already walked, and our scan must not inherit its position.
        cur.moveToPosition(-1)
        while (cur.moveToNext()) {
            val row = arrayOfNulls<Any?>(cols.size)
            for (i in cols.indices) row[i] = cur.getString(i)
            val key = row[iName] as? String
            if (key != null && key in SPOOF_KEYS) {
                row[iVal] = Config.SETTINGS_OFF
                hits++
            }
            rows.add(row)
        }
        if (hits == 0) {
            cur.moveToPosition(-1)
            return cur
        }
        val out = MatrixCursor(cols)
        for (r in rows) out.addRow(r)
        logI("provider query rewritten uri=$u rows=${rows.size} hits=$hits")
        return out
    }

    private val providerCallHooker = WbInterface.Hooker { chain ->
        val result = chain.proceed()
        if (pausedOn() || !spoofOn()) return@Hooker result
        try {
            patchSettingsCallBundle(chain.args, result as? Bundle)
            result
        } catch (t: Throwable) {
            logE("providerCallHooker failed", t)
            result
        }
    }

    /**
     * The provider's call() protocol returns the setting in `Bundle["value"]`; the key is the
     * string right after the GET_* method name, whatever the overload's parameter list looks
     * like on this Android version. Marking the generation index -1 makes the client's
     * NameValueCache treat the answer as uncacheable, so our hook runs on every read instead of
     * a real value being served from cache afterwards — the same thing the framework-mode hook
     * does on the server side.
     */
    private fun patchSettingsCallBundle(args: List<Any?>, b: Bundle?): Boolean {
        if (b == null) return false
        val idx = args.indexOfFirst { it is String && it in GET_METHODS }
        if (idx < 0) return false
        val key = args.getOrNull(idx + 1) as? String ?: return false
        if (key !in SPOOF_KEYS) return false
        if (!b.containsKey(CALL_VALUE)) return false
        b.putString(CALL_VALUE, Config.SETTINGS_OFF)
        b.putInt(CALL_GENERATION_INDEX, -1)
        logI("provider call spoofed method=${args[idx]} key=$key")
        return true
    }

    // ============ A6) DEVICE-STATE SPOOF (keyguard / telephony) ============

    /**
     * The same detector cross-checks device state through APIs that have nothing to do with USB
     * debugging. They are separate checks with their own verdicts, so spoofing only the ADB
     * paths leaves them reporting the real device:
     *
     *   H) 是否有锁屏密码 [KeyguardManager]
     *        if (km.isDeviceSecure() && km.isKeyguardSecure()) NOT_FOUND else FOUND
     *      The sense is inverted — NO password is the risk — so both must read true.
     *      (a missing password is what a freshly-flashed/emulator device has)
     *   I) 插卡检测 [TelephonyManager]  if (5 != tm.getSimState()) FOUND
     *      5 = SIM_STATE_READY. A device with no SIM is the risk.
     *   J) 插卡检测 [SubscriptionManager] ***  if (1 < getActiveSubscriptionInfoCount()) FOUND
     *      The *** marks it "test only, not part of the verdict", so it does not decide anything
     *      today. It is spoofed anyway so every SIM-related reading agrees with getSimState —
     *      otherwise removing the *** later would expose an inconsistency we already know about.
     *      Returns 1 rather than 0 because one ready subscription is what SIM_STATE_READY means.
     *
     * All three are gated by [deviceStateSpoofOn] plus the master pause — deliberately not by
     * [spoofOn]: they answer different questions, and a user may want one without the other.
     */
    private fun installDeviceStateSpoof(cl: ClassLoader) {
        findClass("android.app.KeyguardManager", cl)?.let { km ->
            var n = 0
            for (m in arrayOf("isDeviceSecure", "isKeyguardSecure")) {
                n += hookAll(km, m, keyguardHooker)
            }
            logI("device state spoof: KeyguardManager hooks=$n")
        }
        findClass("android.telephony.TelephonyManager", cl)?.let { tm ->
            // Two overloads exist (getSimState() and the hidden getSimState(int slotIndex));
            // both answer with the same value so the reading cannot depend on which is used.
            val n = hookAll(tm, "getSimState", simStateHooker)
            logI("device state spoof: TelephonyManager.getSimState hooks=$n")
        }
        findClass("android.telephony.SubscriptionManager", cl)?.let { sm ->
            val n = hookAll(sm, "getActiveSubscriptionInfoCount", subscriptionCountHooker)
            logI("device state spoof: SubscriptionManager hooks=$n")
        }
    }

    private val keyguardHooker = WbInterface.Hooker { chain ->
        if (pausedOn() || !deviceStateSpoofOn()) chain.proceed() else true
    }

    private val simStateHooker = WbInterface.Hooker { chain ->
        if (pausedOn() || !deviceStateSpoofOn()) chain.proceed() else SIM_STATE_READY
    }

    private val subscriptionCountHooker = WbInterface.Hooker { chain ->
        if (pausedOn() || !deviceStateSpoofOn()) chain.proceed() else 1
    }

    // ============ A7) USB RUNTIME STATE SPOOF (sticky broadcast) ============

    /**
     * `android.hardware.usb.action.USB_STATE` — the **sticky** broadcast whose extras carry the
     * USB gadget's applied state, and the last piece of ADB evidence the layers above cannot
     * reach. A (Settings) / A2 (properties) / A3 (exec'd getprop) / A5 (provider transport) all
     * rewrite *reads*; a detector that reads the runtime broadcast instead still sees
     * `connected=true, configured=true, adb=true` and concludes ADB is on.
     *
     * Measured 2026-10-14 (Duck Detector 2026.10.11 build 1018, on 22041211AC):
     * ```
     *   USB debugging setting : Disabled      <- layers A/A2 (ours)
     *   init.svc.adbd         : stopped       <- layer A2  (ours)
     *   sys.usb.state         : mtp           <- layer A2  (ours)
     *   USB runtime           : adb (connected=true, configured=true)   <- truth, uncovered
     *   => verdict [WARNING] ADB enabled
     * ```
     * That single uncovered line is what keeps the card out of "clear"; everything else already
     * agrees. The upstream detector grades this channel warning-only by design —
     * `UsbDeviceManager` builds the flags from `getAppliedFunctions()`, so "adb=true is not
     * process proof" (`eltavine/Duck-Detector-Refactoring`, `feature/adbruntime/EVIDENCE.md`) —
     * but a warning card is still a finding, and it is the one finding the module *can* fix.
     *
     * What is rewritten, and why not "everything off": the gadget really is configured, as MTP
     * according to our own `sys.usb.state=mtp` / `sys.usb.config=mtp`. So the coherent story is
     * *a phone plugged in for file transfer*: keep `connected`, keep `configured`, keep the `mtp`
     * function flag, clear `adb`. Claiming `connected=false` would contradict the `sys.usb.state
     * =mtp` we already report (that value means the gadget is applied).
     *
     * Returning a rebuilt Intent is safe with respect to intent-filter matching: `filterEquals()
     * compares action / data / type / identity / flags, never extras`, so a caller's filter
     * still matches exactly what it matched before the rewrite.
     *
     * Three deliberate limits:
     *  - **Only the descriptor, never the device.** This rewrites the Intent handed to a caller
     *    in *this* process; it never calls `UsbManager`, never touches system_server, and never
     *    stops adbd — the real ADB session (and this module's own host) keeps working.
     *  - **Wireless debugging is left alone.** The coherent USB story and the wireless one are not
     *    the same broadcast: a detector can tell them apart from the extras themselves
     *    (`connected` / `configured`), and the way it distinguishes a wireless-only session is
     *    exactly `adb=true` with no configured gadget. Rewriting that case would make an active
     *    wireless ADB session read as absent — a contradiction of its own — so the rewrite
     *    requires `configured=true`, plus a sysfs veto when the adb function is demonstrably
     *    unlinked.
     *  - **A miss is possible, and accepted.** Software can keep answering `adb=true` outside what
     *    this hook sees (its own `UsbManager` transport list, a cached registration, a native
     *    read). A hidden ADB that is still visible through a channel we do not cover is the
     *    intended trade: this layer removes the *coherent public story*, it does not police every
     *    consumer — see the README's ADB Runtime note.
     *
     * Installed inside the same spoof branch as the property layers (so `SPARE_PACKAGES` are
     * excluded too) and gated live by [spoofOn] plus the master pause, matching every other hook
     * here. `ContextWrapper.registerReceiver` exists on every supported API; the
     * `registerReceiverForAllUsers` variant only appears from API 29, and both `findClass` and
     * [hookAll] skip whatever is absent, so `minSdk` 27 is fine.
     */
    private fun installUsbStateSpoof(cl: ClassLoader) {
        // ContextWrapper covers Activity / Service / Application and every custom wrapper;
        // ContextImpl covers code holding the real base (it does not extend ContextWrapper).
        // Both are hooked because either can be the dispatched receiver; each call resolves to
        // exactly one of them, so the result is rewritten once.
        var total = 0
        for (cls in arrayOf("android.content.ContextWrapper", "android.content.ContextImpl")) {
            val c = findClass(cls, cl) ?: continue
            var n = 0
            for (m in arrayOf(
                "registerReceiver",                    // both the 2-arg and the 3-arg overload
                "registerReceiverForAllUsers",         // ContextImpl-only @hide variant
            )) {
                n += hookAll(c, m, usbStateHooker)
            }
            if (n > 0) logI("USB runtime state spoof: $cls hooks=$n")
            total += n
        }
        if (total == 0) {
            logE("USB runtime state spoof: no registerReceiver overload found")
        }
    }

    private val usbStateHooker = WbInterface.Hooker { chain ->
        val result = try { chain.proceed() } catch (t: Throwable) {
            logE("usbStateHooker proceed failed", t); throw t
        }
        try {
            rewriteUsbStateIntent(result)
        } catch (t: Throwable) {
            logE("usbStateHooker rewrite failed", t)
        }
        result
    }

    /**
     * Returns the replacement Intent for a sticky `USB_STATE` result, or the original unchanged
     * whenever there is nothing to hide — which is the common case in this process.
     */
    private fun rewriteUsbStateIntent(result: Any?): Any? {
        val src = result as? Intent ?: return result
        if (pausedOn() || !spoofOn()) return result
        if (src.action != USB_STATE_ACTION) return result

        val extras = try { src.extras } catch (_: Throwable) { null } ?: return result
        // 1) Nothing to hide: the broadcast does not advertise the adb function.
        if (!extras.getBoolean(USB_EXTRA_ADB, false)) return result
        // 2) Wireless-only session (or nothing applied yet): the gadget is not configured, so
        //    the adb flag here is not wired evidence — leave it, or ADB-over-Wi-Fi breaks.
        if (!extras.getBoolean(USB_EXTRA_CONFIGURED, false)) return result
        // 3) sysfs veto: if the adb function is demonstrably not linked, the broadcast is lagging
        //    and there is nothing to rewrite. Unreadable -> no veto, the checks above stand.
        if (usbAdbFunctionLinked() == false) return result

        // Build a fresh Intent: never mutate the object the framework handed us, and never touch
        // connected/configured/mtp — see the A7 note on why those must stay consistent with our
        // own sys.usb.state=mtp.
        val safe = Bundle(extras)
        safe.putBoolean(USB_EXTRA_ADB, false)
        safe.putBoolean(USB_EXTRA_MTP, true)
        val replacement = Intent(USB_STATE_ACTION).replaceExtras(safe)
        if (verboseOn()) {
            logI("USB runtime state spoof: USB_STATE adb=true -> false, kept " +
                "connected=${safe.getBoolean(USB_EXTRA_CONNECTED, false)} " +
                "configured=true ${USB_EXTRA_MTP}=true")
        }
        return replacement
    }

    /**
     * `null` when the adb function-filesystem flag cannot be read (absent file, denied access) —
     * a veto needs positive evidence, so `null` must never suppress the rewrite.
     */
    private fun usbAdbFunctionLinked(): Boolean? = try {
        val v = java.io.File(SYSFS_FFS_ADB).readText().trim()
        if (v.isEmpty()) null else v != "0"
    } catch (_: Throwable) {
        null
    }

    // ========================= B) NOTIFICATION SUPPRESSOR =========================

    /** Hook the public wrapper UsbDeviceManager uses: NotificationManager.notify* . */
    private fun installNotificationSuppressor(cl: ClassLoader) {
        val nm = findClass("android.app.NotificationManager", cl) ?: return
        for (m in arrayOf("notify", "notifyAsUser")) hookAll(nm, m, notifHooker)
    }

    /** Deeper chokepoint inside system_server so we catch it whatever path posts it. */
    private fun installSystemServerSuppressor(cl: ClassLoader) {
        val nms = findClass("com.android.server.notification.NotificationManagerService", cl) ?: return
        hookAll(nms, "enqueueNotificationInternal", notifHooker)
    }

    private val notifHooker = WbInterface.Hooker { chain ->
        val n = chain.args.firstOrNull { it is Notification } as? Notification
        if (n != null && !pausedOn() && hideNotifOn() && isAdbNotification(n)) {
            // Swallow the post: the original never runs, nothing is shown. These are void
            // methods, so the null we return here is discarded.
            null
        } else {
            chain.proceed()
        }
    }

    private fun isAdbNotification(n: Notification): Boolean {
        // 1) By channel (fast path).
        try {
            if (n.channelId in ADB_CHANNELS) return true
        } catch (_: Throwable) {}
        // 2) By (localized) title — resolved live from framework resources, so it
        //    matches whatever wording the ROM uses ("Débogage USB activé", etc.).
        try {
            val title = n.extras?.getCharSequence(Notification.EXTRA_TITLE)?.toString()
            if (title != null && title in adbTitles) return true
        } catch (_: Throwable) {}
        return false
    }

    /** The ROM's own strings for the USB/Wi-Fi debugging notification titles. */
    private val adbTitles: Set<String> by lazy {
        val res = Resources.getSystem()
        arrayOf("adb_active_notification_title", "adb_wifi_active_notification_title")
            .mapNotNull { name ->
                val id = res.getIdentifier(name, "string", "android")
                if (id != 0) runCatching { res.getString(id) }.getOrNull() else null
            }
            .toSet()
    }
}
