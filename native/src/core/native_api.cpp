#include "core/native_api.h"

#include <android/dlext.h>

#include <sys/mman.h>
#include <sys/prctl.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "art_hook/hook_engine.h"
#include "elf/elf_image.h"
#include "elf/symbol_cache.h"
#include "elf_loader.h"

/**
 * @file native_api.cpp
 * @brief Implementation of Vector's wxshadow native hook and core ELF loading helpers.
 */

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
std::mutex g_core_elf_registry_mutex;
std::unordered_map<std::string, std::unique_ptr<ElfLoader>> g_loaded_module_loaders;
std::unordered_map<std::string, void *> g_loaded_module_linker_handles;
std::unordered_set<std::string> g_module_native_libs;
std::unordered_set<void *> g_initialized_linker_module_handles;
std::unordered_set<std::string> g_loading_module_libraries;
std::list<NativeOnModuleLoaded> g_module_loaded_callbacks;

// native_init entrypoints deferred by LoadNativeLibraryFd (soLoader path). They must run from a
// module-class native-method context so that JNIEnv::FindClass inside them resolves the module
// ClassLoader: ART's GetClassLoader picks the declaring class of the CURRENT native method (or
// the nativeLoad override), and under soLoader the framework would otherwise be the resolving
// context. Kotlin fires them via NativeAPI.firePendingNativeInits after the module ClassLoader
// exists. rawVector got this for free because the module itself called System.loadLibrary.
struct PendingNativeInit {
    std::string library_name;
    NativeInit init;
};
std::vector<PendingNativeInit> g_pending_native_inits;

// Temporary diagnostic build switch requested for comparing Vector soLoader against Android linker.
// Keep the Java/daemon transport unchanged (module ELF still arrives as sealed SharedMemory), but
// hand the FD to bionic linker via android_dlopen_ext instead of Vector's ElfLoader. This must not
// be kept enabled in the production soLoader path.
constexpr bool kDiagUseAndroidLinkerForModuleNative = false;

using DoDlopen = void *(*)(const char *name, int flags, const void *extinfo,
                           const void *caller_addr);
DoDlopen g_do_dlopen_backup = nullptr;
std::mutex g_do_dlopen_backup_mutex;
std::condition_variable g_do_dlopen_backup_ready;
std::once_flag g_native_load_monitor_once;
bool g_native_load_monitor_installed = false;

constexpr size_t kModuleHookExecPoolSize = 128 * 1024;
constexpr char kInitialHookExecVmaName[] = "wwb_hook_exec";
std::mutex g_module_hook_engine_mutex;
void *g_module_hook_exec_pool = nullptr;

std::unique_ptr<void, std::function<void(void *)>> g_api_page(
    mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0), [](void *ptr) {
        if (ptr != MAP_FAILED) munmap(ptr, 4096);
    });

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

bool IsWxshadowHookEngineReady() {
#if defined(__aarch64__)
    ExecPoolRange range{};
    return hook_engine_get_exec_ranges(&range, 1) > 0 && range.base != 0 && range.size != 0;
#else
    return false;
#endif
}

std::string FindMapLine(void *address) {
    if (address == nullptr) return "(null)";
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return "(maps unavailable)";

    const uintptr_t target = reinterpret_cast<uintptr_t>(address);
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        if (target >= static_cast<uintptr_t>(start) && target < static_cast<uintptr_t>(end)) {
            fclose(maps);
            const size_t len = strnlen(line, sizeof(line));
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            return line;
        }
    }
    fclose(maps);
    return "(not mapped)";
}

