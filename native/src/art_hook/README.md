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
Vector's callback bridge dispatches both JNI ABIs, selected per method at
install time exactly like the source. `MethodState::native_entry_critical`
mirrors `callback/registry.rs:112` and is set from
`access_flags & kAccCriticalNative` **and**
`install.rs::is_critical_native_signature_supported` (static, primitive
parameters and return); a flag that fails the signature test is downgraded as
ART noise, matching `install.rs:314-319`. Dispatch reads parameters from `x0`
for the critical ABI and from `x2` for the regular JNI ABI
(`callback/hook.rs::extract_critical_native_arg` versus `extract_jni_arg`), and
rejects object parameters on the critical path.

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

## Dispatch hot path resolves no classes

`VectorArtHookCallback` never calls `JNIEnv::FindClass`. `FindClass` needs the
current method's class loader, so ART runs `Thread::GetCurrentMethod()` →
`StackVisitor::WalkStack` down the thread stack. While a hook callback is
running that stack still holds the router's synthetic frames, and a quick frame
whose method has a null `entry_point_from_quick_compiled_code_` trips
`CHECK(existing_entry_point != nullptr)` in `runtime/stack.cc` (or
`art_method.cc`) and aborts the process — observed on device as
`F stack.cc:834] Check failed: existing_entry_point != nullptr` with the
backtrace `JNI::FindClass → Thread::GetCurrentMethod → StackVisitor::WalkStack`
and the callback frame directly below it. The same abort also reached
dispatch through a different expression of the same rule (a null entry point
reached from `ArtMethod` accessors), so the invariant is about `FindClass`, not
about one particular call site.

rustFrida never hits this because its callback marshals arguments straight into
JS values (`callback/hook.rs::marshal_jni_arg_to_js`; object arguments stay raw
pointers in `{__jptr,__jclass}` wrappers) and therefore resolves no class in the
hot path. Vector keeps its Java callback ABI, so the equivalent invariant is
enforced instead: `java/lang/Object`, `java/lang/Number` and the eight box
classes are pinned as JNI global refs during `initializeArtBootstrap` /
`completeArtBootstrap`, `BoxCallbackValue`/`UnboxCallbackValue` only read that
cache, and a dispatch that finds the cache empty skips the call instead of
falling back to `FindClass`. The cache is only ever populated on the bootstrap
path, where the stack carries no synthetic frame.

## `$orig` on a native entry uses the saved registers

For `route=registered_native` methods the hook is installed on `data_` (the
registered JNI fnPtr) with `hook_replace`, and the clone keeps
`data_ = trampoline`, `entry_point_ = jni_trampoline` — the same shape as the
source's NativeEntry branch (`install.rs`). What is *not* guaranteed is the
calling convention of whoever reaches `data_`: it is reachable both from ART's
generic JNI trampoline (`x0 = JNIEnv*`, `x1` = `jclass`) and from a compiled JNI
stub recorded in `entry_point_from_quick_compiled_code_` (observed on device in
`boot-framework.oat`, while `data_` itself sits in `libandroid_runtime.so`),
which passes a different register layout. Re-entering the ArtMethod through
`CallStatic*MethodA` with parameters read as if the entry followed the regular
JNI ABI therefore hands the original native function garbage arguments — the
failure signature is a fault inside the original callee
(`SystemProperties::ReadCallback` → `__strncmp`) with the callback frame at the
top of the local stack.

The source avoids this by never re-entering: for a native entry, `$orig()`
without explicit arguments invokes the saved trampoline with the original
registers (`callback/original_call.rs:1136-1145`,
`hook_ffi::hook_invoke_trampoline`), which is correct for any entry ABI.
Vector mirrors that. The dispatch pushes the live `HookContext*` and the
callback's argument array onto a thread-local frame stack
(`PushDispatchFrame`/`PopDispatchFrame`), and `InvokeOriginal` compares the
hooker's arguments element-wise against the observed ones (`ArgsMatchObserved`);
when they are unchanged it calls
`hook_invoke_trampoline(dispatch_ctx, state.quick_trampoline)` and boxes the raw
return register directly (`ReturnValueFromRaw`). Arguments the hooker actually
modified still take the JNI re-entry path, matching the source's
`supplied_jargs` branch. On the critical ABI the registers *are* the parameters,
so any Java-side argument list is ignored and the trampoline is always used;
this is the Vector equivalent of the source rejecting `orig(...args)` for
critical native hooks.

## Dispatch frames must not own TLS destructors

`PushDispatchFrame`/`PopDispatchFrame`/`LookupDispatchFrame` keep their frame
stack as a fixed-size POD array with a depth counter, deliberately not a
`std::vector`. The library is mapped by Zygisk's own loader, so the linker has no
`soinfo` for it; a `thread_local` object with a destructor calls
`__cxa_thread_atexit_impl(..., &__dso_handle)` on first use, and the linker then
aborts the process with
`increment_dso_handle_reference_counter: Couldn't find soinfo by dso_handle=...`
(`__loader_add_thread_local_dtor`). That abort fired inside the first dispatch in
`system_server`, which restarted four times and left Vector inactive for the
whole boot — a whole-device failure mode, not a hook-local one. Trivial
`thread_local` scalars (for example the stealth override) are safe, since they
need no destruction.

## Fabricated frame metadata must not claim references

The router thunk runs on a 224-byte frame that ART does not know about, so
`OnGetOatQuickMethodHeader` hands `StackVisitor::WalkStack` a fabricated OAT
header plus a 16-byte codeinfo (`FAKE_PACKED_FRAME_SIZE = 224/16 = 14`). That
codeinfo originally also declared spill masks (core popcount 18, fp popcount 8,
inherited from rustFrida). Measured on device, that made the concurrent copying
GC mark slots of *our* frame as references:

```
verification.cc:114] GC tried to mark invalid reference 0xd503233f
  #12 art::ReferenceMapVisitor<art::RootCallbackVisitor, false>::VisitFrame
  #13 art::StackVisitor::WalkStack<(art::CountTransitions)1>
  #14 art::Thread::VisitRoots
  #15 art::gc::collector::ConcurrentCopying::...
```

`0xd503233f` is the first instruction of the thunk (`paciasp`), i.e. ART read a
*code* address out of a slot the mask had called a reference; other runs reported
heap-shaped values (`0xb400006fde9e04b0`) instead. The 224-byte frame is a
SaveRefsAndArgs *shape*, but the saved registers in it are not laid out where the
mask implies references live, so any declared spill bit is a lie to the GC.
Zeroing both masks (`FAKE_CORE_SPILL_MASK`/`FAKE_FP_SPILL_MASK` = 0) stopped the
crash entirely: same device, same workload, hunter 220s+ with zero
`invalid reference`, zero ANR and zero MIUI scout HANG, versus one crash every
1-2 minutes before, with the duckusb spoof still intercepting reads. The earlier
worry that a zero mask stops the walker from refilling caller registers (so the
next compiled frame treats stale scalars as roots) has not reproduced; treat
zeroing as the conservative default and re-check on long/mixed workloads.
