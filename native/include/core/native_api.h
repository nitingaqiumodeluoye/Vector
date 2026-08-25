#pragma once

#include <dlfcn.h>

#include <string>
#include <cstddef>
#include "common/config.h"
#include "common/logging.h"

/**
 * @file native_api.h
 * @brief Internal wxshadow native hook helpers and compatibility ABI declarations.
 *
 * Module APK native ELF images are accepted only through Vector's anonymous soLoader path.
 * Android linker paths remain forbidden for module code.
 */

// NOTE: Module code receives these entries only after Vector soLoader has mapped its ELF.

/*
 * =========================================================================================
 *  Vector Native API Interface
 * =========================================================================================
 *
 * These declarations describe the native module ABI exposed by Vector's soLoader.
 *
 */

// Function pointer type for a native hooking implementation.
using HookFunType = int (*)(void *func, void *replace, void **backup);

// Function pointer type for a native unhooking implementation.
using UnhookFunType = int (*)(void *func);

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

using NativeInit = NativeOnModuleLoaded (*)(const NativeAPIEntries *entries);

/** Compatibility registration entrypoint; it must not invoke Android linker loading. */
void RegisterNativeLib(const std::string &library_name);

/**
 * Loads one module ELF image from sealed SharedMemory through Vector's anonymous ELF loader and
 * invokes its native_init entrypoint. APK paths and Android linker APIs are not accepted.
 */
bool LoadNativeLibraryFd(int fd, size_t size, const std::string &library_name);

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