std::string FormatBytes(const void *address, size_t size) {
    if (address == nullptr || size == 0) return "";
    constexpr size_t kMaxBytes = 32;
    if (size > kMaxBytes) size = kMaxBytes;

    const auto *bytes = reinterpret_cast<const uint8_t *>(address);
    char buffer[kMaxBytes * 3] = {};
    size_t offset = 0;
    for (size_t i = 0; i < size && offset < sizeof(buffer); ++i) {
        const int written = snprintf(buffer + offset, sizeof(buffer) - offset, "%02x%s",
                                     bytes[i], i + 1 == size ? "" : " ");
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
    return std::string(buffer);
}

bool HasEnding(std::string_view full_string, std::string_view ending) {
    if (full_string.length() < ending.length()) return false;
    return full_string.compare(full_string.length() - ending.length(), std::string_view::npos,
                               ending) == 0;
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

    LOGI("Native module wxshadow inline hook installed: target={} replacement={} trampoline={} "
         "targetMap='{}' replacementMap='{}' trampolineMap='{}'",
         original, replace, trampoline, FindMapLine(original).c_str(),
         FindMapLine(replace).c_str(), FindMapLine(trampoline).c_str());

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

const NativeAPIEntries *g_native_api_entries = nullptr;

void InitializeApiEntries() {
    if (g_api_page.get() == MAP_FAILED) {
        LOGE("Failed to allocate NativeAPIEntries page");
        return;
    }
    auto *entries = new (g_api_page.get()) NativeAPIEntries{
        .version = 2,
        .hookFunc = &HookInlineWxshadow,
        .unhookFunc = &UnhookInlineWxshadow,
    };
    if (mprotect(g_api_page.get(), 4096, PROT_READ) != 0) {
        PLOGE("Failed to protect NativeAPIEntries page");
        return;
    }
    g_native_api_entries = entries;
}

bool EnsureNativeApi() {
    static const bool initialized = [] {
        InitializeApiEntries();
        return g_native_api_entries != nullptr;
    }();
    return initialized;
}

void NotifyNativeModuleCallbacks(const char *name, void *handle) {
    if (handle == nullptr) return;

    std::vector<NativeOnModuleLoaded> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        callbacks.assign(g_module_loaded_callbacks.begin(), g_module_loaded_callbacks.end());
    }

    for (const auto callback : callbacks) {
        try {
            callback(name, handle);
        } catch (...) {
            LOGE("Native module library-load callback threw for '{}'.",
                 name != nullptr ? name : "(null)");
        }
    }
}

void *DoDlopenHook(const char *name, int flags, const void *extinfo, const void *caller_addr) {
    // 2026-09-12 诊断打点（AGENTS.md 用户批准）：hook 进→wait 返回与 backup 执行耗时 ——
    // 用于定位 soLoader LoadDependencies 间歇性 7–10s 依赖链窗口是否落入本 hook。
    const auto hook_start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(g_do_dlopen_backup_mutex);
    g_do_dlopen_backup_ready.wait(lock, [] { return g_do_dlopen_backup != nullptr; });
    const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - hook_start)
                             .count();
    const auto backup = g_do_dlopen_backup;
    lock.unlock();

    void *handle = backup(name, flags, extinfo, caller_addr);
    const auto backup_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - hook_start)
                               .count();
    LOGI("[perf] do_dlopen_hook lib='{}' waitUs={} backupUs={} handle={}",
         name != nullptr ? name : "null", static_cast<long long>(wait_us),
         static_cast<long long>(backup_us), reinterpret_cast<void *>(handle));
    const std::string lib_name = name != nullptr ? name : "null";
    LOGV("do_dlopen hook triggered for library: '{}'", lib_name.c_str());
    if (handle != nullptr) {
        bool should_initialize_module = false;
        std::string matched_module_lib;
        {
            std::lock_guard<std::mutex> registry_lock(g_core_elf_registry_mutex);
            for (const auto &module_lib : g_module_native_libs) {
                if (HasEnding(lib_name, module_lib)) {
                    should_initialize_module =
                        g_initialized_linker_module_handles.insert(handle).second;
                    matched_module_lib = module_lib;
                    break;
                }
            }
        }

        if (should_initialize_module) {
            LOGW("DIAG linker detected registered native module loaded: name='{}' match='{}' handle={}",
                 lib_name.c_str(), matched_module_lib.c_str(), handle);
            void *init_sym = dlsym(handle, "native_init");
            if (init_sym == nullptr) {
                LOGE("DIAG linker module '{}' does not export native_init: {}",
                     lib_name.c_str(), dlerror());
            } else {
                try {
                    auto native_init = reinterpret_cast<NativeInit>(init_sym);
                    LOGW("DIAG linker native_init diagnostics for '{}': addr={} map='{}' bytes=[{}]",
                         lib_name.c_str(), init_sym, FindMapLine(init_sym).c_str(),
                         FormatBytes(init_sym, 16).c_str());
                    if (auto callback = native_init(g_native_api_entries); callback != nullptr) {
                        std::lock_guard<std::mutex> registry_lock(g_core_elf_registry_mutex);
                        g_module_loaded_callbacks.push_back(callback);
                        LOGW("DIAG linker native_init for '{}' registered library-load callback {}.",
                             lib_name.c_str(), reinterpret_cast<void *>(callback));
                    } else {
                        LOGW("DIAG linker native_init for '{}' completed without library-load callback.",
                             lib_name.c_str());
                    }
                } catch (...) {
                    LOGE("DIAG linker native_init threw for module '{}'.", lib_name.c_str());
                }
            }
        }
    }
    NotifyNativeModuleCallbacks(name, handle);
    return handle;
}

