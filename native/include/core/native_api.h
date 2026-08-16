#pragma once

#include <dlfcn.h>

#include <string>
#include <utils/hook_helper.hpp>

#include "common/config.h"
#include "common/logging.h"

/**
 * @file native_api.h
 * @brief Internal wxshadow native hook helpers and compatibility ABI declarations.
 *
 * Vector's rustFrida-aligned module model treats module APKs as DEX/script inputs only. Target
 * processes should not load module-provided native libraries from APK paths. The legacy native
 * module ABI types remain here for binary/source compatibility, but Java registration is a no-op
 * and Vector's own native inline hooks are installed by the core wxshadow hook engine.
 */

// NOTE: The following type definitions form a public ABI for native modules.
// Do not change them without careful consideration for backward compatibility.

/*
 * =========================================================================================
 *  Vector Native API Interface
 * =========================================================================================
 *
 * These declarations describe the old native module ABI. New module code must not ship native
 * libraries for target-process loading; native inline hook functionality should be exposed through
 * Vector core APIs implemented inside the framework.
 *
 */

// Function pointer type for a native hooking implementation.
using HookFunType = int (*)(void *func, void *replace, void **backup);

// Function pointer type for a native unhooking implementation.
using UnhookFunType = int (*)(void *func);

// Callback function pointer that modules receive, invoked when any library is loaded.
using NativeOnModuleLoaded = void (*)(const char *name, void *handle);

/**
 * @struct NativeAPIEntries
 * @brief A struct containing function pointers exposed to native modules.
 */
struct NativeAPIEntries {
    uint32_t version;          // The version of this API struct.
    HookFunType hookFunc;      // Pointer to the function for inline  hooking.
    UnhookFunType unhookFunc;  // Pointer to the function for unhooking.
};

// NOTE: Module developers should not include the following INTERNAL definitions.

namespace vector::native {

// The entry point function that native modules must export (`native_init`).
using NativeInit = NativeOnModuleLoaded (*)(const NativeAPIEntries *entries);

/**
 * @brief Installs the hooks required for the native API to function.
 * @param handler The LSPlant hook handler.
 * @return True on success, false on failure.
 */
bool InstallNativeAPI(const lsplant::HookHandler &handler);

/**
 * @brief Legacy native-library registration entry point.
 *
 * Do not call from module paths. The Java bridge is intentionally a no-op in rustFrida-aligned
 * mode so module APKs do not cause foreign native mappings in target processes.
 *
 * @param library_name The filename of the native module's .so file (e.g., "libmymodule.so").
 */
void RegisterNativeLib(const std::string &library_name);

/**
 * @brief WxShadow inline hook implementation used by Vector's internal and module-facing native hooks.
 */
int HookInlineWxshadow(void *original, void *replace, void **backup);

/**
 * @brief WxShadow inline unhook implementation used by Vector's internal and module-facing native hooks.
 */
int UnhookInlineWxshadow(void *original);

/**
 * @brief Compatibility wrapper for existing Vector/LSPlant call sites.
 */
inline int HookInline(void *original, void *replace, void **backup) {
    if constexpr (kIsDebugBuild) {
        Dl_info info;
        if (dladdr(original, &info)) {
            LOGD("WxShadow inline hooking {} ({}) from {} ({})",
                 info.dli_sname ? info.dli_sname : "(unknown symbol)",
                 info.dli_saddr ? info.dli_saddr : original,
                 info.dli_fname ? info.dli_fname : "(unknown file)", info.dli_fbase);
        }
    }
    return HookInlineWxshadow(original, replace, backup);
}

/**
 * @brief Compatibility wrapper for existing Vector/LSPlant call sites.
 */
inline int UnhookInline(void *original) {
    if constexpr (kIsDebugBuild) {
        Dl_info info;
        if (dladdr(original, &info)) {
            LOGD("WxShadow inline unhooking {} ({}) from {} ({})",
                 info.dli_sname ? info.dli_sname : "(unknown symbol)",
                 info.dli_saddr ? info.dli_saddr : original,
                 info.dli_fname ? info.dli_fname : "(unknown file)", info.dli_fbase);
        }
    }
    return UnhookInlineWxshadow(original);
}

}  // namespace vector::native
