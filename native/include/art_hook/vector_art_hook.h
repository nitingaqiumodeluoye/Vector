#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "art_hook/hook_engine.h"

namespace vector::native::art {

enum class ValueKind : unsigned char {
    Void,
    Boolean,
    Byte,
    Char,
    Short,
    Int,
    Long,
    Float,
    Double,
    Object,
};

using NativeHookCallback = HookCallback;

struct MethodState {
    uintptr_t original_art_method = 0;
    uintptr_t original_clone = 0;
    uintptr_t replacement = 0;
    uintptr_t original_entry = 0;
    uintptr_t router_target = 0;
    uintptr_t quick_trampoline = 0;
    uintptr_t native_entry_target = 0;
    size_t method_size = 0;
    size_t access_flags_offset = 4;
    size_t data_offset = 0;
    size_t entry_offset = 0;
    jobject declaring_class = nullptr;
    bool is_static = false;
    bool owns_router_hook = false;
    bool native_entry_hook = false;
    bool native_entry_installed = false;
    // rustFrida callback/registry.rs:112 native_entry_critical —— 原生入口使用
    // @CriticalNative ABI：x0+ 直接是 Java 基本类型参数，没有 JNIEnv*/jclass。
    // 安装期由 access_flags & kAccCriticalNative 结合签名可支持性判定（对齐
    // rustFrida install.rs:311-314 的 is_critical_native）。
    bool native_entry_critical = false;
    // rustFrida's normal Java hook path may temporarily downgrade an
    // unclassified libart shared entry to quick_to_interpreter_bridge.
    // Cleanup restores the original ArtMethod entry_point_.
    bool original_entry_mutated = false;
    NativeHookCallback callback = nullptr;
    void *callback_user_data = nullptr;
    ValueKind return_kind = ValueKind::Void;
    std::vector<ValueKind> parameter_kinds;

    ~MethodState();
};

/** Initialize the complete rustFrida-compatible ART controller. */
bool Initialize(JNIEnv *env);

/**
 * Initialize only the pre-attach ART layout, executable pool and interpreter router needed by
 * rawVector's ActivityThread.attach bootstrap hook.
 */
bool InitializeBootstrap(JNIEnv *env);

/** Complete the shared routers, DoCall, GC, OAT and signal layers after attach returns. */
bool CompleteInitialization(JNIEnv *env);
void SetSystemProcess(bool is_system);
void SetManagerProcess(bool is_manager);

/**
 * Master switch for the in-process `[perf]` timers (`[perf] art_*` / `java_hook` /
 * `do_dlopen_hook` / `load_lib` / `load_native_lib` …).
 *
 * Off unless explicitly requested, so a production build stays silent on logcat. Accepted
 * sources (any one of them, checked in this order):
 *   - env  `VECTOR_ART_HOOK_PERF`     = 1/true/yes/on
 *   - prop `debug.vector.art.perf`    = 1/true/yes/on
 *   - prop `persist.vector.art.perf`  = 1/true/yes/on
 *
 * Defined in vector_art_hook.cpp. It lives outside that file's anonymous namespace on purpose:
 * the perf log sites outside the ART module (core/native_api.cpp, jni/native_api_bridge.cpp,
 * plus soLoader's SOLOADER_PERF_LOGI) need the same switch. In particular DoDlopenHook runs on
 * *every* dlopen, so it must not emit an unconditional INFO line.
 */
bool ArtPerfEnabled();

/** Whether the ARM64 backend was initialized successfully. */
bool IsAvailable();

/** Whether the selected ART patch mode uses the WxShadow publish path. */
bool IsWxShadowStealthMode();

/** Decode an opaque/odd jmethodID into the backing ArtMethod pointer. */
uintptr_t DecodeArtMethod(JNIEnv *env, jobject reflected_method);

/** Install a clone+native-replacement route for one reflected executable. */
std::unique_ptr<MethodState> Install(JNIEnv *env, jobject reflected_method,
                                     NativeHookCallback callback, uintptr_t key,
                                     int stealth_override);

/** Publish the replacement in the router table after the owning HookItem is ready. */
bool Activate(MethodState &state);

/** Invoke the cloned original ArtMethod through JNI Call*MethodA. */
jobject InvokeOriginal(JNIEnv *env, const MethodState &state, jobject receiver,
                       jobjectArray args);

/**
 * rustFrida callback/hook.rs 把 HookContext* 绑定到回调对象（atoms.hook_ctx_ptr），
 * 使 $orig() 能用「进入时的原始寄存器」直调 trampoline，而不是经 JNI 重入
 * ArtMethod（original_call.rs:1137）。Vector 的 hooker 是 Java 侧对象，故改为
 * 把当前 dispatch 帧压入线程本地栈，供 InvokeOriginal 查询同一 target 的 ctx。
 */
void PushDispatchFrame(jmethodID target, HookContext *ctx, jobjectArray values);
void PopDispatchFrame(jmethodID target);
bool LookupDispatchFrame(jmethodID target, HookContext **ctx, jobjectArray *values);

/** Deoptimize one method through ART instrumentation when its symbols are available. */
bool Deoptimize(JNIEnv *env, const MethodState &state);

/** Diagnostic only, gated by debug.vector.art.frame_probe=1.
 *
 * Records one native-thunk frame into a fixed ring buffer and checks the one
 * geometric invariant the stack walker depends on: the generic JNI trampoline
 * publishes its own 224-byte frame as managed_stack_.top_quick_frame_, and our
 * 368-byte thunk frame must sit immediately below it. If that does not hold,
 * ART cannot advance from the JNI frame onto our frame and will read whatever
 * the wrong offset happens to contain instead of an ArtMethod.
 *
 * native_entry_route selects between the two frame shapes: the native
 * trampoline's 368-byte frame (16-byte method slot + 352-byte HookContext) and
 * the registered-native-entry route's 352-byte frame with no method slot.
 *
 * Does nothing (and costs one predictable branch) when the property is unset. */
void ProbeNativeThunkFrame(HookContext *ctx, bool native_entry_route);

/** Copies the probe ring out for post-mortem analysis. Returns the number of
 * entries written; safe to call at any time. */
size_t ReadNativeThunkFrameProbe(uint64_t *out, size_t capacity);

}  // namespace vector::native::art
