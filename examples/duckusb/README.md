# DuckUSB (Vector port)

An example Vector module that makes apps read **USB debugging as OFF while it stays really ON** —
same for wireless debugging and Developer Options. It also spoofs the raw USB system properties
and can hide the persistent *"USB debugging enabled"* notification.

Ported from [DuckUSB](https://github.com/Bouteillepleine/DuckUSB) by Bouteillepleine — an
LSPosed / libxposed **API 101** module — to Vector's `WbModule` API. Upstream's README is worth
reading for the reasoning behind the guards; this file only covers the port.

## What the module does

Detection apps (banking, MDM, games, integrity checks) don't read any real adb state. They read
the settings provider — but not always through the static getters:

```
// 1) the static getters                       (layer A)
Settings.Global.getInt(cr, "adb_enabled")                   // USB debugging
Settings.Global.getInt(cr, "adb_wifi_enabled")              // wireless debugging
Settings.Global.getInt(cr, "development_settings_enabled")  // Developer Options

// 2) the provider transport, named directly    (layer A5)
cr.query(Uri.parse("content://settings/global"), null, null, null, null, null)
cr.call(Uri.parse("content://settings/global"), "GET_global", "adb_enabled", Bundle())
```

Those two forms are not interchangeable from a hooking standpoint: layer A never runs for form 2.
A detector that names the provider therefore sees the truth while every getter is already lying —
and it can cross-check the three readings against each other to prove it.

Within form 1 there is a second, quieter trap: `getInt(cr, name)` (2 args) does **not** delegate to
`getInt(cr, name, def)` (3 args), so hooking only the 3-arg overload installs a hook that never
fires. The symptom is a clean install log, zero hook hits, and `adb_enabled` still reading 1 —
which is exactly what shipped in 1.0.9. `SETTINGS_TARGETS` now lists both overloads.

A third form needs neither of the above, and it is the one that ends up on the ADB Runtime card:

```
// 3) the gadget's runtime state, as a sticky broadcast   (layer A7)
registerReceiver(null, IntentFilter("android.hardware.usb.action.USB_STATE"))
  .extras.getBoolean("adb")        // true while the adb function is applied
```

The extras there are built from `UsbDeviceManager.getAppliedFunctions()`. Nothing in this process
hooks that path — it is not a Setting and not a property — so it kept answering `adb=true` while
every getter above was already lying, and a detector that reads it sees the truth. Measured
2026-10-14: `USB debugging setting: Disabled` / `init.svc.adbd: stopped` / `sys.usb.state: mtp`
next to `USB runtime: adb (connected=true, configured=true)`. Layer A7 closes it.

Layers that can lie:

| Layer | Where it runs | How it works |
|---|---|---|
| **Per-app: settings** (default) | Each scoped app | Hooks the static `Settings.Global` / `Settings.Secure` getters (both `getInt` overloads) **and** the provider transport — `ContentResolver.query` (every overload; rebuilds a settings cursor with the `value` column substituted) and `ContentResolver.call` (rewrites the returned `Bundle["value"]` and marks it uncacheable). Covers both forms above with no server-side component. |
| **Per-app: properties** | Each scoped app | `SystemProperties.native_get*`, and natively libc's `__system_property_get` / `__system_property_read` / `__system_property_read_callback` — plus, for the one path no in-process hook can reach (an exec'd `getprop`), wrapping `ProcessBuilder.start()` so the child's stdout carries the same overrides |
| **Per-app: USB runtime** | Each scoped app | The sticky `android.hardware.usb.action.USB_STATE` broadcast, whose extras carry the gadget's applied state (`connected` / `configured` / `adb`). It is neither a Setting nor a property, so the two layers above cannot reach it — a detector reading it still saw `adb=true`. `registerReceiver` is hooked (`ContextWrapper` **and** `ContextImpl`, every overload) and the returned Intent is rebuilt with `adb=false` while `connected` / `configured` / `mtp` stay, so the story matches our own `sys.usb.state=mtp`. Gated on `configured=true` — a wireless-only session is exactly `adb=true` with no configured gadget, and rewriting that would make an active wireless session read as absent — plus a sysfs veto when `/sys/android_usb/android0/ffs/adb` reads `0` (the MTK layout; the same file is `/sys/class/android_usb/android0/ffs/adb` on other SoCs, and an unreadable path simply skips the veto). Skipped for `SPARE_PACKAGES` |
| **Per-app: device state** | Each scoped app | The same detector cross-checks things that are not USB at all: `KeyguardManager.isDeviceSecure` / `isKeyguardSecure` (a device with *no* lock-screen password is the risk), `TelephonyManager.getSimState` (no SIM is the risk) and `SubscriptionManager.getActiveSubscriptionInfoCount`. Spoofing only the ADB paths leaves these reporting the real device. |
| **Framework mode** (opt-in, experimental) | `system_server` | Hooks `ContentProvider.attachInfo` → waits for the settings provider (matched by **authority**) → hooks only that provider's `call()`, rewriting results per `Binder.getCallingUid()` |