bool InstallNativeLibraryLoadMonitor() {
    std::call_once(g_native_load_monitor_once, [] {
        // Reference: the original Vector native ABI hooked this linker internal entrypoint to
        // deliver NativeOnModuleLoaded(name, handle) after a successful target-library load.
        // The replacement is installed through Vector's wxshadow hook engine instead of LSPlant.
        const auto *linker = ElfSymbolCache::GetLinker();
        if (linker == nullptr) {
            LOGE("Cannot install native library-load monitor: linker image unavailable.");
            return;
        }
        void *target = linker->getSymbAddress<void *>(
            "__dl__Z9do_dlopenPKciPK17android_dlextinfoPKv");
        if (target == nullptr) {
            LOGE("Cannot install native library-load monitor: do_dlopen symbol unavailable.");
            return;
        }

        void *backup = nullptr;
        if (HookInlineWxshadow(target, reinterpret_cast<void *>(&DoDlopenHook), &backup) != HOOK_OK ||
            backup == nullptr) {
            LOGE("Cannot install native library-load monitor through wxshadow.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_do_dlopen_backup_mutex);
            g_do_dlopen_backup = reinterpret_cast<DoDlopen>(backup);
        }
        g_do_dlopen_backup_ready.notify_all();
        g_native_load_monitor_installed = true;
    });
    return g_native_load_monitor_installed;
}
}  // namespace

int HookInlineWxshadow(void *original, void *replace, void **backup) {
    return HookModuleInlineWxshadow(original, replace, backup);
}

int UnhookInlineWxshadow(void *original) {
    return UnhookModuleInlineWxshadow(original);
}

void RegisterNativeLib(const std::string &library_name) {
    if (library_name.empty()) {
        LOGE("Cannot register empty native module library name.");
        return;
    }
    if (!EnsureNativeApi()) return;
    if (!InstallNativeLibraryLoadMonitor()) return;

    std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
    if (!g_module_native_libs.insert(library_name).second) {
        LOGD("DIAG linker native module library '{}' is already registered.",
             library_name.c_str());
        return;
    }
    LOGW("DIAG linker native module library '{}' has been registered.",
         library_name.c_str());
}

