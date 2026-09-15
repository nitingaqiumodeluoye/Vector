#pragma once

#include <stdint.h>

/**
 * Vector's native-module ABI, redeclared for this module.
 *
 * This mirrors native/include/core/native_api.h in the Vector repository. It is duplicated
 * rather than included because a module is built against the framework's *published* ABI, not
 * against framework internals: nothing but the four declarations below is contract.
 *
 * Differences from the LSPosed/libxposed header this module was ported from:
 *   - the function-pointer fields are `hookFunc` / `unhookFunc` (CamelCase) where upstream
 *     spells them `hook_func` / `unhook_func`; the field ORDER and types are unchanged, so the
 *     struct is ABI-identical and only the source spelling differs;
 *   - `version` is 2 in both.
 *
 * What the framework guarantees, and what a module may rely on:
 *   - Vector loads the ELF named in META-INF/xposed/native_init.list from sealed SharedMemory
 *     through its own loader. The Android linker is never involved in a module's load, so
 *     System.loadLibrary() cannot reach this file and must not be used.
 *   - `native_init` is the only entry point. JNI_OnLoad is NEVER called: Vector's loader runs
 *     DT_INIT / init_array and then looks up the exported `native_init` symbol by name.
 *   - `native_init` is deferred, and then invoked from a module-class context (a native method
 *     registered on a carrier class that belongs to the module's own ClassLoader). The reason
 *     is JNIEnv::FindClass: ART resolves it against the ClassLoader of the *current method's
 *     declaring class*, so a call made from the framework's own frame would resolve the
 *     framework and a module could never find its own Java classes. The deferral restores the
 *     calling context upstream gets for free, because there a module calls System.loadLibrary
 *     from its own code and native_init therefore runs on the module's stack.
 *   - `hookFunc` is a raw-replacement inline hook: `replace` receives the target's original
 *     arguments and return type (identical ABI, no trampoline wrapper), and calling `backup`
 *     runs the original. It returns 0 and a non-null `backup` on success. Hooking the same
 *     target twice fails — Vector's engine rejects a duplicate target rather than chaining.
 */
typedef int (*HookFunType)(void *func, void *replace, void **backup);
typedef int (*UnhookFunType)(void *func);
typedef void (*NativeOnModuleLoaded)(const char *name, void *handle);

typedef struct {
    uint32_t version;
    HookFunType hookFunc;
    UnhookFunType unhookFunc;
} NativeAPIEntries;

typedef NativeOnModuleLoaded (*NativeInit)(const NativeAPIEntries *entries);
