#include "core/native_api.h"

#include <sys/mman.h>
#include <sys/prctl.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>

#include "common/logging.h"
#include "art_hook/hook_engine.h"
#include "elf/elf_image.h"
#include "elf/symbol_cache.h"

/**
 * @file native_api.cpp
 * @brief Implementation of the native module loading and API provisioning system.
 */

using lsplant::operator""_sym;
/*
 * ===========================================================================================
 * LSPLANT HOOKING DSL (DOMAIN SPECIFIC LANGUAGE) DOCUMENTATION
 * ===========================================================================================
 *
 * This source file utilizes the 'lsplant' library, which implements a C++20 Hooking DSL.
 * Unlike traditional C-style hooking (which relies on void* casting, manual trampolines,
 * and global function pointers), this DSL uses compile-time metaprogramming to ensure
 * type safety and encapsulate hooking logic.
 *
 * -------------------------------------------------------------------------------------------
 * 1. SYNTAX ANATOMY
 * -------------------------------------------------------------------------------------------
 * The hooking syntax follows this pattern:
 *    "SYMBOL_NAME"_sym .hook ->* [] <auto backup> (args...) { ...body... };
 *
 *  A. "SYMBOL_NAME"_sym
 *     - This is a C++ User-Defined Literal (UDL). It converts the string literal into a
 *       compile-time 'Symbol' type.
 *     - For C++ mangled names (common in Android system libs), you must provide the full
 *       mangled signature (e.g., "__dl__Z9do_dlopen...").
 *
 *  B. Multi-Architecture Support (| Operator)
 *     - Android often requires different symbol names for 32-bit (ARM) and 64-bit (ARM64).
 *     - The DSL supports the pipe operator '|' to select the correct symbol at compile time:
 *       ("Sym32"_sym | "Sym64"_sym)
 *
 *  C. .hook ->*
 *     - '.hook' accesses the hook injection mechanism.
 *     - '->*' (Member Pointer Operator) is overloaded to bind the symbol to the lambda.
 *
 *  D. The Template Lambda (The Replacement)
 *     - Syntax: [] <lsplant::Backup auto backup> (Type arg1, Type arg2...) { ... }
 *     - This is a C++20 Template Lambda.
 *     - 'backup': Represents the ORIGINAL function (trampoline).
 *                 You call this to execute the original system logic.
 *     - 'args...': Must match the signature of the target function exactly.
 *
 * -------------------------------------------------------------------------------------------
 * 2. EXAMPLE USAGE
 * -------------------------------------------------------------------------------------------
 * inline static auto my_hook =
 *     "__open"_sym.hook ->* []<auto backup>(const char* path, int flags) {
 *         // 1. Pre-processing (Before original)
 *         LOGD("Opening file: %s", path);
 *
 *         // 2. Call Original (The "Backup")
 *         int result = backup(path, flags);
 *
 *         // 3. Post-processing (After original)
 *         return result;
 *     };
 *
 * -------------------------------------------------------------------------------------------
 * 3. REGISTRATION
 * -------------------------------------------------------------------------------------------
 * Defining the hook variable does not apply it.
 * You must pass the variable to the HookHandler to modify memory: handler(my_hook).
 * ===========================================================================================
 */

namespace vector::native {

namespace {
// Mutex to protect access to the global module lists.
std::mutex g_module_registry_mutex;
// List of callback functions provided by loaded native modules.
std::list<NativeOnModuleLoaded> g_module_loaded_callbacks;
// List of native library filenames that are registered as modules.
std::list<std::string> g_module_native_libs;

constexpr size_t kModuleHookExecPoolSize = 128 * 1024;
constexpr char kInitialHookExecVmaName[] = "wwb_hook_exec";
std::mutex g_module_hook_engine_mutex;
void *g_module_hook_exec_pool = nullptr;

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

// A smart pointer to a memory page that will hold the NativeAPIEntries struct.
std::unique_ptr<void, std::function<void(void *)>> g_api_page(
    mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0), [](void *ptr) {
        if (ptr != MAP_FAILED) {
            munmap(ptr, 4096);
        }
    });

bool IsWxshadowHookEngineReady() {
#if defined(__aarch64__)
    ExecPoolRange range{};
    return hook_engine_get_exec_ranges(&range, 1) > 0 && range.base != 0 && range.size != 0;
#else
    return false;
#endif
}

void SetInitialHookExecVmaName(void *base, size_t size) {
    // Source reference: rustFrida agent/src/quickjs_loader.rs::ExecMemory::new_near
    // names the initial hook-engine exec_mem as "wwb_hook_exec". Extension pools are named
    // "wwb_hook_pool" in quickjs-hook/src/hook_engine_mem.c::create_pool_near_range_sized.
    if (!base || base == MAP_FAILED || size == 0) return;
    errno = 0;
    int ret = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, reinterpret_cast<unsigned long>(base), size,
                    reinterpret_cast<unsigned long>(kInitialHookExecVmaName));
    if (ret != 0) {
        LOGW("Native module wxshadow hook pool VMA name failed: base={} size={} name={} errno={}",
             base, size, kInitialHookExecVmaName, errno);
    }
}

