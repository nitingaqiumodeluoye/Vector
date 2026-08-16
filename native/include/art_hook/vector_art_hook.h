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

/** Initialize the ARM64 WxShadow engine and discover the current ART layout. */
bool Initialize(JNIEnv *env);

/** Whether the ARM64 backend was initialized successfully. */
bool IsAvailable();

/** Decode an opaque/odd jmethodID into the backing ArtMethod pointer. */
uintptr_t DecodeArtMethod(JNIEnv *env, jobject reflected_method);

/** Install a clone+native-replacement route for one reflected executable. */
std::unique_ptr<MethodState> Install(JNIEnv *env, jobject reflected_method,
                                     NativeHookCallback callback, uintptr_t key);

/** Publish the replacement in the router table after the owning HookItem is ready. */
bool Activate(MethodState &state);

/** Invoke the cloned original ArtMethod through JNI Call*MethodA. */
jobject InvokeOriginal(JNIEnv *env, const MethodState &state, jobject receiver,
                       jobjectArray args);

/** Deoptimize one method through ART instrumentation when its symbols are available. */
bool Deoptimize(JNIEnv *env, const MethodState &state);

}  // namespace vector::native::art
