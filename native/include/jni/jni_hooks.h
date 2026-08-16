#pragma once

#include <jni.h>

/**
 * @file jni_hooks.h
 * @brief Declares the registration functions for all JNI bridge modules.
 */

namespace vector::native::jni {

/// Selects the LSPlant-only bootstrap path used by Vector's parasitic manager.
void SetManagerProcess(bool is_manager);

/// Records whether LSPlant has been initialized in this process.
void SetLsplantInitialized(bool initialized);

/// Returns whether LSPlant fallback hooks may be used in this process.
bool IsLsplantInitialized();

/// Registers the JNI methods for the HookBridge.
void RegisterHookBridge(JNIEnv *env);

/// Registers the JNI methods for the NativeApiBridge.
void RegisterNativeApiBridge(JNIEnv *env);

/// Registers the JNI methods for the ResourcesHook bridge.
void RegisterResourcesHook(JNIEnv *env);

}  // namespace vector::native::jni