bool LoadNativeLibraryFd(int fd, size_t size, const std::string &library_name) {
    if (fd < 0 || size == 0 || library_name.empty()) {
        LOGE("Invalid module ELF input name='{}' fd={} size={}", library_name, fd, size);
        return false;
    }
    if (!EnsureNativeApi()) return false;
    if (!InstallNativeLibraryLoadMonitor()) return false;
    LOGI("Vector soLoader loading module ELF '{}' fd={} size={}", library_name.c_str(), fd, size);
    const auto load_start = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        if (g_loaded_module_loaders.contains(library_name)) {
            LOGI("Module ELF '{}' is already loaded through Vector soLoader.", library_name.c_str());
            return true;
        }
        if (g_loaded_module_linker_handles.contains(library_name)) {
            LOGI("Module ELF '{}' is already loaded through diagnostic Android linker path.",
                 library_name.c_str());
            return true;
        }
        if (!g_loading_module_libraries.insert(library_name).second) {
            LOGE("Module ELF '{}' is already being loaded.", library_name.c_str());
            return false;
        }
    }

    const auto finish_loading = [&library_name]() {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        g_loading_module_libraries.erase(library_name);
    };

    if constexpr (kDiagUseAndroidLinkerForModuleNative) {
        LOGW("DIAG linker loader enabled for module ELF '{}' fd={} size={}; bypassing Vector soLoader for this build.",
             library_name.c_str(), fd, size);

        android_dlextinfo extinfo{};
        extinfo.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_FORCE_LOAD;
        extinfo.library_fd = fd;
        void *handle = android_dlopen_ext(library_name.c_str(), RTLD_NOW, &extinfo);
        if (handle == nullptr) {
            LOGE("DIAG linker loader failed for module '{}': {}", library_name.c_str(), dlerror());
            finish_loading();
            return false;
        }
        LOGW("DIAG linker loader linked module ELF '{}' handle={} map='{}'.",
             library_name.c_str(), handle, FindMapLine(handle).c_str());

        auto init_sym = dlsym(handle, "native_init");
        if (init_sym == nullptr) {
            LOGE("DIAG linker loader module '{}' does not export native_init: {}",
                 library_name.c_str(), dlerror());
            finish_loading();
            return false;
        }

        try {
            auto native_init = reinterpret_cast<NativeInit>(init_sym);
            LOGW("DIAG linker native_init diagnostics for '{}': addr={} map='{}' bytes=[{}]",
                 library_name.c_str(), init_sym, FindMapLine(init_sym).c_str(),
                 FormatBytes(init_sym, 16).c_str());
            LOGW("DIAG linker calling native_init for module ELF '{}' at {}",
                 library_name.c_str(), init_sym);
            if (auto callback = native_init(g_native_api_entries); callback != nullptr) {
                std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
                g_module_loaded_callbacks.push_back(callback);
                LOGW("DIAG linker native_init for '{}' registered library-load callback {}.",
                     library_name.c_str(), reinterpret_cast<void *>(callback));
            } else {
                LOGW("DIAG linker native_init for '{}' completed without library-load callback.",
                     library_name.c_str());
            }
        } catch (...) {
            LOGE("DIAG linker native_init threw for module '{}'.", library_name.c_str());
            finish_loading();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
            g_loading_module_libraries.erase(library_name);
            g_loaded_module_linker_handles.emplace(library_name, handle);
        }
        return true;
    }

    auto loader = std::make_unique<ElfLoader>();
    if (!loader->LoadLibraryFd(fd, size, library_name.c_str())) {
        LOGE("Vector soLoader failed for module '{}'.", library_name.c_str());
        finish_loading();
        return false;
    }
    LOGI("Vector soLoader linked module ELF '{}'.", library_name.c_str());

    auto init_sym = loader->GetSymbol("native_init");
    if (init_sym == nullptr) {
        LOGE("Module '{}' does not export native_init.", library_name.c_str());
        finish_loading();
        return false;
    }

    // Defer native_init: firing it here would run inside NativeAPI.loadNativeLibraries, whose
    // declaring class belongs to the framework, so FindClass inside native_init would resolve the
    // framework ClassLoader and every module resolving its own Java classes would break. The
    // entrypoint runs later from a module-class context via FirePendingNativeInits.
    auto native_init = reinterpret_cast<NativeInit>(init_sym);
    LOGI("native_init for '{}' deferred to module-class context: addr={} map='{}'",
         library_name.c_str(), init_sym, FindMapLine(init_sym).c_str());
    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        g_pending_native_inits.push_back({library_name, native_init});
    }
    LOGI("[perf] load_lib stage=native_init_deferred elapsedUs={}",
         static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - load_start)
             .count()));

    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        g_loading_module_libraries.erase(library_name);
        g_loaded_module_loaders.emplace(library_name, std::move(loader));
    }
    LOGI("[perf] load_lib stage=finish_total elapsedUs={}",
         static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - load_start)
             .count()));
    return true;
}