uintptr_t FindLibartExecutableBase() {
    // Source reference: rustFrida agent/src/quickjs_loader.rs::find_libart_base
    // uses the r-xp libart.so mapping as hook_mmap_near() hint.
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long map_start = 0;
        if (sscanf(line, "%lx-", &map_start) != 1) continue;
        if (strstr(line, "libart.so") && strstr(line, "r-xp")) {
            base = static_cast<uintptr_t>(map_start);
            break;
        }
    }
    fclose(maps);
    return base;
}

bool EnsureModuleWxshadowHookEngine() {
#if defined(__aarch64__)
    std::lock_guard lock(g_module_hook_engine_mutex);
    if (IsWxshadowHookEngineReady()) {
        return true;
    }

    const uintptr_t libart_hint = FindLibartExecutableBase();
    void *pool = hook_mmap_near(reinterpret_cast<void *>(libart_hint),
                                kModuleHookExecPoolSize);
    if (pool == MAP_FAILED) {
        PLOGE("Failed to allocate native module wxshadow hook pool near libart");
        return false;
    }
    SetInitialHookExecVmaName(pool, kModuleHookExecPoolSize);

    // Source reference: rustFrida agent/src/quickjs_loader.rs::init_hook_runtime
    // passes an externally owned RWX exec_mem region into hook_engine_init().
    if (hook_engine_init(pool, kModuleHookExecPoolSize) != HOOK_OK) {
        LOGE("Failed to initialize native module wxshadow hook engine");
        munmap(pool, kModuleHookExecPoolSize);
        return false;
    }

    g_module_hook_exec_pool = pool;
    LOGI("Native module wxshadow hook engine initialized: pool={} size={}", g_module_hook_exec_pool,
         kModuleHookExecPoolSize);
    return true;
#else
    return false;
#endif
}

int HookModuleInlineWxshadow(void *original, void *replace, void **backup) {
#if defined(__aarch64__)
    if (!original || !replace || !backup) return HOOK_ERROR_INVALID_PARAM;
    *backup = nullptr;

    if (!EnsureModuleWxshadowHookEngine()) return HOOK_ERROR_NOT_INITIALIZED;

    // Source reference: rustFrida quickjs-hook/src/hook_engine_inline.c::hook_install.
    // NativeAPI's ABI is raw replacement + backup trampoline, so this maps to
    // hook_install(), not hook_replace() (HookContext callback ABI).
    void *trampoline = hook_install(original, replace, 1);
    if (!trampoline) {
        LOGE("Native module wxshadow inline hook failed: target={} replacement={}", original,
             replace);
        return HOOK_ERROR_WXSHADOW_FAILED;
    }

    *backup = trampoline;
    return HOOK_OK;
#else
    (void)original;
    (void)replace;
    (void)backup;
    return HOOK_ERROR_NOT_INITIALIZED;
#endif
}

int UnhookModuleInlineWxshadow(void *original) {
#if defined(__aarch64__)
    if (!original) return HOOK_ERROR_INVALID_PARAM;
    if (!EnsureModuleWxshadowHookEngine()) return HOOK_ERROR_NOT_INITIALIZED;

    // Source reference: rustFrida quickjs-hook/src/jsapi/hook_api/mod.rs::remove_single_hook
    // removes native inline hooks through hook_remove().
    return hook_remove(original);
#else
    (void)original;
    return HOOK_ERROR_NOT_INITIALIZED;
#endif
}
}  // namespace

