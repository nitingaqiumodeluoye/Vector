# DuckUSB (Vector port)

An example Vector module that makes apps read **USB debugging as OFF while it stays really ON** —
same for wireless debugging and Developer Options. It also spoofs the raw USB system properties
and can hide the persistent *"USB debugging enabled"* notification.

Ported from [DuckUSB](https://github.com/Bouteillepleine/DuckUSB) by Bouteillepleine — an
LSPosed / libxposed **API 101** module — to Vector's `WbModule` API. Upstream's README is worth
reading for the reasoning behind the guards; this file only covers the port.

## What the module does

Detection apps (banking, MDM, games, integrity checks) don't read any real adb state. They query
the settings provider:

```
Settings.Global.getInt(cr, "adb_enabled")                   // USB debugging
Settings.Global.getInt(cr, "adb_wifi_enabled")              // wireless debugging
Settings.Global.getInt(cr, "development_settings_enabled")  // Developer Options
```

Two independent layers can lie about that:

| Layer | Where it runs | How it works |
|---|---|---|
| **Per-app** (default) | Each scoped app | Hooks the static `Settings.Global` / `Settings.Secure` getters, plus `SystemProperties.native_get*` and, natively, libc's `__system_property_get` / `__system_property_read` / `__system_property_read_callback` — and, for the one path those cannot reach (an exec'd `getprop`), wraps `ProcessBuilder.start()` so the child's stdout carries the same overrides |
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
| **Per-app Settings spoof** | Client-side getters, installed in each scoped app. |
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

`sys.usb.ffs.ready` is deliberately **not** spoofed (it is machinery the USB stack acts on, not
telemetry a detector reads).

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

## Provenance and licence

Ported from [Bouteillepleine/DuckUSB](https://github.com/Bouteillepleine/DuckUSB). Upstream's
README, its comments and the values in `Config.kt` are its authors' work; the guards and the
`sys.usb.ffs.ready` / `persist.sys.usb.config` exclusions encode their measured failures and are
kept verbatim.