namespace {

// Runs deferred native_init entrypoints and collects their library-load callbacks. Called either
// from NativeInitTrampoline (module-class context) or directly (framework-context fallback).
void RunPendingNativeInits(const std::vector<PendingNativeInit> &pending) {
    for (const auto &item : pending) {
        try {
            if (auto callback = item.init(g_native_api_entries); callback != nullptr) {
                std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
                g_module_loaded_callbacks.push_back(callback);
                LOGI("native_init for '{}' registered library-load callback {}.",
                     item.library_name.c_str(), reinterpret_cast<void *>(callback));
            } else {
                LOGI("native_init for '{}' completed without library-load callback.",
                     item.library_name.c_str());
            }
        } catch (...) {
            LOGE("native_init threw for module '{}'.", item.library_name.c_str());
        }
    }
}

// Trampoline bound onto the module-defined carrier class (NativeInitFire.fireNativeInit) by
// FirePendingNativeInits. While ART executes this native method, the "current method" belongs to
// the module ClassLoader, so JNIEnv::FindClass calls made by the deferred native_init
// entrypoints resolve the module's own Java classes (rawVector semantics; see GetClassLoader in
// ART's jni_internal.cc).
void NativeInitTrampoline(JNIEnv * /*env*/, jclass /*clazz*/) {
    std::vector<PendingNativeInit> pending;
    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        pending.swap(g_pending_native_inits);
    }
    RunPendingNativeInits(pending);
}

}  // namespace

bool FirePendingNativeInits(JNIEnv *env, jclass carrier_class) {
    if (!EnsureNativeApi()) return false;

    // NOTE: pending is NOT drained here. The carrier-class trampoline (NativeInitTrampoline)
    // performs the swap itself, exactly once, while ART executes the module-defined native
    // method. Draining here would race/regress into the trampoline swapping an empty list.
    size_t pending_count = 0;
    {
        std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
        pending_count = g_pending_native_inits.size();
    }
    LOGI("FirePendingNativeInits: pending_count={} carrier_class={}", pending_count,
         static_cast<const void *>(carrier_class));
    if (pending_count == 0) return true;

    if (env == nullptr || carrier_class == nullptr) {
        // Fallback for modules whose ClassLoader could not carry the NativeInitFire dex: fire
        // from the current (framework) context. FindClass inside native_init will not see module
        // classes — same as the pre-deferral behavior.
        std::vector<PendingNativeInit> pending;
        {
            std::lock_guard<std::mutex> lock(g_core_elf_registry_mutex);
            pending.swap(g_pending_native_inits);
        }
        LOGW("Firing {} deferred native_init(s) without a module-class context; FindClass "
             "inside them resolves the framework ClassLoader.",
             pending.size());
        RunPendingNativeInits(pending);
        return true;
    }

    const JNINativeMethod methods[] = {
        {"fireNativeInit", "()V", reinterpret_cast<void *>(&NativeInitTrampoline)},
    };
    if (env->RegisterNatives(carrier_class, methods, 1) != JNI_OK) {
        LOGE("Failed to register the native_init trampoline on the module carrier class.");
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
        return false;
    }

    jmethodID mid = env->GetStaticMethodID(carrier_class, "fireNativeInit", "()V");
    if (mid == nullptr) {
        LOGE("Module carrier class does not expose fireNativeInit()V.");
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
        return false;
    }

    env->CallStaticVoidMethod(carrier_class, mid);
    if (env->ExceptionCheck() == JNI_TRUE) {
        LOGE("native_init carrier invocation threw; clearing the exception.");
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
    }
    return true;
}

}  // namespace vector::native