Callers at **uid < 10000** (root / system / shell) always see the truth, so `adb` and the Settings
toggle keep working. The module also never lies to *itself* — see [Guards](#guards).

Upstream's measured trade-off, kept here because it is the reason framework mode exists: in
framework mode a scoped app carries no hook residue of its own, because nothing is installed in
its process. In per-app mode the copy-on-write from inline hooks shows up in that app's
`/proc/self/smaps`, which a detector reading its own smaps can see.

## Scope

**This port does not fix its scope**, matching upstream: `module.prop` sets `staticScope=false`, and
`scope.list` names three packages that are **recommendations, not limits**:

```
system                 # the framework's convention for system_server
android                # carried over from upstream; see the warning below
com.android.systemui   # for the notification suppressor
```

The daemon ignores `scope.list` entirely while `staticScope` is false, so the Vector manager lists
every installed app and you tick whatever you want. The three names above are highlighted as
recommended (`ScopeOrigin.Requested`), nothing more. **Nothing is scoped until you tick it** — the
module can be enabled and injected into its own process while hooking no app at all.

So the setup is:

1. Vector manager → DuckUSB → **Scope** → tick the apps you want to lie to. For framework mode or
the notification suppressor, tick the entry named **`system`**.
2. Reboot (or re-open the app). Force-stop the target app if it was already running.

**Do not** rely on the entry named `android` — it is only the framework package of ordinary app
processes and does **not** inject into `system_server`. It is in `scope.list` because upstream
carries it, not because it does anything here; tick `system` instead.

### If you do want the scope fixed

Set `staticScope=true` in `module.prop`. Then the daemon prunes every scope row the module does not
claim (`ModuleDatabase.pruneScopeToClaimed`), and **an app added through the manager or
`cli scope add` is deleted again on the next cache rebuild**; `scope add` is refused outright while
the manager's picker narrows to the declared set.

That is the safer configuration — the property spoof writes the USB stack's own control surface
(`sys.usb.state`, `init.svc.adbd`), and pointing it at a core OS process takes the USB gadget down
outright, no MTP and no adb — but it costs a rebuild per app you want to cover, which is why the
module ships unlocked. What keeps a careless tick from reaching a core process in the unlocked
configuration is the three guards in `DuckUsbModule.onPackageReady` (uid, process name, package
name) plus `SPARE_PACKAGES`, and `should_skip_hooks()` in the native layer.

One trap either way: **`scope.list` has no comment syntax.** It is read as "every non-empty line is
a package", with no `#` handling, so a comment line becomes a claimed package. Keep that file to
bare package names (Java `.properties` rules apply to `module.prop` instead, which *does* support
`#`).

## Toggles

| Toggle | What it does |
|---|---|
| **Master pause** | Stops every hook body. Does *not* uninstall hooks; Vector's own module switch is the real off. |
| **Spoof USB debugging** | The `adb_enabled` / `adb_wifi_enabled` / Developer Options lie. |
| **Framework mode** | Server-side spoof. Needs the `system` scope + reboot. Experimental. |
| **Per-app Settings spoof** | Client-side getters **and** the provider transport (`query`/`call`), installed in each scoped app. |
| **Hide notification** | Suppressor in `system_server` / System UI. |
| **Verbose logging** | One log line per injection. Off by default. |

Hook *installation* follows the toggles: enabling one feature places that feature's hooks and
nothing else.

## Spoofed values

| Setting | Forced |
|---|---|
| `adb_enabled` | `0` |
| `adb_wifi_enabled` | `0` |
| `development_settings_enabled` | `0` |

| Property | Forced |
|---|---|
| `sys.usb.config` | `mtp` |
| `sys.usb.state` | `mtp` |
| `init.svc.adbd` | `stopped` |
| `persist.sys.usb.config` | `mtp` |

Sticky `USB_STATE` broadcast extras (layer A7):

| Extra | Forced | Why |
|---|---|---|
| `adb` | `false` | The one uncovered ADB evidence — see [the reason above](#what-the-module-does) |
| `mtp` | `true` | Kept consistent with `sys.usb.state=mtp` |
| `connected` | *untouched* | The phone really is on a cable; forcing `false` would contradict `sys.usb.state=mtp` |
| `configured` | *untouched* (read as a gate) | `true` is required before `adb` is rewritten; a wireless-only session is `adb=true` with `configured=false`, and that case is left alone |
| any other function flag | *untouched* | We only remove the adb claim |

`sys.usb.ffs.ready` is deliberately **not** spoofed (it is machinery the USB stack acts on, not
telemetry a detector reads). **Four of these five values are mirrored in PropMask and must stay
byte-identical** — see [Division of labour with PropMask](#division-of-labour-with-propmask);
`persist.security.adbinput` / `persist.security.adbinstall` are PropMask-only for the reason given
there, and are deliberately absent from `Config.PROP_OVERRIDES`.

Device-state reads, spoofed by the per-app device-state layer:

| API | Forced | Why |
|---|---|---|
| `KeyguardManager.isDeviceSecure()` | `true` | The check is inverted: **no** lock-screen password is the risk |
| `KeyguardManager.isKeyguardSecure()` | `true` | Same check, second half — the detector requires both |
| `TelephonyManager.getSimState()` | `5` | `SIM_STATE_READY` — a device with no SIM is the risk |
| `SubscriptionManager.getActiveSubscriptionInfoCount()` | `1` | Kept consistent with `SIM_STATE_READY`; one ready subscription |

`persist.sys.usb.config` **is** spoofed — and that is exactly why the getprop layer exists. An
exec'd `getprop` reads the real property store in its own process, beyond any in-process hook,
so a detector cross-checking sources sees reflection `mtp` vs getprop `adb` and flags a
*"source mismatch / Diverged"* (measured with Duck Detector 526). The fix is not to stop lying
but to cover that path: the module hooks `ProcessBuilder.start()` — the spawn happens in the
app's own process — and rewrites the getprop child's stdout with the same overrides, handling
both the full dump (`[key]: [value]` lines) and the single-key (`getprop <name>`) forms. All
three sources then agree. See the comments in `Config.kt`.

## Guards

Three guards against lying to the wrong process, because each alone has a proven gap:

- **uid** — anything below 10000 is OS;
- **process name** — `android`, `system`, `com.android.systemui`, `com.android.settings`,
  `com.android.shell`, `com.android.phone` (OEM plugins can ride `com.android.systemui` at an app
  uid, which a package check misses);
- **package name** — `SPARE_PACKAGES` in `Config.kt`: the OS's own file-transfer plumbing
  (`com.android.mtp`, `com.android.externalstorage`, …). These run at *app* uids, so the uid rule
  does not reach them, and telling the MTP service `sys.usb.state=mtp` is how USB ends up stuck on
  charge-only.

The module never lies to itself either. Vector loads a module into its own app process whether or
not it is scoped, so both halves skip `com.example.duckusb`:

- **native**: `should_skip_hooks()` in [`native_hooks.cpp`](src/main/cpp/native_hooks.cpp) refuses
  it (and its `:sub` processes) before installing anything — this is the one that matters, because
  the hooks go in when the library loads, before any Kotlin gate runs;
- **framework mode**: the `SettingsProvider.call` hook spares our own app id, since a client-side
  guard cannot stop a lie told inside `system_server`.

Upstream paid for this lesson: the first property read landing in a trampoline took `SIGILL` and
killed their UI seconds after launch (their issue #2), in a process that never needed the hook.

## Layout

```
examples/duckusb/
├── src/main/AndroidManifest.xml              # no INTERNET: the update checker is not ported
├── src/main/aidl/…/IDuckService.aidl         # system_server ↔ UI channel
├── src/main/cpp/
│   ├── CMakeLists.txt                        # → libvector_duckusb.so
│   ├── native_hooks.cpp                      # the libc property hooks
│   ├── vector_native.h                       # Vector's NativeAPIEntries ABI
│   └── Logger.h
├── src/main/java/com/example/duckusb/
│   ├── DuckUsbModule.kt                      # the hook logic (entry class)
│   ├── DuckUsbApp.kt                         # holds the framework service for the UI
│   ├── MainActivity.kt                       # simplified status/toggles/readings UI
│   ├── Config.kt, NativeProps.kt
│   └── service/{Bridge,DuckService,DuckServiceClient}.kt
└── src/main/resources/META-INF/xposed/
    ├── module.prop                           # targetApiVersion=102, staticScope=false
    ├── java_init.list, native_init.list, scope.list
```

The `.WbService` provider in the merged manifest comes from
[`:services:libxposed-service`](../../services/libxposed-service), which wraps the libxposed
service submodule. It is what makes the daemon's `ModuleAppService.sendBinder` find a receiver —
without it the daemon retries three times and throttles for a minute, and the UI's status card
could never say "active".

## What changed in the port

The hook bodies are upstream's; only the framework surface and the native loading differ.

| # | Upstream (libxposed API 101) | Here | Why |
|---|---|---|---|
| 1 | `io.github.libxposed.api.XposedModule` / `XposedInterface.Hooker` / `XposedModuleInterface.*` | `de.wwb.android.wbosed.api.WbModule` / `WbInterface.Hooker` / `WbModuleInterface.*` | Vector's API is the same design under its own package; `hook(...).intercept(...)`, `Chain.proceed()`, `Chain.args`, `getRemotePreferences(...)` and `log(...)` are identical |
| 2 | `io.github.libxposed.service.XposedService{Helper}` | `de.wwb.android.wbosed.service.WbService{Helper}` | Same library, Vector's package |
| 3 | `implementation("io.github.libxposed:service:101.0.0")` | `implementation(project(":services:libxposed-service"))` | In-tree module so the `.WbService` provider merges from source, matching this repo's build |
| 4 | `compileOnly("io.github.libxposed:api:101.0.1")`, `targetApiVersion=101` | `compileOnly(project(":xposed"))`, `targetApiVersion=102` | Vector implements API 102 |
| 5 | `System.loadLibrary("duckusb")` in `NativeProps` | removed; `setProps` is bound by `RegisterNatives` from `native_init` | **Vector never uses the Android linker for module ELFs.** The daemon reads `lib/arm64-v8a/libvector_duckusb.so` into sealed SharedMemory and Vector's `ElfLoader::LoadLibraryFd()` maps it anonymously, so no JNI symbol is discoverable by name. `VectorModuleClassLoader.findLibrary` deliberately refuses `apk!/lib/<abi>/<name>.so` entries so no linker mapping of the APK can appear in the target process |
| 6 | `entries->hook_func` / `hook_func` spelling | `entries->hookFunc` / `hookFunc` | Same struct, CamelCase; the field order and types are unchanged, so the ABI is identical |
| 7 | `JNI_OnLoad`-style env access is unavailable anyway; upstream got the VM from the hook DSL | `GetJavaVm()` walks libart's `.dynsym` for `JNI_GetCreatedJavaVMs` | Vector's `native_init` signature carries no `JNIEnv`. libart is an APEX module, so `dlopen` / `dlsym(RTLD_DEFAULT)` cannot reach it — the ELF is walked by hand, mirroring rawVector's `get_or_init_vm` and rustFrida's `resolve.rs` |
| 8 | ndkBuild (`Android.mk` / `Application.mk`), 4 ABIs | CMake, arm64-v8a only | The root build injects `ndkVersion`, the abiFilter and `-Wl,-z,max-page-size=16384` through CMake; an `Android.mk` would opt out of all three. Vector's soLoader currently requires arm64-v8a |
| 9 | `Updater.kt` + `INTERNET` permission | removed | An example should not phone home; the update checker has nothing to check against |
| 10 | `com.strawing.duckusb` / `libduckusb.so` | `com.example.duckusb` / `libvector_duckusb.so` | Example naming, matching `native-hook-demo` and `lsp-hello` |
| 11 | 939-line Material UI (dynamic colour, theme switch, diagnostics list, scroll dance) | ~330-line plain-view UI | Kept: status + scope, every toggle, the real Settings readings, framework-mode diagnostics. Dropped: presentation. No appcompat/material dependency |

### `native_init` timing is different, and it matters not at all here

Upstream's `native_init` runs synchronously inside the `dlopen` of the module's library, on the
module's own `System.loadLibrary` stack. Under Vector it is **deferred**: the ELF is linked
first, and the entrypoint is later invoked from a module-class context (a native method
registered on a carrier class that belongs to the module's own ClassLoader).

That exists for `FindClass`: ART resolves it against the ClassLoader of the *current method's
declaring class*, so a call made from the framework's own frame would resolve the framework and a
module could never find its own Java classes. The deferral restores the context upstream gets for
free.

For this module it changes nothing:

- the libc hooks are installed by `native_init` and there is no earlier caller — module Java code
  cannot run before the module is loaded — so no property read can slip through unhooked;
- `should_skip_hooks()` reads `/proc/self/cmdline`, valid at any time;
- `FindClass` *does* work in `native_init` here, which is what makes the `RegisterNatives`
  self-binding of `NativeProps.setProps` possible;
- `native_init` runs strictly before the entry class is instantiated, so `onModuleLoaded` and
  everything after it see the hooks in place.

One consequence worth knowing: the deferred list is drained once, and an already-loaded ELF is
not reloaded, so `native_init` runs exactly once per process — which is also why the repeated
`NativeProps.install()` calls from `onPackageReady` are safe. They only republish data.

## Build

```powershell
$ErrorActionPreference = 'Stop'
$env:ANDROID_HOME = 'D:\sdk'
$env:ANDROID_SDK_ROOT = 'D:\sdk'
cd E:\52\Vector
.\gradlew.bat :examples:duckusb:assembleRelease
```

Output: `examples/duckusb/build/outputs/apk/release/duckusb-release.apk`.

The APK is **release (non-debuggable) with the debug signing config**. That is not cosmetic: a
debuggable APK is permanently run-from-apk, the system refuses to dex2oat it, and every process
fork re-verifies and JITs the module dex — measured at 5~10 s of extra host startup for a dex this
size. See the repository's `AGENTS.md`.

## Debugging

```
adb logcat -s DuckUSB VectorDuckUSB-native
```

Expected on load, per process:

```
native_init for 'com.example.duckusb!/libvector_duckusb.so' deferred to module-class context
Firing deferred native_init entrypoints for com.example.duckusb
FirePendingNativeInits: pending_count=1 carrier_class=class org.matrix.vector.nativeload.NativeInitFire
native_init for '…/libvector_duckusb.so' registered library-load callback 0x…
DuckUSB-native: bind_native_methods: NativeProps.setProps registered
DuckUSB-native: native_init: hook rc get=0 read_callback=0 read=0
DuckUSB: framework mode armed — hooked ContentProvider.attachInfo (awaiting SettingsProvider)   [system_server, framework mode on]
DuckUSB: framework settings spoof installed: SettingsProvider.call hooks=N
```

In a core process or in the module's own app the libc line reads
`native_init: own process, skipping libc hooks` / `core OS process, skipping libc hooks` — that is
the intended path, not a failure.

| Symptom | Look at |
|---|---|
| No `native_init` line at all | `META-INF/xposed/native_init.list` name vs `lib/arm64-v8a/libvector_duckusb.so` inside the APK |
| `completed without library-load callback` | Only means `native_init` returned null; this module returns a real callback |
| UI shows "not connected" | The `.WbService` provider did not merge, or the daemon was throttled — grep for `No service provider for com.example.duckusb` / `failed to take its binder` |
| `No service provider` three times then a 60 s throttle | The daemon's `ModuleAppService` could not find `<applicationId>.WbService`. Check the merged manifest |
| Scoped app still reads `adb_enabled=1` | Was the app force-stopped after scoping? Are you looking at the module's *own* readings card, which is deliberately truthful? |
| Nothing happens in the detector app | `staticScope` is false here, so the app is simply not ticked yet — see [Scope](#scope) |
| `cli scope ls` looks right, but `scope add` reports nonsense claims (`#`, comment words) | `scope.list` has comments in it. `readStaticScope` has no `#` syntax — every non-empty line is a claimed package. Keep that file to bare package names |
| USB drops to charge-only | Something in `SPARE_PACKAGES` got spoofed; the property layer is the cause |
| A detector still reports ADB on, via `USB_STATE` or the ADB runtime card | Is `verbose_log` on? Layer A7 logs `USB runtime state spoof: USB_STATE adb=true -> false` per hit. No line at all means no `USB_STATE` broadcast reached this process (`registerReceiver` never called, or the detector reads `UsbManager` state that never comes through the broadcast — see the [ADB Runtime note](#division-of-labour-with-propmask)). A line but still found means the extra names moved: check `UsbDeviceManager.makeUsbStateIntent` in the ROM |
| ADB-over-Wi-Fi stops being detected (or wireless debugging looks wrong) | A7 must *not* have rewritten a wireless-only broadcast. Confirm `configured=false` there — the gate leaves those alone by design |

## Division of labour with PropMask

This module is only one half of the ADB/USB lie. The other half is **PropMask**
(`E:/52/propmask`, a Zygisk module + companion KPM), which covers the two channels no in-process
hook can reach. The full coverage matrix, the value-parity table and the measured evidence live in
that repository's README §13 (**「与 Vector/duckusb 的联合边界(检测通道覆盖矩阵)」**) — read it
before interpreting any detector report. What matters here:

**PropMask owns what a Java/native hook structurally cannot see:**

| Channel | Why not here | Handled by |
|---|---|---|
| Raw prop_area read — `openat("/dev/__properties__/...")` + `mmap` + hand-parsed trie | No hook in this process is on that path | PropMask: bind-mounts a patched copy into the app's mount ns, plus `patch_inherited_property_maps()` for the zygote-inherited COW page |
| File metadata of the patched copy | — | PropMask: `copy_stat_meta` + `propmask-hide.kpm` drops the three mount tables |
| init.rc static scan | — | PropMask: equal-length rewrite (0 hits on this device) |
| Freshness / cross-channel drift | — | PropMask: rebuilds the copy image and compares it byte for byte before reuse |

**Parity is a hard requirement, not a courtesy.** The spoofed values must be byte-identical on
both sides or a detector flags *source divergence* instead of reading the lie. `sys.usb.config`,
`sys.usb.state` and `persist.sys.usb.config` are `mtp` in both, `init.svc.adbd` is `stopped` in
both. The single deliberate exception is `persist.security.adbinput` / `persist.security.adbinstall`
(empty string): **only PropMask covers them**, because this module's hooks cannot reach an exec'd
`getprop` child, and spoofing them here alone would manufacture exactly the mismatch the pair
exists to avoid. `sys.usb.ffs.ready` is deliberately spoofed by neither — it is machinery the USB
stack acts on, and claiming `0` kills the gadget.

**Scope is not shared.** PropMask decides by uid rule (`uid % 100000 ∈ [10000,19999]` + keep-out)
and covers *every* app process; this module uses Vector's module scope, which is **empty until you
tick something** (see [Scope](#scope)). Neither side manages the other. This module's own package is
in PropMask's keep-out list on purpose: PropMask rewrites the process property view, bind-mounts and
clears `runtime_flags`, so aiming it at the module that provides the hook backend would break the
injection stack itself.

**USB_STATE (layer A7).** `USB_STATE` — the sticky `android.hardware.usb.action.USB_STATE`
broadcast whose extras carry `connected` / `configured` / the `adb` function flag — was covered by
neither half, because it is not a property and not a file: PropMask's bind-mounts cannot reach it,
and none of the read hooks above are on that path. It is now handled here by layer **A7**
(`installUsbStateSpoof`), the only per-process place it can be rewritten. The hook has to sit on
`android.app.ContextImpl` (plus `ContextWrapper`), because `applicationContext` — the reference the
detector actually holds — resolves there and `ContextImpl` overrides `registerReceiver` rather than
delegating to `ContextWrapper`. The design
constraints come straight from the upstream detector's own evidence document —
`eltavine/Duck-Detector-Refactoring`, `feature/adbruntime/EVIDENCE.md`: the flags are built from
framework `UsbDeviceManager.getAppliedFunctions()`, so "adb=true is not process proof"; the channel
is graded **warning only**; `connected` and `configured` describe the *gadget*, not adbd liveness.
That is why A7 removes only the adb claim and leaves the gadget description standing, and why it
never touches system_server or adbd: the real ADB session keeps working.

The remaining uncovered ADB channel is the protocol-level one: a local `_adb._tcp` /
`_adb-tls-connect._tcp` endpoint that answers a real A_CNXN. Nothing in-process can fake that
(and it is the only path to a red card), but it also requires the detector to actually probe; the
2026-10-14 scan reported `ADB mDNS: Not observed`, so no Danger was reachable. PropMask's README
§13.5 lists it as the one channel neither module can hide.

`sys.usb.ffs.ready` is still spoofed by neither half: it is machinery the USB stack acts on, and
claiming `0` kills the gadget. A7 *reads* the sibling `/sys/android_usb/android0/ffs/adb`
(veto only; the path lives under `/sys/class/android_usb/` on non-MTK SoCs and a read failure is
treated as "no veto").

Finally, out of both modules' remit by design: **package-enumeration signals** (a detector listing
installed APKs and reading their meta-data, e.g. the `com.zhenxi.jnitrace` hit, or the `FolkPatch`
root-manager match in the same report). That is "what is installed on the device", not a view any
process can lie about in-process.

## Provenance and licence

Ported from [Bouteillepleine/DuckUSB](https://github.com/Bouteillepleine/DuckUSB). Upstream's
README, its comments and the values in `Config.kt` are its authors' work; the guards and the
`sys.usb.ffs.ready` / `persist.sys.usb.config` exclusions encode their measured failures and are
kept verbatim.
