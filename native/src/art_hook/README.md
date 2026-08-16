# Vector WxShadow ART backend

This directory contains the ARM64 WxShadow router subset imported from
`D:\crack\rsfrida\rustFrida\quickjs-hook`.

Vector uses the ART router, native replacement thunk, and ARM64 relocator to
install hooks through the kernel-backed WxShadow write path. The adapter in
`vector_art_hook.cpp` supplies the JNI reflection/layout bridge and keeps the
existing Vector callback API intact.

The rustFrida Layer 1 shared JNI/Nterp/interpreter/resolution routers and Layer
2 ART `DoCall` controller are adapted to Vector's callback registry. The method
route split follows `java_hook_api/install.rs` and
`java_hook_api/install_support.rs`: registered native methods hook `data_`,
unregistered/shared native methods use the Layer 3 ArtMethod table route, and
ordinary non-native methods keep exact known shared routers such as nterp,
interpreter bridge, JNI, and resolution. For normal module Java hooks,
unclassified non-native libart entries use the internal interpreter-bridge
downgrade described below; unsupported native and managed high-frequency
paths still return to their existing fallback. ClassLinker trampoline discovery follows
`art_method/bridge.rs` when ART quick symbols are hidden. GC replacement
synchronization, `GetOatQuickMethodHeader`, and the implicit-suspend/NULL-OAT
signal guards are enabled on a best-effort basis.

ArtMethod layout discovery first uses the known native
`Process.getElapsedCpuTime` anchor, the source `0x0119` access-flag scan, and
the `libandroid_runtime.so` `data_` range, then obtains `entry_point_` from that
same method after ClassLinker and symbol lookup. If the Frida-style scan cannot
produce both fields, Vector uses the source-defined
`jni_core.rs::probe_entry_point_offset_legacy` candidates (24/32, dex-index
disambiguation, then `Object.hashCode`) and its API-level size rule; it does not
invent a new executable-field heuristic. The reflection `getModifiers()` scan
is retained only as a Vector compatibility fallback for vendor flag layouts.
ClassLinker-derived addresses are accepted only after executable/libart
validation; rustFrida has a last-resort non-zero runtime candidate fallback,
but Vector deliberately returns to LSPlant when no candidate passes that
validation because its API has no safe way to repair an unknown pointer later.
Vector's callback bridge exposes the regular JNI ABI only; rustFrida's separate
critical-native primitive ABI is therefore intentionally left on the LSPlant
fallback rather than being approximated.

rustFrida can refresh downgraded entries on detached timers. Vector keeps
`MethodState` for the HookItem lifetime and folds replacement synchronization
into its GC/ClassLinker callbacks. For normal module Java hooks, an
unclassified non-native `libart` shared entry follows rustFrida's
`allow_internal_entry_downgrade=true` path: it is temporarily changed to
`quick_to_interpreter_bridge`, dispatched through the existing shared router,
and restored by `MethodState` cleanup when the hook is removed or activation
fails. Compiled quick code and native/shared-native entries are not changed by
this adapter.

Deoptimization is also intentionally an explicit operation. The source
controller does not run `DeoptimizeBootImage`, `DeoptimizeEverything`,
`InvalidateAllMethods`, or `forced_interpret_only` while installing hooks;
Vector follows that lifecycle and exposes only `HookBridge.deoptimizeMethod`.
That API enables instrumentation and deoptimizes the hooked original method,
then falls back to LSPlant if the vendor ART symbols are unavailable. The
rustFrida JS/global deopt APIs, raw-clone executor, and automatic JIT lifecycle
controls have no equivalent Vector API and are not approximated here.

The rustFrida OAT inline patcher, Recomp backend, DecodeGcMasksOnly pattern
patch, and QuickJS runtime remain intentionally excluded. The Vector adapter
does not restore the deleted dex2oat service. LSPlant remains a fallback when
the ART layout or WxShadow backend cannot be initialized.

Stealth selection follows rustFrida's `StealthMode` values (`0` = Normal,
`1` = WxShadow). Vector defaults to `auto`: it probes the kernel patch and
release operations before enabling WxShadow, which avoids forcing a `-8`
failure when a separately packaged KPM has not loaded yet. Set
`VECTOR_ART_HOOK_STEALTH=normal`, `VECTOR_ART_HOOK_STEALTH=wxshadow`, or
`VECTOR_ART_HOOK_STEALTH=auto` to override the default. The equivalent Android
properties are `debug.vector.art.stealth` and `persist.vector.art.stealth`.