int HookInlineWxshadow(void *original, void *replace, void **backup) {
    return HookModuleInlineWxshadow(original, replace, backup);
}

int UnhookInlineWxshadow(void *original) {
    return UnhookModuleInlineWxshadow(original);
}

// The read-only, statically available Native API entry points for modules.
const NativeAPIEntries *g_native_api_entries = nullptr;

/**
 * @brief Initializes the Native API entries struct and makes it read-only.
 */
void InitializeApiEntries() {
    if (g_api_page.get() == MAP_FAILED) {
        LOGF("Failed to allocate memory for native API entries.");
        LOGD("Release the memory page pointer %p", g_api_page.release());
        return;
    }
    auto *entries = new (g_api_page.get()) NativeAPIEntries{
        .version = 2,
        .hookFunc = &HookInlineWxshadow,
        .unhookFunc = &UnhookInlineWxshadow,
    };
    if (mprotect(g_api_page.get(), 4096, PROT_READ) != 0) {
        PLOGE("Failed to mprotect API page to read-only");
    }
    g_native_api_entries = entries;
    LOGI("Native API entries initialized and protected.");
}

void RegisterNativeLib(const std::string &library_name) {
    static bool is_initialized = []() {
        InitializeApiEntries();
        return InstallNativeAPI(lsplant::InitInfo{
            .inline_hooker =
                [](void *target, void *replacement) {
                    void *backup = nullptr;
                    return HookInline(target, replacement, &backup) == 0 ? backup : nullptr;
                },
            .art_symbol_resolver =
                [](auto symbol) { return ElfSymbolCache::GetLinker()->getSymbAddress(symbol); },
        });
    }();

    if (!is_initialized) {
        LOGE("Cannot register module '{}' because native API failed to initialize.",
             library_name.c_str());
        return;
    }

    std::lock_guard<std::mutex> lock(g_module_registry_mutex);
    // The list is walked on every dlopen in the process and never shrinks - there is no
    // unregistration, and hot reload records a module's names again for each new generation - so
    // without this it grows without bound and every dlopen pays for the duplicates.
    if (std::find(g_module_native_libs.begin(), g_module_native_libs.end(), library_name) !=
        g_module_native_libs.end()) {
        LOGD("Native module library '{}' is already registered.", library_name.c_str());
        return;
    }
    g_module_native_libs.push_back(library_name);
    LOGD("Native module library '{}' has been registered.", library_name.c_str());
}

bool HasEnding(std::string_view fullString, std::string_view ending) {
    if (fullString.length() >= ending.length()) {
        return (fullString.compare(fullString.length() - ending.length(), std::string_view::npos,
                                   ending) == 0);
    }
    return false;
}

inline static auto do_dlopen_hook =
    "__dl__Z9do_dlopenPKciPK17android_dlextinfoPKv"_sym.hook->*
    []<lsplant::Backup auto backup>(const char *name, int flags, const void *extinfo,
                                    const void *caller_addr) static -> void * {
    void *handle = backup(name, flags, extinfo, caller_addr);
    const std::string lib_name = (name != nullptr) ? name : "null";
    LOGV("do_dlopen hook triggered for library: '{}'", lib_name.c_str());

    if (handle == nullptr) return nullptr;

    std::lock_guard<std::mutex> lock(g_module_registry_mutex);

    for (std::string_view module_lib : g_module_native_libs) {
        if (HasEnding(lib_name, module_lib)) {
            LOGI("Detected registered native module being loaded: '{}'", lib_name.c_str());
            void *init_sym = dlsym(handle, "native_init");
            if (init_sym == nullptr) {
                LOGW("Library '{}' matches a module name but does not export 'native_init'.",
                     lib_name.c_str());
                break;
            }
            auto native_init = reinterpret_cast<NativeInit>(init_sym);
            if (auto callback = native_init(g_native_api_entries)) {
                g_module_loaded_callbacks.push_back(callback);
                LOGI("Initialized native module '{}' and registered its callback.",
                     lib_name.c_str());
            }
            break;
        }
    }

    for (const auto &callback : g_module_loaded_callbacks) {
        callback(name, handle);
    }

    return handle;
};

bool InstallNativeAPI(const lsplant::HookHandler &handler) { return handler(do_dlopen_hook); }

}  // namespace vector::native
