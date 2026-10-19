#include "art_hook/vector_art_hook.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/system_properties.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "art_hook/hook_engine.h"
#include "common/logging.h"
#include "elf/elf_image.h"
#include "elf/symbol_cache.h"

namespace vector::native::art {

// Defined with the frame-probe helpers further down, but the OAT header layer
// (defined earlier) uses it to decide whether app processes install the
// logging-only variant of that hook.
bool FrameProbeEnabled();

// Defined outside the anonymous namespace below (and outside its __aarch64__ guard) because the
// perf log sites in core/native_api.cpp and jni/native_api_bridge.cpp gate on it too. See the
// declaration in native/include/art_hook/vector_art_hook.h for the accepted sources.
bool ArtPerfEnabled() {
    const char *value = getenv("VECTOR_ART_HOOK_PERF");
    if (value &&
        (strcmp(value, "1") == 0 ||
         strcmp(value, "true") == 0 ||
         strcmp(value, "TRUE") == 0 ||
         strcmp(value, "yes") == 0 ||
         strcmp(value, "YES") == 0 ||
         strcmp(value, "on") == 0 ||
         strcmp(value, "ON") == 0)) {
        return true;
    }
    char property[PROP_VALUE_MAX] = {};
    for (const char *name : {"debug.vector.art.perf", "persist.vector.art.perf"}) {
        if (__system_property_get(name, property) > 0 &&
            (strcmp(property, "1") == 0 ||
             strcmp(property, "true") == 0 ||
             strcmp(property, "TRUE") == 0 ||
             strcmp(property, "yes") == 0 ||
             strcmp(property, "YES") == 0 ||
             strcmp(property, "on") == 0 ||
             strcmp(property, "ON") == 0)) {
            return true;
        }
    }
    return false;
}

// Forward declaration for the WalkStack forensic probe, defined below at namespace
// scope. Declared here because the controller installs it before the GC layer.
size_t InitializeWalkStackProbe();

namespace {

#if defined(__aarch64__)

constexpr uint32_t kAccStatic = 0x0008u;
constexpr uint32_t kAccNative = 0x0100u;
constexpr uint64_t kPacStripMask = 0x0000FFFFFFFFFFFFULL;
constexpr uint32_t kAccCriticalNative = 0x00200000u;
constexpr uint32_t kAccFastNative = 0x00080000u;
constexpr uint32_t kAccNterpFastPath = 0x00100000u;
constexpr uint32_t kAccFastInterpreterToInterpreterInvoke = 0x40000000u;
constexpr uint32_t kAccPublicApi = 0x10000000u;
constexpr uint32_t kAccNterpInvokeFastPath = 0x00200000u;
constexpr size_t kInitialPoolSize = 128 * 1024;
constexpr char kHookExecPoolVmaName[] = "wwb_hook_exec";

uint64_t ArtPerfNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t ArtPerfElapsedUs(uint64_t start_ns) {
    if (start_ns == 0) return 0;
    const uint64_t now_ns = ArtPerfNowNs();
    return now_ns >= start_ns ? (now_ns - start_ns) / 1000ULL : 0;
}

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

void SetHookVmaName(void *base, size_t size, const char *name) {
    // Keep Vector's ART hook initial executable pool aligned with rustFrida:
    // agent/src/quickjs_loader.rs::ExecMemory::new_near names the initial exec_mem
    // "wwb_hook_exec". Extension pools remain "wwb_hook_pool" in
    // quickjs-hook/src/hook_engine_mem.c::create_pool_near_range_sized.
    if (!base || base == MAP_FAILED || size == 0) return;
    errno = 0;
    int ret = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME,
                    reinterpret_cast<unsigned long>(base), size,
                    reinterpret_cast<unsigned long>(name));
    if (ret != 0) {
        LOGW("WxShadow ART trace: failed to name exec pool base={} size={} name={} errno={}",
             base, size, name, errno);
    }
}

void SetHookExecPoolVmaName(void *base, size_t size) {
    SetHookVmaName(base, size, kHookExecPoolVmaName);
}

enum class ArtStealthMode : int {
    Normal = 0,
    WxShadow = 1,
};

enum class NativeRouteKind : unsigned char {
    NotNative,
    RegisteredNative,
    SharedNative,
    UnsupportedNative,
};

// Source: rustFrida jni_core.rs::probe_art_method_spec_frida NOISE_MASK. These bits are set on
// live ArtMethods by the interpreter/router but carry no method-kind meaning, so they must be
// masked out before the flags are read as a method description.
constexpr uint32_t kArtAccessFlagsNoiseMask =
    kAccFastInterpreterToInterpreterInvoke | kAccPublicApi | kAccNterpInvokeFastPath;

/**
 * Route classification for a native method.
 *
 * Source: rustFrida install.rs:311-319. The `@CriticalNative` flag alone is NOT authoritative:
 * vendor ART builds set kAccCriticalNative spuriously as ART noise (seen on MIUI 13, where
 * SystemProperties.native_get carries the bit while its signature is (String,String)String —
 * a shape the critical ABI cannot express). Treating the bare flag as authoritative made
 * WxShadow refuse methods it could have hooked.
 *
 * [critical_signature_supported] is therefore the caller's verdict — computed from the live
 * MethodState (static + primitive-only params/return), the same evidence rustFrida derives from
 * its parsed JNI signature — and must hold together with the flag before the method is refused.
 */
constexpr NativeRouteKind ClassifyNativeRoute(uint32_t access_flags, uintptr_t data,
                                               bool data_is_executable,
                                               bool data_is_in_art,
                                               bool critical_signature_supported) {
    if ((access_flags & kAccNative) == 0) return NativeRouteKind::NotNative;
    if ((access_flags & kAccCriticalNative) != 0 && critical_signature_supported) {
        return NativeRouteKind::UnsupportedNative;
    }
    if (data < 0x10000 || !data_is_executable || data_is_in_art) {
        // rustFrida treats a native method without an independently
        // registered fnPtr as a shared/ART-entry route. The entry-point
        // classifier below decides whether it can use the shared JNI router
        // or must remain on LSPlant; do not reject it at data_ probing time.
        return NativeRouteKind::SharedNative;
    }
    return NativeRouteKind::RegisteredNative;
}

static_assert(ClassifyNativeRoute(0, 0, false, false, true) == NativeRouteKind::NotNative);
static_assert(ClassifyNativeRoute(kAccNative, 0x10000, true, false, false) ==
              NativeRouteKind::RegisteredNative);
// A real @CriticalNative (flag + critical-callable signature) stays unsupported...
static_assert(ClassifyNativeRoute(kAccNative | kAccCriticalNative, 0x10000, true, false, true) ==
              NativeRouteKind::UnsupportedNative);
// ...while the same flag with a non-critical signature is ART noise and must hook normally.
static_assert(ClassifyNativeRoute(kAccNative | kAccCriticalNative, 0x10000, true, false, false) ==
              NativeRouteKind::RegisteredNative);
static_assert(ClassifyNativeRoute(kAccNative, 0, false, false, true) ==
              NativeRouteKind::SharedNative);
// Interpreter/router noise bits must not change the verdict.
static_assert(ClassifyNativeRoute(kAccNative | kArtAccessFlagsNoiseMask, 0x10000, true, false,
                                  false) == NativeRouteKind::RegisteredNative);

struct Layout {
    size_t method_size = 0;
    size_t access_flags_offset = 4;
    size_t data_offset = 0;
    size_t entry_offset = 0;
    void *jni_trampoline = nullptr;
};

// Mirrors rustFrida art_thread.rs for the supported API >= 23 layout. The
// enclosing Thread offset is still discovered from the live JNIEnv instead of
// being hard-coded, because vendor ART builds may move tlsPtr_.jni_env.
struct ArtThreadLayout {
    size_t managed_stack_offset = 0;
    size_t suspend_trigger_offset = 0;
    bool valid = false;
};

struct SharedNterpRouter {
    uintptr_t source = 0;
    uintptr_t target = 0;
    uintptr_t trampoline = 0;
};

// Matches rustFrida art_method/bridge.rs::find_classlinker_trampolines.
// art_quick_* symbols are LOCAL/HIDDEN on a number of ART builds, so the
// symbol-cache path must have a layout-validated ClassLinker fallback.
struct ClassLinkerTrampolines {
    uintptr_t generic_jni = 0;
    uintptr_t interpreter = 0;
    uintptr_t resolution = 0;
    uintptr_t imt_conflict = 0;
};

using ThreadCurrentFn = uintptr_t (*)();

Layout g_layout;
ArtThreadLayout g_thread_layout;
ThreadCurrentFn g_thread_current = nullptr;
SharedNterpRouter g_nterp_router;
SharedNterpRouter g_nterp_clinit_router;
SharedNterpRouter g_jni_router;
SharedNterpRouter g_interpreter_bridge_router;
SharedNterpRouter g_resolution_router;
std::array<uintptr_t, 4> g_do_call_targets{};
size_t g_do_call_target_count = 0;
std::array<uintptr_t, 8> g_gc_hook_targets{};
size_t g_gc_hook_target_count = 0;
uintptr_t g_oat_header_hook_target = 0;
std::mutex g_state_mutex;
std::vector<MethodState *> g_active_states;
std::atomic<bool> g_gc_sync_in_progress{false};
JNIEnv *g_init_env = nullptr;
std::once_flag g_quick_entrypoints_once;
uintptr_t g_quick_test_suspend_entrypoint = 0;
std::atomic<bool> g_sigchain_guard_installed{false};
std::atomic<bool> g_bionic_sigsegv_guard_installed{false};
struct sigaction g_previous_sigsegv_action {};
std::atomic<uintptr_t> g_implicit_suspend_entrypoint{0};
std::atomic<uintptr_t> g_implicit_suspend_trigger_offset{UINTPTR_MAX};
std::once_flag g_classlinker_discovery_once;
ClassLinkerTrampolines g_classlinker_trampolines;
ArtStealthMode g_art_stealth_mode = ArtStealthMode::Normal;
// Per-install stealth override. Set by art::Install when a caller (e.g. a framework
// hook such as LoadedApk.<init>) requests non-stealth for a single method. -1 = use global.
thread_local int g_art_stealth_override = -1;

// RAII guard that scopes a per-install stealth override to the current thread and
// restores the previous value on destruction (covers every return path out of Install).
struct ScopedStealthOverride {
    explicit ScopedStealthOverride(int override) : saved_(g_art_stealth_override) {
        g_art_stealth_override = override;
    }
    ~ScopedStealthOverride() { g_art_stealth_override = saved_; }
    ScopedStealthOverride(const ScopedStealthOverride &) = delete;
    ScopedStealthOverride &operator=(const ScopedStealthOverride &) = delete;
    int saved_;
};
bool g_is_system_process = false;
bool g_is_manager_process = false;
bool g_art_stealth_auto_selected = true;
std::string g_art_stealth_source = "default:auto";
std::string g_art_stealth_value = "auto";

bool ResolveRuntime(JNIEnv *env, uintptr_t *runtime);
int GetAndroidApiLevel();
void DiscoverClassLinkerTrampolines(JNIEnv *env);
jobject FindKnownNativeMethod(JNIEnv *env, uintptr_t *art_method);
uint32_t CompileDontBotherFlag();
void RegisterActiveState(MethodState *state);
void UnregisterActiveState(MethodState *state);
void SynchronizeReplacementMethods(const char *phase);
void *g_exec_pool = nullptr;
std::once_flag g_base_init_once;
std::once_flag g_bootstrap_init_once;
std::once_flag g_interpreter_router_once;
std::once_flag g_controller_init_once;
bool g_base_initialized = false;
bool g_bootstrap_initialized = false;
bool g_interpreter_router_initialized = false;
bool g_initialized = false;
bool g_thread_layout_found = false;
std::mutex g_install_mutex;
jfieldID g_art_method_field = nullptr;
std::once_flag g_art_method_field_once;

const char *ArtStealthModeName(ArtStealthMode mode) {
    return mode == ArtStealthMode::WxShadow ? "wxshadow" : "normal";
}

int ArtStealthFlag() {
    // Per-install override (e.g. framework hook requesting non-stealth) wins over the
    // process-wide mode. -1 means "use the global mode".
    if (g_art_stealth_override >= 0) return g_art_stealth_override;
    return static_cast<int>(g_art_stealth_mode);
}

void *InstallArtRouterWithFallback(void *target, uint32_t quickcode_offset,
                                   JNIEnv *env, void **out_hooked_target,
                                   int skip_resolve, uint64_t current_pc_hint,
                                   int use_blr, int pre_scan) {
    LOGI("[art-router-diag] install target={} offset={} skip_resolve={} pc_hint={} use_blr={} pre_scan={} stealth={}",
         target, quickcode_offset, skip_resolve,
         reinterpret_cast<void *>(current_pc_hint), use_blr, pre_scan, ArtStealthFlag());
    void *trampoline = hook_install_art_router(
        target, quickcode_offset, ArtStealthFlag(), env, out_hooked_target,
        skip_resolve, current_pc_hint, use_blr, pre_scan);
    if (trampoline || g_art_stealth_mode != ArtStealthMode::WxShadow ||
        !g_art_stealth_auto_selected) {
        return trampoline;
    }
    LOGW("WxShadow ART hook failed at {}; falling back to normal patch", target);
    g_art_stealth_mode = ArtStealthMode::Normal;
    hook_set_stealth_mode(0);
    return hook_install_art_router(
        target, quickcode_offset, 0, env, out_hooked_target,
        skip_resolve, current_pc_hint, use_blr, pre_scan);
}

bool IsWxShadowStealthMode() {
    return g_art_stealth_mode == ArtStealthMode::WxShadow;
}

std::string NormalizeStealthValue(const char *raw) {
    if (!raw) return {};
    std::string value(raw);
    const auto first = std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) == 0;
    });
    const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) == 0;
    }).base();
    if (first >= last) return {};
    std::string normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return normalized;
}

bool ParseArtStealthConfig(const char *raw, ArtStealthMode *mode, bool *auto_mode) {
    const std::string value = NormalizeStealthValue(raw);
    if (value.empty()) return false;
    if (value == "auto") {
        if (auto_mode) *auto_mode = true;
        return true;
    }
    if (auto_mode) *auto_mode = false;
    if (value == "1" || value == "wxshadow" || value == "shadow" ||
        value == "true" || value == "on" || value == "yes") {
        if (mode) *mode = ArtStealthMode::WxShadow;
        return true;
    }
    if (value == "0" || value == "normal" || value == "mprotect" ||
        value == "false" || value == "off" || value == "no") {
        if (mode) *mode = ArtStealthMode::Normal;
        return true;
    }
    return false;
}

bool ParseBooleanConfig(const char *raw, bool *enabled) {
    const std::string value = NormalizeStealthValue(raw);
    if (value.empty()) return false;
    if (value == "1" || value == "true" || value == "on" || value == "yes" ||
        value == "enable" || value == "enabled") {
        if (enabled) *enabled = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no" ||
        value == "disable" || value == "disabled") {
        if (enabled) *enabled = false;
        return true;
    }
    return false;
}

bool ReadArtStealthProperty(const char *name, std::string *value) {
    char buffer[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, buffer) <= 0 || buffer[0] == '\0') return false;
    if (value) *value = buffer;
    return true;
}

bool ReadConfiguredArtStealthMode(ArtStealthMode *mode, bool *auto_mode,
                                  std::string *source, std::string *raw_value) {
    const char *env_value = getenv("VECTOR_ART_HOOK_STEALTH");
    if (ParseArtStealthConfig(env_value, mode, auto_mode)) {
        if (source) *source = "env:VECTOR_ART_HOOK_STEALTH";
        if (raw_value) *raw_value = env_value ? env_value : "";
        return true;
    }
    for (const char *property :
         {"debug.vector.art.stealth", "persist.vector.art.stealth"}) {
        std::string property_value;
        if (!ReadArtStealthProperty(property, &property_value)) continue;
        if (ParseArtStealthConfig(property_value.c_str(), mode, auto_mode)) {
            if (source) *source = std::string("prop:") + property;
            if (raw_value) *raw_value = property_value;
            return true;
        }
        LOGW("WxShadow ART stealth config ignored: {}={}", property,
             property_value.c_str());
    }
    return false;
}

bool ReadConfiguredSharedLayer1(bool *enabled, std::string *source,
                                std::string *raw_value) {
    const char *env_value = getenv("VECTOR_ART_HOOK_SHARED_LAYER1");
    if (ParseBooleanConfig(env_value, enabled)) {
        if (source) *source = "env:VECTOR_ART_HOOK_SHARED_LAYER1";
        if (raw_value) *raw_value = env_value ? env_value : "";
        return true;
    }
    for (const char *property :
         {"debug.vector.art.shared_layer1", "persist.vector.art.shared_layer1"}) {
        std::string property_value;
        if (!ReadArtStealthProperty(property, &property_value)) continue;
        if (ParseBooleanConfig(property_value.c_str(), enabled)) {
            if (source) *source = std::string("prop:") + property;
            if (raw_value) *raw_value = property_value;
            return true;
        }
        LOGW("WxShadow ART shared Layer1 config ignored: {}={}", property,
             property_value.c_str());
    }
    return false;
}

bool UseSharedJniNterpLayer1() {
    if (!IsWxShadowStealthMode()) return false;

    bool enabled = false;
    std::string source;
    std::string raw_value;
    if (ReadConfiguredSharedLayer1(&enabled, &source, &raw_value)) {
        LOGI("WxShadow ART shared JNI/Nterp Layer1 selected: {} source={} value={}",
             enabled, source.c_str(), raw_value.c_str());
        return enabled;
    }

    // rustFrida reference:
    // - quickjs-hook/src/jsapi/java/art_controller.rs installs
    //   quick_generic_jni_trampoline and Nterp only when
    //   stealth_mode() != Normal.
    // - rust_frida/src/repl.rs preconfigures that mode only when the script
    //   explicitly declares Java.setStealth(...).
    //
    // Vector's default auto policy selects wxshadow only for system_server,
    // but that is not equivalent to rustFrida's explicit Java.setStealth(1/2).
    // Do not hook process-wide hot JNI/Nterp stubs in auto mode; Hunter executes
    // Nterp at very high frequency and that creates a wxshadow read/exec switch storm.
    return !g_art_stealth_auto_selected;
}

ArtStealthMode SelectArtStealthMode() {
    ArtStealthMode configured_mode = ArtStealthMode::Normal;
    bool auto_mode = true;
    std::string source = "default:auto";
    std::string raw_value = "auto";
    const bool configured =
        ReadConfiguredArtStealthMode(&configured_mode, &auto_mode, &source, &raw_value);
    g_art_stealth_auto_selected = auto_mode || !configured;
    g_art_stealth_source = source;
    g_art_stealth_value = raw_value;

    // Source reference: rustFrida quickjs-hook/src/jsapi/hook_api/registry.rs
    // defines StealthMode { Normal=0, WxShadow=1 }, and
    // art_controller.rs keeps the default Normal until Java.setStealth opts in.
    // Vector has no Java-facing setStealth bootstrap here, so auto applies the
    // process policy below before ART hooks lock the mode.
    if (auto_mode || !configured) {
        // rawVector installs the parasitic manager hooks before ActivityThread.attach. On this
        // Android 12 host, publishing WxShadow pages during that cold specialization window can
        // collide with framework native initialization before the process attaches. rustFrida's
        // default mode is Normal until stealth is explicitly requested, so keep the manager's
        // internal bootstrap hooks on that baseline while target apps remain WxShadow-first.
        if (g_is_manager_process) {
            LOGI("WxShadow ART stealth mode selected: normal source={} value={} "
                 "policy=manager-bootstrap-normal process=manager",
                 source.c_str(), raw_value.c_str());
            return ArtStealthMode::Normal;
        }
        // Auto is WxShadow-first. Do not create a temporary executable probe page:
        // rustFrida selects the mode before installing hooks and lets the real patch
        // operation report capability. A failed real patch remains an explicit hook
        // installation failure instead of racing Hunter's /proc/maps scanner.
        LOGI("WxShadow ART stealth mode selected: wxshadow source={} value={} "
             "policy=wxshadow-first process={} probe=skipped",
             source.c_str(), raw_value.c_str(),
             g_is_system_process ? "system_server" : "app");
        return ArtStealthMode::WxShadow;
    }

    LOGI("WxShadow ART stealth mode selected: {} source={} value={} probe=skipped",
         ArtStealthModeName(configured_mode), source.c_str(), raw_value.c_str());
    return configured_mode;
}

bool IsMappedExecutable(uintptr_t address) {
    address &= kPacStripMask;
    if (address < 0x1000) return false;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512];
    bool executable = false;
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3 &&
            address >= start && address < end) {
            executable = perms[2] == 'x';
            break;
        }
    }
    fclose(maps);
    return executable;
}

bool IsReadable(uintptr_t address, size_t size);

void LogArtEntryDiagnostic(const char *stage, uintptr_t address) {
    address &= kPacStripMask;
    if (address < 0x1000) {
        LOGI("[art-entry-diag] stage={} addr=null", stage);
        return;
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    char perms[5] = {};
    uintptr_t start = 0;
    uintptr_t end = 0;
    char mapping[256] = {};
    if (maps) {
        char line[512];
        while (fgets(line, sizeof(line), maps)) {
            uintptr_t lo = 0;
            uintptr_t hi = 0;
            char p[5] = {};
            char name[256] = {};
            if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %255[^\n]",
                       &lo, &hi, p, name) >= 3 && address >= lo && address < hi) {
                start = lo;
                end = hi;
                memcpy(perms, p, sizeof(perms));
                memcpy(mapping, name, sizeof(mapping) - 1);
                break;
            }
        }
        fclose(maps);
    }
    uint32_t words[4] = {};
    const bool readable = IsReadable(address, sizeof(words));
    if (readable) memcpy(words, reinterpret_cast<const void *>(address), sizeof(words));
    LOGI("[art-entry-diag] stage={} addr={} vma={}..{} perms={} map={} readable={} insn={:08x},{:08x},{:08x},{:08x}",
         stage, reinterpret_cast<void *>(address), reinterpret_cast<void *>(start),
         reinterpret_cast<void *>(end), perms[0] ? perms : "?", mapping,
         readable, words[0], words[1], words[2], words[3]);
}

bool IsReadable(uintptr_t address, size_t size) {
    if (address < 0x1000 || size == 0) return false;
    const uintptr_t end = address + size - 1;
    if (end < address) return false;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[512];
    bool readable = false;
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t start = 0;
        uintptr_t finish = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &finish, perms) == 3 &&
            address >= start && end < finish) {
            readable = perms[0] == 'r';
            break;
        }
    }
    fclose(maps);
    return readable;
}

bool FindModuleRange(const char *module_name, uintptr_t *start_out, uintptr_t *end_out) {
    if (!module_name || !start_out || !end_out) return false;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;
    uintptr_t start = std::numeric_limits<uintptr_t>::max();
    uintptr_t end = 0;
    char line[768];
    while (fgets(line, sizeof(line), maps)) {
        uintptr_t map_start = 0;
        uintptr_t map_end = 0;
        if (sscanf(line, "%lx-%lx", &map_start, &map_end) != 2) continue;
        if (strstr(line, module_name) == nullptr) continue;
        start = std::min(start, map_start);
        end = std::max(end, map_end);
    }
    fclose(maps);
    if (start == std::numeric_limits<uintptr_t>::max() || end <= start) return false;
    *start_out = start;
    *end_out = end;
    return true;
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

bool ReadExecutableModifiers(JNIEnv *env, jobject executable, jint *modifiers) {
    if (!env || !executable || !modifiers) return false;
    auto executable_class = env->FindClass("java/lang/reflect/Executable");
    if (!executable_class) return false;
    auto get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");
    if (!get_modifiers) {
        env->DeleteLocalRef(executable_class);
        return false;
    }
    *modifiers = env->CallIntMethod(executable, get_modifiers);
    const bool ok = !env->ExceptionCheck();
    if (!ok) env->ExceptionClear();
    env->DeleteLocalRef(executable_class);
    return ok;
}

uintptr_t ReflectedArtMethod(JNIEnv *env, jobject executable) {
    if (!env || !executable) return 0;
    return reinterpret_cast<uintptr_t>(env->FromReflectedMethod(executable));
}

void ResolveArtMethodField(JNIEnv *env) {
    if (!env) return;
    auto executable = env->FindClass("java/lang/reflect/Executable");
    if (!executable) {
        env->ExceptionClear();
        return;
    }
    g_art_method_field = env->GetFieldID(executable, "artMethod", "J");
    if (!g_art_method_field) env->ExceptionClear();
    env->DeleteLocalRef(executable);
}

uintptr_t DecodeReflectedArtMethod(JNIEnv *env, jobject executable) {
    const auto raw = ReflectedArtMethod(env, executable);
    if (raw == 0) return 0;

    // Prefer the reflection object's ArtMethod field whenever it is exposed.
    // This handles both direct and opaque jmethodIDs; the latter are not
    // guaranteed to be distinguishable solely by bit 0 on vendor ART builds.
    std::call_once(g_art_method_field_once, [env] { ResolveArtMethodField(env); });
    if (g_art_method_field) {
        const auto decoded = static_cast<uintptr_t>(
            env->GetLongField(executable, g_art_method_field));
        if (!env->ExceptionCheck() && decoded != 0) {
            return decoded & kPacStripMask;
        }
        env->ExceptionClear();
    }

    // On older ART versions the field may be absent; retain the direct ID
    // fallback, including the odd value so callers can reject it safely.
    return (raw & 1u) == 0 ? (raw & kPacStripMask) : raw;
}

std::vector<uintptr_t> CollectArtMethodAddresses(JNIEnv *env) {
    std::vector<uintptr_t> result;
    auto class_class = env->FindClass("java/lang/Class");
    auto object_class = env->FindClass("java/lang/Object");
    if (!class_class || !object_class) {
        if (class_class) env->DeleteLocalRef(class_class);
        if (object_class) env->DeleteLocalRef(object_class);
        env->ExceptionClear();
        return result;
    }
    auto get_declared_methods =
        env->GetMethodID(class_class, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    if (!get_declared_methods) {
        env->DeleteLocalRef(object_class);
        env->DeleteLocalRef(class_class);
        env->ExceptionClear();
        return result;
    }
    auto methods = reinterpret_cast<jobjectArray>(
        env->CallObjectMethod(object_class, get_declared_methods));
    if (!methods || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(object_class);
        env->DeleteLocalRef(class_class);
        return result;
    }
    const jsize count = env->GetArrayLength(methods);
    for (jsize i = 0; i < count; ++i) {
        jobject method = env->GetObjectArrayElement(methods, i);
        if (method) {
            const auto address = DecodeReflectedArtMethod(env, method);
            if (address != 0) result.push_back(address);
            env->DeleteLocalRef(method);
        }
    }
    env->DeleteLocalRef(methods);
    env->DeleteLocalRef(object_class);
    env->DeleteLocalRef(class_class);
    return result;
}

size_t InferMethodSize(JNIEnv *env, uintptr_t target) {
    auto addresses = CollectArtMethodAddresses(env);
    addresses.push_back(target);
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::array<size_t, 15> frequency{};
    size_t best = std::numeric_limits<size_t>::max();
    for (size_t i = 1; i < addresses.size(); ++i) {
        const uintptr_t delta = addresses[i] - addresses[i - 1];
        if (delta >= 16 && delta <= 128 && (delta % alignof(void *)) == 0) {
            const auto index = static_cast<size_t>((delta - 16) / 8);
            ++frequency[index];
            best = std::min(best, static_cast<size_t>(delta));
        }
    }
    size_t repeated = 0;
    size_t repeated_count = 0;
    for (size_t i = 0; i < frequency.size(); ++i) {
        if (frequency[i] > repeated_count) {
            repeated = 16 + i * 8;
            repeated_count = frequency[i];
        }
    }
    if (repeated_count > 0) return repeated;
    if (best != std::numeric_limits<size_t>::max()) return best;
    return 32;
}

bool IsInArtModule(uintptr_t address);

void *ResolveArtSymbol(const char *name) {
    const bool trace = name && strcmp(name, "_ZN3art7Runtime15DisableVerifierEv") == 0;
    if (const auto *art = ElfSymbolCache::GetArt()) {
        if (void *address = art->getSymbAddress<void *>(name)) {
            if (trace) {
                LOGI("[art-router-diag] ResolveArtSymbol name={} address={} cache={} exec={} art={}",
                     name, address, static_cast<const void *>(art),
                     IsMappedExecutable(reinterpret_cast<uintptr_t>(address)),
                     IsInArtModule(reinterpret_cast<uintptr_t>(address)));
            }
            return address;
        }
    }
    void *address = dlsym(RTLD_DEFAULT, name);
    if (trace) {
        LOGI("[art-router-diag] ResolveArtSymbol name={} address={} source=dlsym exec={} art={}",
             name, address,
             address && IsMappedExecutable(reinterpret_cast<uintptr_t>(address)),
             address && IsInArtModule(reinterpret_cast<uintptr_t>(address)));
    }
    return address;
}

bool ProbeArtThreadLayout(JNIEnv *env) {
    if (!env) return false;
    // Android's MTE/PAC builds may return a tagged JNIEnvExt pointer (for example
    // 0xb400...). rustFrida's safe_mem map checks operate on the canonical address;
    // strip the tag before both the maps check and the raw read. Keeping the tagged
    // value here makes the probe fail before it ever reaches Thread::tlsPtr_.jni_env.
    const auto raw_env_address = reinterpret_cast<uintptr_t>(env);
    const auto env_address = raw_env_address & kPacStripMask;
    if (!IsReadable(env_address + sizeof(uintptr_t), sizeof(uintptr_t))) {
        LOGW("WxShadow ART thread layout: JNIEnvExt unreadable at {}",
             reinterpret_cast<void *>(raw_env_address));
        return false;
    }

    // rustFrida art_thread.rs: JNIEnvExt starts with functions_ then self_.
    const auto raw_thread = *reinterpret_cast<volatile const uintptr_t *>(
        env_address + sizeof(uintptr_t));
    const auto thread = raw_thread & kPacStripMask;
    if (!IsReadable(thread + 144, 384 - 144)) {
        LOGW("WxShadow ART thread layout: Thread range unreadable env={} raw_thread={} thread={}",
             static_cast<void *>(env), reinterpret_cast<void *>(raw_thread),
             reinterpret_cast<void *>(thread));
        return false;
    }

    const auto stripped_env = env_address;
    for (size_t offset = 144; offset < 384; offset += sizeof(uintptr_t)) {
        const auto candidate = *reinterpret_cast<volatile const uintptr_t *>(thread + offset);
        if ((candidate & kPacStripMask) != stripped_env) continue;

        // API >= 23 in rustFrida: managed_stack_ is four pointer slots before
        // tlsPtr_.jni_env; ManagedStack::top_quick_frame_ is at offset zero.
        g_thread_layout.managed_stack_offset = offset - 4 * sizeof(uintptr_t);
        // Source reference: rustFrida art_thread.rs::probe_art_thread_spec.
        // tlsPtr_.suspend_trigger_ immediately precedes tlsPtr_.jni_env.
        g_thread_layout.suspend_trigger_offset = offset - sizeof(uintptr_t);
        g_thread_layout.valid = true;
        LOGI("WxShadow ART thread layout: Thread+{} managed_stack=Thread+{}",
             offset, g_thread_layout.managed_stack_offset);
        return true;
    }

    LOGW("WxShadow ART thread layout: JNIEnv not found in Thread offsets 144..384");
    return false;
}

bool IsInArtModule(uintptr_t address);

bool ShouldReplaceForManagedStack(uintptr_t replacement) {
    // Keep rustFrida's fail-open behavior: this guard is recursion protection,
    // not a prerequisite for ordinary per-method routing.
    if (!g_thread_current || !g_thread_layout.valid) return true;

    const auto thread = g_thread_current() & kPacStripMask;
    if (thread == 0) return true;
    const auto managed_stack = thread + g_thread_layout.managed_stack_offset;

    const auto top_quick_frame = *reinterpret_cast<volatile const uintptr_t *>(managed_stack);
    if (top_quick_frame != 0) {
        const auto frame = (top_quick_frame & ~uintptr_t{1}) & kPacStripMask;
        if (frame == 0) return true;
        const auto method =
            *reinterpret_cast<volatile const uintptr_t *>(frame) & kPacStripMask;
        return method != (replacement & kPacStripMask);
    }

    // API >= 23 ManagedStack::link_ is the second pointer slot. This matches
    // rustFrida should_replace_for_stack, including TaggedQuickFrame bit 0.
    const auto link =
        *reinterpret_cast<volatile const uintptr_t *>(managed_stack + sizeof(uintptr_t)) &
        kPacStripMask;
    if (link == 0) return true;
    const auto linked_top = *reinterpret_cast<volatile const uintptr_t *>(link);
    const auto frame = (linked_top & ~uintptr_t{1}) & kPacStripMask;
    if (frame == 0) return true;
    const auto method = *reinterpret_cast<volatile const uintptr_t *>(frame) & kPacStripMask;
    return method != (replacement & kPacStripMask);
}

void OnDoCallEnter(HookContext *ctx, void *) {
    if (!ctx) return;
    ctx->intercept_leave = 0;
    if (hook_managed_reentry_guard_active() != 0) return;

    const auto method = ctx->x[0] & kPacStripMask;
    uint64_t route_mode = 0;
    const auto replacement = hook_art_router_table_lookup_do_call(method, &route_mode) &
                             kPacStripMask;
    if (!replacement) return;

    // Source reference: rustFrida art_controller.rs::on_do_call_enter. Vector's
    // InvokeOriginal calls a cloned ArtMethod, but keep the engine's one-shot
    // bypass for managed helper routes and future API-compatible call paths.
    if (orig_bypass_consume_current_thread(method) != 0) return;
    if (!ShouldReplaceForManagedStack(replacement)) return;

    // malloc-backed native replacement ArtMethods are not visited by GC. Keep
    // their compressed declaring_class_ synchronized before interpreter entry.
    // Managed helper modes (>= 4) own a real dex ArtMethod and must retain it.
    if (route_mode < 4) {
        const auto declaring_class =
            *reinterpret_cast<volatile const uint32_t *>(method);
        *reinterpret_cast<volatile uint32_t *>(replacement) = declaring_class;
    }
    ctx->x[0] = replacement;
}

int GetAndroidApiLevel() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.sdk", value) <= 0) return 0;
    return atoi(value);
}

uint32_t CompileDontBotherFlag() {
    // Source: rustFrida jni_core.rs::k_acc_compile_dont_bother. The bit moved
    // between API 24-26 and API 27+, and is absent on older ART releases.
    const int api = GetAndroidApiLevel();
    if (api >= 27) return 0x02000000u;
    if (api >= 24) return 0x01000000u;
    return 0;
}

std::string GetAndroidCodename() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.codename", value) <= 0) return {};
    return value;
}

void DiscoverClassLinkerTrampolines(JNIEnv *env) {
    // Source reference: rustFrida art_method/bridge.rs::find_classlinker_trampolines,
    // art_method/runtime.rs::find_runtime_java_vm and shared.rs::
    // compute_classlinker_candidates. art_quick_* entries are commonly LOCAL/HIDDEN;
    // keep this discovery layout-validated and fail closed when an anchor is absent.
    std::call_once(g_classlinker_discovery_once, [env] {
        if (!env) return;

        auto direct = [](const char *name) {
            return reinterpret_cast<uintptr_t>(ResolveArtSymbol(name)) & kPacStripMask;
        };
        const auto direct_jni = direct("art_quick_generic_jni_trampoline");
        const auto direct_interpreter = direct("art_quick_to_interpreter_bridge");
        const auto direct_resolution = direct("art_quick_resolution_trampoline");
        const auto direct_imt = direct("art_quick_imt_conflict_trampoline");
        const bool direct_jni_valid = IsMappedExecutable(direct_jni) && IsInArtModule(direct_jni);
        const bool direct_interpreter_valid =
            IsMappedExecutable(direct_interpreter) && IsInArtModule(direct_interpreter);
        const bool direct_resolution_valid =
            IsMappedExecutable(direct_resolution) && IsInArtModule(direct_resolution);
        const bool direct_imt_valid = IsMappedExecutable(direct_imt) && IsInArtModule(direct_imt);
        // Source: rustFrida art_method/bridge.rs::find_classlinker_trampolines
        // returns usable direct symbols even when only a subset is visible.
        g_classlinker_trampolines.generic_jni = direct_jni_valid ? direct_jni : 0;
        g_classlinker_trampolines.interpreter = direct_interpreter_valid ? direct_interpreter : 0;
        g_classlinker_trampolines.resolution = direct_resolution_valid ? direct_resolution : 0;
        g_classlinker_trampolines.imt_conflict = direct_imt_valid ? direct_imt : 0;
        if (direct_jni_valid && direct_interpreter_valid && direct_resolution_valid) {
            LOGI("WxShadow ART ClassLinker trampolines ready from symbols: jni={} interp={} "
                 "resolution={} imt={}",
                 reinterpret_cast<void *>(direct_jni),
                 reinterpret_cast<void *>(direct_interpreter),
                 reinterpret_cast<void *>(direct_resolution),
                 reinterpret_cast<void *>(g_classlinker_trampolines.imt_conflict));
            return;
        }

        JavaVM *vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || !vm) {
            LOGW("WxShadow ART ClassLinker discovery: JavaVM unavailable");
            return;
        }
        uintptr_t runtime = 0;
        if (!ResolveRuntime(env, &runtime)) {
            LOGW("WxShadow ART ClassLinker discovery: Runtime unavailable");
            return;
        }

        const auto vm_address = reinterpret_cast<uintptr_t>(vm) & kPacStripMask;
        size_t java_vm_offset = 0;
        for (size_t offset = 384; offset < 384 + 800; offset += sizeof(uintptr_t)) {
            if (!IsReadable(runtime + offset, sizeof(uintptr_t))) continue;
            const auto candidate = *reinterpret_cast<const uintptr_t *>(runtime + offset) &
                                   kPacStripMask;
            if (candidate == vm_address) {
                java_vm_offset = offset;
                break;
            }
        }
        if (java_vm_offset == 0) {
            LOGW("WxShadow ART ClassLinker discovery: Runtime java_vm_ anchor not found");
            return;
        }

        const int api_level = GetAndroidApiLevel();
        const auto codename = GetAndroidCodename();
        std::vector<size_t> class_linker_offsets;
        auto add_offset = [&class_linker_offsets](size_t distance) {
            if (distance > 0 && distance <= 384) {
                class_linker_offsets.push_back(distance);
            }
        };
        if (api_level >= 33 || codename == "Tiramisu") {
            add_offset(4 * sizeof(uintptr_t));
        } else if (api_level >= 30 || codename == "R" || codename == "S") {
            add_offset(3 * sizeof(uintptr_t));
            add_offset(4 * sizeof(uintptr_t));
        } else if (api_level >= 29 || codename == "Q") {
            add_offset(2 * sizeof(uintptr_t));
        } else if (api_level >= 27) {
            add_offset(6 * sizeof(uintptr_t));
        } else {
            add_offset(5 * sizeof(uintptr_t));
        }

        const size_t delta = (api_level >= 30 || codename == "R" || codename == "S") ?
                                 6 : ((api_level >= 29 || codename == "Q") ? 4 :
                                      (api_level >= 23 ? 3 : 5));
        const size_t resolution_delta = api_level >= 23 ? 2 : 3;
        for (const auto distance : class_linker_offsets) {
            if (java_vm_offset <= distance || java_vm_offset - distance < sizeof(uintptr_t)) {
                continue;
            }
            const auto class_linker_offset = java_vm_offset - distance;
            const auto intern_table_offset = class_linker_offset - sizeof(uintptr_t);
            if (!IsReadable(runtime + class_linker_offset, sizeof(uintptr_t)) ||
                !IsReadable(runtime + intern_table_offset, sizeof(uintptr_t))) {
                continue;
            }
            const auto class_linker =
                *reinterpret_cast<const uintptr_t *>(runtime + class_linker_offset) &
                kPacStripMask;
            const auto intern_table =
                *reinterpret_cast<const uintptr_t *>(runtime + intern_table_offset) &
                kPacStripMask;
            if (!class_linker || !intern_table || !IsReadable(class_linker + 200, 800)) {
                continue;
            }

            size_t intern_table_class_linker_offset = 0;
            for (size_t offset = 200; offset < 200 + 800; offset += sizeof(uintptr_t)) {
                const auto candidate =
                    *reinterpret_cast<const uintptr_t *>(class_linker + offset) &
                    kPacStripMask;
                if (candidate == intern_table) {
                    intern_table_class_linker_offset = offset;
                    break;
                }
            }
            if (intern_table_class_linker_offset < delta * sizeof(uintptr_t)) continue;

            const auto jni_offset = intern_table_class_linker_offset +
                                    delta * sizeof(uintptr_t);
            const auto interp_offset = jni_offset + sizeof(uintptr_t);
            const auto imt_offset = jni_offset - sizeof(uintptr_t);
            if (jni_offset < resolution_delta * sizeof(uintptr_t)) continue;
            const auto resolution_offset =
                jni_offset - resolution_delta * sizeof(uintptr_t);
            if (!IsReadable(class_linker + resolution_offset, sizeof(uintptr_t)) ||
                !IsReadable(class_linker + imt_offset, sizeof(uintptr_t)) ||
                !IsReadable(class_linker + jni_offset, sizeof(uintptr_t)) ||
                !IsReadable(class_linker + interp_offset, sizeof(uintptr_t))) {
                continue;
            }

            const auto generic_jni =
                *reinterpret_cast<const uintptr_t *>(class_linker + jni_offset) &
                kPacStripMask;
            const auto interpreter =
                *reinterpret_cast<const uintptr_t *>(class_linker + interp_offset) &
                kPacStripMask;
            const auto resolution =
                *reinterpret_cast<const uintptr_t *>(class_linker + resolution_offset) &
                kPacStripMask;
            const auto imt_conflict =
                *reinterpret_cast<const uintptr_t *>(class_linker + imt_offset) &
                kPacStripMask;
            if (!generic_jni || !IsMappedExecutable(generic_jni) ||
                !IsInArtModule(generic_jni)) {
                continue;
            }

            // Keep direct symbols when present and use the validated scan to
            // fill only entries hidden from symbol lookup, matching source
            // find_classlinker_trampolines' per-field merge.
            if (!direct_jni_valid) g_classlinker_trampolines.generic_jni = generic_jni;
            if (!direct_interpreter_valid) {
                g_classlinker_trampolines.interpreter =
                    IsMappedExecutable(interpreter) && IsInArtModule(interpreter) ?
                        interpreter : 0;
            }
            if (!direct_resolution_valid) {
                g_classlinker_trampolines.resolution =
                    IsMappedExecutable(resolution) && IsInArtModule(resolution) ?
                        resolution : 0;
            }
            if (!direct_imt_valid) {
                g_classlinker_trampolines.imt_conflict =
                    IsMappedExecutable(imt_conflict) && IsInArtModule(imt_conflict) ?
                        imt_conflict : 0;
            }
            LOGI("WxShadow ART ClassLinker trampolines ready: api={} java_vm=+{} "
                 "class_linker=+{} intern_table=+{} jni={} interp={} resolution={} imt={}",
                 api_level, java_vm_offset, class_linker_offset,
                 intern_table_class_linker_offset, reinterpret_cast<void *>(generic_jni),
                 reinterpret_cast<void *>(g_classlinker_trampolines.interpreter),
                 reinterpret_cast<void *>(g_classlinker_trampolines.resolution),
                 reinterpret_cast<void *>(g_classlinker_trampolines.imt_conflict));
            return;
        }
        LOGW("WxShadow ART ClassLinker discovery: no validated candidate");
    });
}

size_t InitializeDoCallLayer() {
    // Source reference: rustFrida art_method/bridge.rs::find_do_call_symbols and
    // art_controller.rs Layer 2. DoCall is attached once per template instance;
    // misses tail-jump directly to the original trampoline.
    const int api_level = GetAndroidApiLevel();
    std::vector<std::string> symbols;
    // Source: rustFrida art_method/bridge.rs::find_do_call_symbols treats an
    // unavailable API property (0) like the legacy <=22 signature.
    if (api_level <= 22) {
        for (const char *b0 : {"0", "1"}) {
            for (const char *b1 : {"0", "1"}) {
                symbols.emplace_back(
                    "_ZN3art11interpreter6DoCallILb" + std::string(b0) +
                    "ELb" + b1 +
                    "EEEbPNS_6mirror9ArtMethodEPNS_6ThreadERNS_11ShadowFrameEPKNS_11InstructionEtPNS_6JValueE");
            }
        }
    } else if (api_level <= 33) {
        for (const char *b0 : {"0", "1"}) {
            for (const char *b1 : {"0", "1"}) {
                symbols.emplace_back(
                    "_ZN3art11interpreter6DoCallILb" + std::string(b0) +
                    "ELb" + b1 +
                    "EEEbPNS_9ArtMethodEPNS_6ThreadERNS_11ShadowFrameEPKNS_11InstructionEtPNS_6JValueE");
            }
        }
    } else {
        for (const char *b0 : {"0", "1"}) {
            symbols.emplace_back(
                "_ZN3art11interpreter6DoCallILb" + std::string(b0) +
                "EEEbPNS_9ArtMethodEPNS_6ThreadERNS_11ShadowFrameEPKNS_11InstructionEtbPNS_6JValueE");
        }
    }

    for (size_t i = 0; i < symbols.size() &&
                       g_do_call_target_count < g_do_call_targets.size(); ++i) {
        const auto source = reinterpret_cast<uintptr_t>(
            ResolveArtSymbol(symbols[i].c_str())) & kPacStripMask;
        if (!source) continue;
        void *resolved = resolve_art_trampoline(reinterpret_cast<void *>(source), nullptr);
        if (!resolved) resolved = reinterpret_cast<void *>(source);
        const auto target = reinterpret_cast<uintptr_t>(resolved) & kPacStripMask;
        if (!IsMappedExecutable(target) || !IsInArtModule(target)) {
            LOGW("WxShadow ART controller: rejected DoCall[{}] target {}", i,
                 reinterpret_cast<void *>(target));
            continue;
        }
        if (std::find(g_do_call_targets.begin(),
                      g_do_call_targets.begin() + g_do_call_target_count,
                      target) != g_do_call_targets.begin() + g_do_call_target_count) {
            continue;
        }
        const int result = hook_attach(reinterpret_cast<void *>(target), OnDoCallEnter,
                                       nullptr, reinterpret_cast<void *>(i & 1),
                                       ArtStealthFlag());
        if (result != HOOK_OK) {
            LOGW("WxShadow ART controller: DoCall[{}] hook failed target={} result={}",
                 i, reinterpret_cast<void *>(target), result);
            continue;
        }
        g_do_call_targets[g_do_call_target_count++] = target;
        LOGI("WxShadow ART controller: DoCall[{}] ready source={} target={}", i,
             reinterpret_cast<void *>(source), reinterpret_cast<void *>(target));
    }
    return g_do_call_target_count;
}

void RegisterActiveState(MethodState *state) {
    if (!state) return;
    std::lock_guard lock(g_state_mutex);
    if (std::find(g_active_states.begin(), g_active_states.end(), state) ==
        g_active_states.end()) {
        g_active_states.push_back(state);
    }
}

void UnregisterActiveState(MethodState *state) {
    std::lock_guard lock(g_state_mutex);
    g_active_states.erase(
        std::remove(g_active_states.begin(), g_active_states.end(), state),
        g_active_states.end());
}

void SynchronizeReplacementMethods(const char *phase) {
    // Source reference: rustFrida art_controller.rs::synchronize_replacement_methods.
    // Vector's registry owns MethodState for the process lifetime after a
    // successful activation. Keep this callback free of /proc/self/maps scans:
    // it runs from ART GC/fixup paths where blocking the collector blocks all
    // threads waiting for GC completion.
    bool expected = false;
    if (!g_gc_sync_in_progress.compare_exchange_strong(
            expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        if (ArtPerfEnabled()) {
            LOGI("[perf] gc_sync phase={} skipped=reentrant", phase ? phase : "unknown");
        }
        return;
    }

    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    size_t active_count = 0;
    size_t synced_count = 0;
    {
        // GC callbacks run while ART holds ClassLinker/collector coordination
        // locks. Never make the collector wait for an install/uninstall writer;
        // a later GC/fixup callback will retry the declaring-class sync.
        std::unique_lock lock(g_state_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            if (ArtPerfEnabled()) {
                LOGI("[perf] gc_sync phase={} skipped=state_mutex_busy",
                     phase ? phase : "unknown");
            }
            g_gc_sync_in_progress.store(false, std::memory_order_release);
            return;
        }
        active_count = g_active_states.size();
        for (auto *state : g_active_states) {
            if (!state || state->native_entry_hook || !state->replacement ||
                !state->original_art_method) {
                continue;
            }
            const auto declaring_class = *reinterpret_cast<volatile const uint32_t *>(
                state->original_art_method);
            *reinterpret_cast<volatile uint32_t *>(state->replacement) = declaring_class;
            ++synced_count;
        }
    }
    g_gc_sync_in_progress.store(false, std::memory_order_release);

    if (perf_start != 0) {
        LOGI("[perf] gc_sync phase={} active={} synced={} elapsedUs={}",
             phase ? phase : "unknown", active_count, synced_count,
             ArtPerfElapsedUs(perf_start));
    }
}

void OnGcSyncLeave(HookContext *, void *) {
    SynchronizeReplacementMethods("leave");
}

void OnGcSyncEnter(HookContext *, void *) {
    SynchronizeReplacementMethods("enter");
}

bool IsReplacementMethod(uintptr_t method) {
    method &= kPacStripMask;
    if (!method) return false;
    // OAT header queries are also made from ART hot paths. Returning a
    // conservative miss on contention avoids stalling ClassLinker while a
    // replacement is being published; the next query observes the registry.
    std::unique_lock lock(g_state_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    return std::any_of(g_active_states.begin(), g_active_states.end(),
                       [method](const MethodState *state) {
                           return state && !state->native_entry_hook &&
                                  (state->replacement & kPacStripMask) == method;
                       });
}

// ---------------------------------------------------------------------------
// App-process OAT lookup diagnostic
//
// Measured: our own thunk's [sp+0] always holds a valid ArtMethod, so the bogus
// method the walker dies on comes from a different frame. Instead of sampling at
// hook entry and hoping to catch that moment, listen where ART actually asks:
// every stack walk ends up in GetOatQuickMethodHeader(method, lr), and the method
// it passes is the frame's method slot.
//
// A method pointer that lands inside the calling thread's own stack is never
// legitimate, and it is exactly the tombstone_10 shape (PrettyMethod on
// 0x77ff0a8000, a page-aligned stack address). Only that case is logged, so the
// hot path stays a couple of compares.
// ---------------------------------------------------------------------------
bool g_oat_header_diagnostic_only = false;

struct ThreadStackBounds {
    uintptr_t lo;
    uintptr_t hi;
    bool valid;
};

const ThreadStackBounds &CurrentThreadStackBounds() {
    // One pthread_getattr_np per thread, cached in TLS. This runs on the GC
    // thread's walk path, so it must not repeat per call.
    static thread_local ThreadStackBounds bounds{0, 0, false};
    if (!bounds.valid) {
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void *base = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &base, &size) == 0 && base && size) {
                bounds.lo = reinterpret_cast<uintptr_t>(base);
                bounds.hi = bounds.lo + size;
                bounds.valid = true;
            }
            pthread_attr_destroy(&attr);
        }
    }
    return bounds;
}

void MaybeLogStackMethodOatLookup(uintptr_t method, uintptr_t lr) {
    if (!method) return;
    const auto &bounds = CurrentThreadStackBounds();
    if (!bounds.valid) return;
    const uintptr_t m = method & kPacStripMask;
    if (m < bounds.lo || m >= bounds.hi) return;   // not a stack address

    static std::atomic<uint32_t> reported{0};
    const uint32_t n = reported.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16) return;

    // The walker's own view of the current frame, recorded alongside so the bad
    // frame can be located in the chain without a second build.
    uintptr_t top = 0;
    uintptr_t thread = 0;
#if defined(__aarch64__)
    uintptr_t tls = 0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls));
    if (tls) {
        thread = *reinterpret_cast<volatile const uintptr_t *>(tls + 7 * sizeof(uintptr_t)) &
                 kPacStripMask;
    }
#endif
    if (thread && g_thread_layout.valid) {
        top = *reinterpret_cast<volatile const uintptr_t *>(
            thread + g_thread_layout.managed_stack_offset);
    }

    LOGW("WxShadow ART OAT lookup got a STACK address as the method: method={} lr={} "
         "stack={}..{} top_quick_frame={} thread={} tid={} n={}",
         reinterpret_cast<void *>(m), reinterpret_cast<void *>(lr),
         reinterpret_cast<void *>(bounds.lo), reinterpret_cast<void *>(bounds.hi),
         reinterpret_cast<void *>(top), reinterpret_cast<void *>(thread),
         static_cast<long>(gettid()), n);
}

// A dex method index that large cannot belong to any real dex file. ART reaches
// this state when it treats something that is not an ArtMethod as one: it reads
// access_flags_ at +4 (finding "not native") and dex_method_index_ at +8
// (finding garbage), then FindOatMethodFor aborts with
//   Check failed: found_virtual Didn't find oat method index for virtual method
//   <<invalid-method-idx-NNN>>
// Observed values: 351803424, 349474360, 345416752. Valid indices in these apps
// are in the low hundreds of thousands.
constexpr uint32_t kImplausibleDexMethodIndex = 0x01000000u;

// Readable mappings cached ONCE per process.
//
// A fixed address window cannot work here: this device maps ART and its
// ArtMethods around 0x70xxxxxx while another boot put the same process at
// 0x77ff... - and an earlier revision used [0x7000000000, 0x8000000000), which
// silently rejected every real method (0x7025ba98 < 0x7000000000) and made this
// diagnostic report nothing at all. Validate against the process's own maps
// instead, parsed once so the GC walk path never touches /proc.
//
// SIZE MATTERS, and getting it wrong fails SILENTLY in the worst possible way.
// /proc/self/maps is in ascending address order, so a cap that is too small does
// not drop an arbitrary sample -- it drops the HIGH end, which is exactly where
// the interesting objects live (ART heap 0x7f..., thread stacks 0x7ff...).
// Measured on the hunter process at build 3285: 3847 VMAs total, 3193 readable.
// With the old cap of 1024 the cache ended at 0x77e1e75000, so IsCachedReadable()
// answered "no" for every heap object and every stack address. That made the
// frame probe's classification always report 0x0, and made the WalkStack
// forensic probe return at its very first check without ever logging. Two
// separate instruments produced confident-looking nothing because of this.
constexpr size_t kCachedRangeMax = 8192;
// Every range also carries a short label, because a forensic log full of raw
// addresses is unusable: "0x72969a3c" says nothing, "0x72969a3c[boot.oat]" says
// which mapping ART was pointing into. Labels are interned (maps repeats the same
// path for many VMAs), so the cost is one small table rather than a path per VMA.
constexpr size_t kMappedNameLen = 40;
constexpr size_t kMappedNameMax = 96;
struct MappedRange {
    uintptr_t lo;
    uintptr_t hi;
    uint16_t name_idx;   // index into g_mapped_names, 0 = "[anon]"
};
MappedRange g_cached_ranges[kCachedRangeMax];
size_t g_cached_range_count = 0;
char g_mapped_names[kMappedNameMax][kMappedNameLen];
size_t g_mapped_name_count = 0;
// Set when maps had more readable ranges than the cache could hold: every query
// above the last stored range is then a false negative, so say so out loud.
std::atomic<bool> g_cached_ranges_truncated{false};
std::once_flag g_cached_ranges_once;

// Short, stable label for a maps path. "/apex/com.android.art/lib64/libart.so"
// becomes "libart.so"; "anon:dalvik-/apex/.../boot.art" becomes "boot.art";
// "[anon:libc_malloc]" becomes "libc_malloc"; a bare mapping becomes "[anon]".
size_t InternMappedName(const char *raw) {
    char label[kMappedNameLen] = {};
    if (raw == nullptr || raw[0] == '\0') {
        // unnamed mapping
    } else if (strcmp(raw, "[stack]") == 0 || strstr(raw, "[stack]")) {
        snprintf(label, sizeof(label), "stack");
    } else {
        const char *path = raw;
        if (const char *a = strstr(raw, "anon:")) path = a + 5;
        if (const char *slash = strrchr(path, '/')) path = slash + 1;
        if (path[0] == '[') {
            snprintf(label, sizeof(label), "%s", path);
            if (char *bracket = strchr(label, ']')) *bracket = '\0';
        } else {
            snprintf(label, sizeof(label), "%s", path);
        }
    }
    for (size_t i = 0; i < g_mapped_name_count; ++i) {
        if (strcmp(g_mapped_names[i], label) == 0) return i;
    }
    if (g_mapped_name_count >= kMappedNameMax) return 0;
    snprintf(g_mapped_names[g_mapped_name_count], kMappedNameLen, "%s", label);
    return g_mapped_name_count++;
}

void LoadReadableRanges() {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    char line[512];
    size_t n = 0;
    bool truncated = false;
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char perms[5] = {};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (n >= kCachedRangeMax) {
            truncated = true;
            break;
        }
        // Sixth field onwards is the path/name; it may be absent.
        char *path = strchr(line, '/');
        char *anon = strstr(line, "[anon");
        char *name = path ? path : anon;
        if (name) {
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
        }
        g_cached_ranges[n].lo = start;
        g_cached_ranges[n].hi = end;
        g_cached_ranges[n].name_idx = (uint16_t)InternMappedName(name);
        ++n;
    }
    fclose(maps);
    g_cached_range_count = n;
    g_cached_ranges_truncated.store(truncated, std::memory_order_release);
    if (truncated) {
        // Above this address every readability answer is a false negative.
        LOGW("WxShadow ART readable-range cache TRUNCATED: kept={} last_end={} -- queries "
             "above this address will be reported unreadable (raise kCachedRangeMax)",
             n, reinterpret_cast<void *>(n ? g_cached_ranges[n - 1].hi : 0));
    } else {
        LOGI("WxShadow ART readable-range cache ready: ranges={}", n);
    }
}

// libart's own range, resolved once. IsInArtModule() calls FindModuleRange() which
// fopen()s /proc/self/maps, so it cannot be used from the stack-walk probe or from
// a signal handler. This cached form is safe in both places.
struct CachedArtRange {
    uintptr_t lo;
    uintptr_t hi;
    bool valid;
};

const CachedArtRange &CachedArtModuleRange() {
    static CachedArtRange range{0, 0, false};
    static std::once_flag once;
    std::call_once(once, [] {
        uintptr_t lo = 0;
        uintptr_t hi = 0;
        if (FindModuleRange("libart.so", &lo, &hi)) {
            range.lo = lo;
            range.hi = hi;
            range.valid = true;
        }
    });
    return range;
}


// Readability answer with the reason kept separate from the verdict.
//
// A bare `false` conflates two very different facts:
//   * the address really is not in a readable mapping, and
//   * the cache simply does not cover it (truncation, or a mapping created after
//     the cache was built -- thread stacks appear later, and so does anything
//     mmap'd during startup).
// Earlier revisions returned `false` for both, which is how two instruments came
// to report confident-looking negatives. Callers that only want "can I safely
// dereference" use IsCachedReadable(); the forensic paths use this so a log line
// can say "out of cache" instead of implying a real finding.
enum class ReadableVerdict { Yes, No, OutOfCache };

ReadableVerdict ProbeReadability(uintptr_t address) {
    std::call_once(g_cached_ranges_once, LoadReadableRanges);
    const uintptr_t addr = address & kPacStripMask;
    const size_t count = g_cached_range_count;
    if (count == 0) return ReadableVerdict::OutOfCache;
    // Ranges from /proc/self/maps are in ascending order, and this sits on
    // GetOatQuickMethodHeader which ART calls on the order of 100k/s, so a
    // linear scan would cost far too much.
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (addr < g_cached_ranges[mid].lo) {
            hi = mid;
        } else if (addr >= g_cached_ranges[mid].hi) {
            lo = mid + 1;
        } else {
            return ReadableVerdict::Yes;
        }
    }
    // Past the last cached range: the cache cannot speak for this address.
    if (addr >= g_cached_ranges[count - 1].hi) return ReadableVerdict::OutOfCache;
    return ReadableVerdict::No;
}

bool IsCachedReadable(uintptr_t address) {
    return ProbeReadability(address) == ReadableVerdict::Yes;
}

// Label of the mapping that owns `address`, or "" when unmapped / not cached.
// Used by forensic logs so an address can be read as "which mapping" without a
// second pass over /proc.
bool SafeReadU64(uintptr_t address, uintptr_t *out) {
#if defined(__aarch64__)
    if (out == nullptr) return false;
    const uintptr_t addr = address & kPacStripMask;
    if (addr == 0) return false;
    uint64_t value = 0;
    struct iovec local = {&value, sizeof(value)};
    // iovec::iov_base is void*; process_vm_readv only reads through it.
    struct iovec remote = {reinterpret_cast<void *>(addr), sizeof(value)};
    const ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n != static_cast<ssize_t>(sizeof(value))) return false;
    *out = static_cast<uintptr_t>(value);
    return true;
#else
    (void)address;
    (void)out;
    return false;
#endif
}

const char *MappedNameFor(uintptr_t address) {
    std::call_once(g_cached_ranges_once, LoadReadableRanges);
    const uintptr_t addr = address & kPacStripMask;
    size_t lo = 0;
    size_t hi = g_cached_range_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (addr < g_cached_ranges[mid].lo) {
            hi = mid;
        } else if (addr >= g_cached_ranges[mid].hi) {
            lo = mid + 1;
        } else {
            const uint16_t idx = g_cached_ranges[mid].name_idx;
            return idx < g_mapped_name_count ? g_mapped_names[idx] : "";
        }
    }
    return "";
}

void MaybeLogImplausibleMethodIndex(uintptr_t method, uintptr_t lr) {
    if (!method) return;
    const uintptr_t m = method & kPacStripMask;
    // Needs the method header to be readable; ask the cached maps rather than
    // assuming an address range.
    if (!IsCachedReadable(m + 12)) return;
    const auto flags = *reinterpret_cast<volatile const uint32_t *>(m + 4);
    const auto index = *reinterpret_cast<volatile const uint32_t *>(m + 8);

    // Two distinct ways a non-ArtMethod reaches the OAT lookup, both observed:
    //   index: ART read a garbage dex_method_index_ and aborted in FindOatMethodFor
    //          (<<invalid-method-idx-NNN>>, seen 351803424 / 349474360 / 345416752)
    //   entry: ART read entry_ == 0 and aborted in CHECK(existing_entry_point)
    uintptr_t entry = 0;
    // entry_offset is only meaningful once the layout probe has run; method_size 0
    // means it has not, and reading a wrong offset would report a false finding.
    if (g_layout.method_size != 0 &&
        g_layout.entry_offset + sizeof(uintptr_t) <= g_layout.method_size) {
        entry = *reinterpret_cast<volatile const uintptr_t *>(m + g_layout.entry_offset);
    }
    const bool bad_index = index >= kImplausibleDexMethodIndex;
    const bool null_entry = (entry == 0);
    if (!bad_index && !null_entry) return;

    static std::atomic<uint32_t> reported{0};
    const uint32_t n = reported.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16) return;

    uintptr_t thread = 0;
#if defined(__aarch64__)
    uintptr_t tls = 0;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls));
    if (tls) {
        thread = *reinterpret_cast<volatile const uintptr_t *>(tls + 7 * sizeof(uintptr_t)) &
                 kPacStripMask;
    }
#endif
    // Distinguish "the frame held art::Thread*" (seen before: tid at +0xC,
    // jni_env at +0xD0) from "the frame held some other non-method value".
    const bool looks_like_thread = thread && (method & kPacStripMask) == thread;
    LOGW("WxShadow ART OAT lookup got a NON-ArtMethod: method={} flags=0x{:x} idx={} "
         "entry={} bad_index={} null_entry={} is_thread={} lr={} thread={} tid={} n={}",
         reinterpret_cast<void *>(method), flags, index, reinterpret_cast<void *>(entry),
         bad_index ? "yes" : "no", null_entry ? "yes" : "no",
         looks_like_thread ? "yes" : "no", reinterpret_cast<void *>(lr),
         reinterpret_cast<void *>(thread), static_cast<long>(gettid()), n);
}

void OnGetOatQuickMethodHeader(HookContext *ctx, void *) {
    if (!ctx) return;
    const auto method = ctx->x[0] & kPacStripMask;
    const auto pc = ctx->x[1] & kPacStripMask;

    MaybeLogStackMethodOatLookup(method, pc);
    MaybeLogImplausibleMethodIndex(method, pc);

    // Source reference: rustFrida art_controller.rs::on_get_oat_quick_method_header.
    // Hook-pool PCs do not belong to ART-owned quick code, and malloc-backed
    // replacement ArtMethods have no valid OAT header.
    //
    // 诊断（限频）：GC 崩溃现场是 VisitFrame 里 `ldr x13,[x11,...]`（x11 = 非 NULL 垃圾），
    // 而伪 codeinfo 下 stack_maps_ 应全零（基址为 NULL，guard 能救）。因此那一帧拿到的
    // codeinfo 很可能不是我们那 16 字节伪前缀。这里把每次「返回 NULL」的判定细节打出来，
    // 用来对照崩溃时刻。
    const bool in_pool = hook_is_in_exec_pool(pc) != 0;
    const bool replacement = IsReplacementMethod(method);
    // 诊断默认关闭：GetOatQuickMethodHeader 在 system_server 里是 10 万次/秒级的
    // 热路径（实测开机 20s 内 214 万次），在这里写 logcat 本身就会拖慢 system_server
    // 主线程（实测 Slow dispatch 1757ms → 输入分发超时 ANR）。需要时用
    // VECTOR_ART_OAT_TRACE=1 打开。
    static const bool oat_trace = [] {
        const char *env = getenv("VECTOR_ART_OAT_TRACE");
        return env != nullptr && env[0] != '0';
    }();
    if (oat_trace && (in_pool || replacement)) {
        static std::atomic<uint32_t> g_oat_null_count{0};
        const uint32_t n = g_oat_null_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 4096) == 0) {
            LOGI("[oat-header] NULL n={} method={} pc={} in_pool={} replacement={}", n,
                 reinterpret_cast<void *>(method), reinterpret_cast<void *>(pc), in_pool,
                 replacement);
        }
    }
    if (in_pool || replacement) {
        // system_server relies on this NULL to keep malloc-backed replacement
        // methods out of the OAT lookup. App processes install this hook for
        // diagnostics only, so they must keep the unhooked behaviour: fall
        // through and call the real function instead of inventing a NULL.
        if (!g_oat_header_diagnostic_only) {
            ctx->x[0] = 0;
            return;
        }
    }
    if (oat_trace) {
        // 透传（真正调 ART）也限频记录，用于发现「本该判为我们的帧却走了透传」。
        static std::atomic<uint32_t> g_oat_pass_count{0};
        const uint32_t n = g_oat_pass_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 16384) == 0) {
            LOGI("[oat-header] pass n={} method={} pc={}", n,
                 reinterpret_cast<void *>(method), reinterpret_cast<void *>(pc));
        }
    }
    if (ctx->trampoline) {
        ctx->x[0] = hook_invoke_trampoline(ctx, ctx->trampoline);
    } else {
        ctx->x[0] = 0;
    }
}

bool InitializeOatHeaderLayer() {
    // system_server-only (caller gates with g_is_system_process). This hook starts every
    // oat-method code path (GetOatQuickMethodHeader), so under WxShadow its code page
    // (code+literal-pool in same page) flips between --x and r-- forever during load
    // (kernel fault storm: dmesg exec/read fault same-page hundreds of times) and blinds
    // ActivityTaskManager for seconds -> stalls every later app launch (xff 36s delay).
    // It exists only for the system_server replacement lifecycle, NOT as an anti-tamper
    // target, so force non-stealth mprotect-RX here; app per-method/ interp hooks stay WxShadow.
    ScopedStealthOverride scoped_oat_normal(0);
    constexpr const char *kCandidates[] = {
        "_ZN3art9ArtMethod23GetOatQuickMethodHeaderEm",
        "_ZN3art9ArtMethod23GetOatQuickMethodHeaderEj",
    };
    for (const auto *name : kCandidates) {
        const auto target = reinterpret_cast<uintptr_t>(ResolveArtSymbol(name)) &
                            kPacStripMask;
        if (!target || !IsMappedExecutable(target) || !IsInArtModule(target)) continue;
        void *trampoline = hook_replace(reinterpret_cast<void *>(target),
                                        OnGetOatQuickMethodHeader, nullptr,
                                        ArtStealthFlag());
        if (!trampoline) {
            LOGW("WxShadow ART GetOatQuickMethodHeader hook failed: {}", name);
            return false;
        }
        g_oat_header_hook_target = target;
        LOGI("WxShadow ART GetOatQuickMethodHeader ready target={} trampoline={}",
             reinterpret_cast<void *>(target), trampoline);
        return true;
    }
    LOGW("WxShadow ART GetOatQuickMethodHeader unavailable");
    return false;
}

size_t InitializeGcSyncLayer() {
    // Application startup can synchronously query ART heap/class metadata
    // (Launch's environment detector is one example). Installing the
    // CopyingPhase/CollectGarbageInternal/RunFlipFunction hooks in every app
    // process makes those queries contend with GC coordination locks and can
    // turn a normal collection into a multi-second main-thread stall.
    // rustFrida needs these hooks for the system_server replacement lifecycle;
    // app processes use the stable per-method router instead.
    if (!g_is_system_process) {
        LOGI("WxShadow ART GC sync skipped for app process");
        return 0;
    }

    // system_server-only. These GC coordination hooks (CopyingPhase/CollectGarbageInternal/
    // RunFlipFunction) contend with GC locks and, under WxShadow, their code pages with embedded
    // literal pools flip between --x and r-- forever (kernel fault storm) soaking system_server CPU
    // (manager/xff startup stalls). They back the system_server replacement lifecycle, NOT anti-tamper
    // targets, so force non-stealth mprotect-RX here; app per-method/ interp hooks stay WxShadow.
    ScopedStealthOverride scoped_gc_normal(0);

    // Source reference: rustFrida art_method/bridge.rs discovery and
    // art_controller.rs GC hook installation. Signatures vary by API, so try
    // the same candidate symbols and attach generic enter/leave callbacks.
    constexpr const char *kCopyingCandidates[] = {
        "_ZN3art2gc9collector17ConcurrentCopying12CopyingPhaseEv",
        "_ZN3art2gc9collector17ConcurrentCopying12MarkingPhaseEv",
    };
    constexpr const char *kCollectCandidates[] = {
        "_ZN3art2gc4Heap22CollectGarbageInternalENS0_9collector6GcTypeENS0_7GcCauseEbj",
        "_ZN3art2gc4Heap22CollectGarbageInternalENS0_9collector6GcTypeENS0_7GcCauseEb",
        "_ZN3art2gc4Heap22CollectGarbageInternalENS0_9collector6GcTypeENS0_7GcCauseEbPKNS0_9collector14GarbageCollectorE",
        "_ZN3art2gc4Heap22CollectGarbageInternalENS0_13GcCauseEb",
    };
    constexpr const char *kFlipCandidates[] = {
        "_ZN3art6Thread15RunFlipFunctionEPS0_b",
        "_ZN3art6Thread15RunFlipFunctionEPS0_",
    };
    std::vector<uintptr_t> targets;
    auto add_first = [&targets](const auto &candidates) {
        for (const auto *name : candidates) {
            const auto address = reinterpret_cast<uintptr_t>(ResolveArtSymbol(name)) &
                                 kPacStripMask;
            if (!address || !IsMappedExecutable(address) || !IsInArtModule(address)) continue;
            if (std::find(targets.begin(), targets.end(), address) == targets.end()) {
                targets.push_back(address);
            }
            return address;
        }
        return uintptr_t{0};
    };

    const auto copying = add_first(kCopyingCandidates);
    const auto collecting = add_first(kCollectCandidates);
    const auto flipping = add_first(kFlipCandidates);
    // Match rustFrida art_controller.rs: GC synchronization is installed only
    // at Copying/MarkingPhase and CollectGarbageInternal on-leave. ClassLinker
    // fixup runs while ClassLinker locks are held and is not a GC sync point;
    // doing replacement writes and icache maintenance there can block dex
    // registration and the next GC checkpoint.
    for (const auto address : {copying, collecting}) {
        if (!address || g_gc_hook_target_count >= g_gc_hook_targets.size()) continue;
        if (std::find(g_gc_hook_targets.begin(),
                      g_gc_hook_targets.begin() + g_gc_hook_target_count,
                      address) != g_gc_hook_targets.begin() + g_gc_hook_target_count) {
            continue;
        }
        if (hook_attach(reinterpret_cast<void *>(address), nullptr, OnGcSyncLeave,
                        nullptr, ArtStealthFlag()) == HOOK_OK) {
            g_gc_hook_targets[g_gc_hook_target_count++] = address;
            LOGI("WxShadow ART GC sync leave hook ready: {}",
                 reinterpret_cast<void *>(address));
        }
    }
    if (flipping && g_gc_hook_target_count < g_gc_hook_targets.size() &&
        std::find(g_gc_hook_targets.begin(),
                  g_gc_hook_targets.begin() + g_gc_hook_target_count,
                  flipping) == g_gc_hook_targets.begin() + g_gc_hook_target_count &&
        hook_attach(reinterpret_cast<void *>(flipping), OnGcSyncEnter, nullptr,
                    nullptr, ArtStealthFlag()) == HOOK_OK) {
        g_gc_hook_targets[g_gc_hook_target_count++] = flipping;
        LOGI("WxShadow ART GC sync enter hook ready: {}",
             reinterpret_cast<void *>(flipping));
    }
    return g_gc_hook_target_count;
}

void *FindJniTrampoline(JNIEnv *env, uintptr_t known_native_method) {
    if (void *address = ResolveArtSymbol("art_quick_generic_jni_trampoline")) {
        const auto value = reinterpret_cast<uintptr_t>(address) & kPacStripMask;
        if (IsMappedExecutable(value) && IsInArtModule(value)) return address;
    }
    DiscoverClassLinkerTrampolines(env);
    if (g_classlinker_trampolines.generic_jni != 0) {
        return reinterpret_cast<void *>(g_classlinker_trampolines.generic_jni);
    }
    // Source: rustFrida art_method/bridge.rs::
    // find_jni_trampoline_from_known_native. Once the known native method's
    // data_/entry_ layout is validated, read its exact entry_point_ rather
    // than selecting an arbitrary executable ArtMethod field.
    if (known_native_method != 0 && g_layout.entry_offset != 0 &&
        IsReadable(known_native_method, g_layout.entry_offset + sizeof(uintptr_t))) {
        const auto entry = *reinterpret_cast<const volatile uintptr_t *>(
            known_native_method + g_layout.entry_offset) & kPacStripMask;
        if (IsMappedExecutable(entry) && IsInArtModule(entry)) {
            return reinterpret_cast<void *>(entry);
        }
    }
    return nullptr;
}

struct InstrumentationLayout {
    size_t runtime_offset = 0;
    bool pointer_mode = false;
};

bool IsInArtModule(uintptr_t address) {
    uintptr_t start = 0;
    uintptr_t end = 0;
    return FindModuleRange("libart.so", &start, &end) &&
           address >= start && address < end;
}

bool InitializeSharedNterpRouter(JNIEnv *env) {
    // Source reference: rustFrida bridge.rs::find_nterp_entry_point uses the
    // exported getter, OatQuickMethodHeader::NterpImpl, then ExecuteNterpImpl;
    // art_controller.rs installs Nterp once as a Layer 1 shared router with
    // pre_scan enabled.
    using GetNterpEntryPointFn = uintptr_t (*)();
    auto getter = reinterpret_cast<GetNterpEntryPointFn>(
        ResolveArtSymbol("_ZN3art11interpreter18GetNterpEntryPointEv"));
    uintptr_t source = 0;
    const char *source_kind = nullptr;
    if (getter && IsMappedExecutable(reinterpret_cast<uintptr_t>(getter))) {
        source = getter() & kPacStripMask;
        source_kind = "GetNterpEntryPoint";
    }
    if ((!source || !IsMappedExecutable(source) || !IsInArtModule(source))) {
        const auto header = reinterpret_cast<uintptr_t>(ResolveArtSymbol(
            "_ZN3art20OatQuickMethodHeader9NterpImplE"));
        if (header && IsReadable(header, sizeof(uintptr_t))) {
            const auto candidate = *reinterpret_cast<const uintptr_t *>(header) &
                                   kPacStripMask;
            if (IsMappedExecutable(candidate) && IsInArtModule(candidate)) {
                source = candidate;
                source_kind = "OatQuickMethodHeader::NterpImpl";
            }
        }
    }
    if ((!source || !IsMappedExecutable(source) || !IsInArtModule(source))) {
        const auto execute = reinterpret_cast<uintptr_t>(
            ResolveArtSymbol("ExecuteNterpImpl")) & kPacStripMask;
        if (IsMappedExecutable(execute) && IsInArtModule(execute)) {
            source = execute;
            source_kind = "ExecuteNterpImpl";
        }
    }
    if (!IsMappedExecutable(source) || !IsInArtModule(source)) {
        LOGW("WxShadow ART controller: Nterp entry unavailable/rejected (getter={} source={})",
             reinterpret_cast<void *>(getter), reinterpret_cast<void *>(source));
        return false;
    }

    void *resolved = resolve_art_trampoline(reinterpret_cast<void *>(source), env);
    if (!resolved) resolved = reinterpret_cast<void *>(source);
    const auto target = reinterpret_cast<uintptr_t>(resolved) & kPacStripMask;
    if (!IsMappedExecutable(target) || !IsInArtModule(target)) {
        LOGW("WxShadow ART controller: rejected resolved Nterp target {}",
             reinterpret_cast<void *>(target));
        return false;
    }

    void *hooked_target = nullptr;
    void *trampoline = InstallArtRouterWithFallback(
        reinterpret_cast<void *>(target), static_cast<uint32_t>(g_layout.entry_offset),
        env, &hooked_target,
        1,  // Source already resolved, matching rustFrida prepare_hook_target.
        0,  // Layer 1 has no current-PC hint.
        0,  // Layer 1 shared stubs do not use BLR mode.
        1); // Nterp misses must bypass the full ART-visible router frame.
    if (!trampoline) {
        LOGE("WxShadow ART controller: shared Nterp router installation failed");
        return false;
    }

    g_nterp_router.source = source;
    g_nterp_router.target = reinterpret_cast<uintptr_t>(
        hooked_target ? hooked_target : reinterpret_cast<void *>(target));
    g_nterp_router.trampoline = reinterpret_cast<uintptr_t>(trampoline);
    LOGI("WxShadow ART controller: shared Nterp ready via {} source={} target={} trampoline={} pre_scan=1",
         source_kind ? source_kind : "fallback",
         reinterpret_cast<void *>(g_nterp_router.source),
         reinterpret_cast<void *>(g_nterp_router.target), trampoline);
    return true;
}

bool InitializeSharedNterpWithClinitRouter(JNIEnv *env) {
    // Source reference: rustFrida art_method/bridge.rs::find_nterp_with_clinit_entry_point
    // and art_controller.rs Layer 1. The exported symbol is an OatQuickMethodHeader
    // whose first word is the actual entry point.
    auto header = reinterpret_cast<uintptr_t>(ResolveArtSymbol(
        "_ZN3art20OatQuickMethodHeader19NterpWithClinitImplE"));
    if (!header || !IsReadable(header, sizeof(uintptr_t))) {
        LOGW("WxShadow ART controller: NterpWithClinit header unavailable");
        return false;
    }
    const auto source = (*reinterpret_cast<const uintptr_t *>(header)) & kPacStripMask;
    if (!IsMappedExecutable(source) || !IsInArtModule(source)) {
        LOGW("WxShadow ART controller: rejected NterpWithClinit entry {}",
             reinterpret_cast<void *>(source));
        return false;
    }
    void *resolved = resolve_art_trampoline(reinterpret_cast<void *>(source), env);
    if (!resolved) resolved = reinterpret_cast<void *>(source);
    const auto target = reinterpret_cast<uintptr_t>(resolved) & kPacStripMask;
    if (!IsMappedExecutable(target) || !IsInArtModule(target)) {
        LOGW("WxShadow ART controller: rejected resolved NterpWithClinit target {}",
             reinterpret_cast<void *>(target));
        return false;
    }

    void *hooked_target = nullptr;
    void *trampoline = InstallArtRouterWithFallback(
        reinterpret_cast<void *>(target), static_cast<uint32_t>(g_layout.entry_offset),
        env, &hooked_target, 1, 0, 0, 1);
    if (!trampoline) {
        LOGE("WxShadow ART controller: shared NterpWithClinit router installation failed");
        return false;
    }
    g_nterp_clinit_router.source = source;
    g_nterp_clinit_router.target = reinterpret_cast<uintptr_t>(
        hooked_target ? hooked_target : reinterpret_cast<void *>(target));
    g_nterp_clinit_router.trampoline = reinterpret_cast<uintptr_t>(trampoline);
    LOGI("WxShadow ART controller: shared NterpWithClinit ready source={} target={} trampoline={} pre_scan=1",
         reinterpret_cast<void *>(source),
         reinterpret_cast<void *>(g_nterp_clinit_router.target), trampoline);
    return true;
}

bool IsSharedRouterEntry(uintptr_t entry, const SharedNterpRouter &router) {
    return router.trampoline != 0 &&
           (entry == router.source || entry == router.target);
}

// Source reference: rustFrida
// java_hook_api/install_support.rs::install_per_method_router_hook with
// allow_internal_entry_downgrade=true. The write is deliberately limited to
// the non-native, unclassified shared-entry path. Compiled quick code and
// native/shared-native routes must keep their original ArtMethod entry.
bool DowngradeMethodEntryToInterpreterBridge(MethodState &state) {
    const auto bridge = g_interpreter_bridge_router.source & kPacStripMask;
    if (!bridge || g_interpreter_bridge_router.trampoline == 0 ||
        !state.original_art_method ||
        state.entry_offset + sizeof(uintptr_t) > state.method_size ||
        !IsReadable(state.original_art_method,
                     state.entry_offset + sizeof(uintptr_t))) {
        return false;
    }

    auto *entry = reinterpret_cast<volatile uintptr_t *>(
        state.original_art_method + state.entry_offset);
    const auto current = *entry & kPacStripMask;
    if (current == bridge) {
        return true;
    }

    *entry = bridge;
    hook_flush_cache(reinterpret_cast<void *>(
                         state.original_art_method + state.entry_offset),
                     sizeof(uintptr_t));
    state.original_entry_mutated = true;
    LOGI("WxShadow ART shared entry downgraded: method={} source={} bridge={} "
         "(internal entry only)",
         reinterpret_cast<void *>(state.original_art_method),
         reinterpret_cast<void *>(state.original_entry), reinterpret_cast<void *>(bridge));
    return true;
}

bool InitializeSharedLibartRouter(const char *label, const char *symbol, JNIEnv *env,
                                  SharedNterpRouter *router) {
    if (!label || !symbol || !router) return false;
    auto source = reinterpret_cast<uintptr_t>(ResolveArtSymbol(symbol)) & kPacStripMask;
    if (!source || !IsMappedExecutable(source) || !IsInArtModule(source)) {
        DiscoverClassLinkerTrampolines(env);
        if (strcmp(symbol, "art_quick_generic_jni_trampoline") == 0) {
            source = g_classlinker_trampolines.generic_jni;
        } else if (strcmp(symbol, "art_quick_to_interpreter_bridge") == 0) {
            source = g_classlinker_trampolines.interpreter;
        } else if (strcmp(symbol, "art_quick_resolution_trampoline") == 0) {
            source = g_classlinker_trampolines.resolution;
        } else if (strcmp(symbol, "art_quick_imt_conflict_trampoline") == 0) {
            source = g_classlinker_trampolines.imt_conflict;
        }
    }
    if (!source || !IsMappedExecutable(source) || !IsInArtModule(source)) {
        LOGW("WxShadow ART controller: {} shared symbol unavailable: {}", label, symbol);
        return false;
    }
    void *resolved = resolve_art_trampoline(reinterpret_cast<void *>(source), env);
    if (!resolved) resolved = reinterpret_cast<void *>(source);
    const auto target = reinterpret_cast<uintptr_t>(resolved) & kPacStripMask;
    if (!IsMappedExecutable(target) || !IsInArtModule(target)) {
        LOGW("WxShadow ART controller: rejected resolved {} target {}", label,
             reinterpret_cast<void *>(target));
        return false;
    }
    void *hooked_target = nullptr;
    void *trampoline = nullptr;
    // ART may still be publishing the libart executable/shadow mapping during the first
    // post-specialize hook. rustFrida's controller retries this shared-entry publication rather
    // than changing patch semantics; keep the same stealth path and use a short bounded retry.
    for (int attempt = 0; attempt < 4 && !trampoline; ++attempt) {
        trampoline = InstallArtRouterWithFallback(
            reinterpret_cast<void *>(target), static_cast<uint32_t>(g_layout.entry_offset),
            env, &hooked_target, 1, 0, 0, 0);
        if (!trampoline && attempt < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    if (!trampoline) {
        LOGW("WxShadow ART controller: {} shared router installation failed", label);
        return false;
    }
    router->source = source;
    router->target = reinterpret_cast<uintptr_t>(
        hooked_target ? hooked_target : reinterpret_cast<void *>(target));
    router->trampoline = reinterpret_cast<uintptr_t>(trampoline);
    LOGI("WxShadow ART controller: shared {} ready source={} target={} trampoline={} pre_scan=0",
         label, reinterpret_cast<void *>(source),
         reinterpret_cast<void *>(router->target), trampoline);
    return true;
}

bool ResolveRuntime(JNIEnv *env, uintptr_t *runtime) {
    if (!env || !runtime) return false;
    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) return false;
    const auto vm_addr = reinterpret_cast<uintptr_t>(vm);
    if (!IsReadable(vm_addr + sizeof(void *), sizeof(uintptr_t))) return false;
    const auto value = *reinterpret_cast<const uintptr_t *>(vm_addr + sizeof(void *));
    if (!value) return false;
    *runtime = value & kPacStripMask;
    return *runtime != 0 && IsReadable(*runtime, sizeof(uintptr_t));
}

bool ProbeInstrumentationLayout(uintptr_t runtime, InstrumentationLayout *layout) {
    if (!runtime || !layout) return false;
    auto boot_image_deopt = reinterpret_cast<uintptr_t>(
        ResolveArtSymbol("_ZN3art7Runtime19DeoptimizeBootImageEv"));
    if (!boot_image_deopt || !IsMappedExecutable(boot_image_deopt) ||
        !IsInArtModule(boot_image_deopt) ||
        !IsReadable(boot_image_deopt, 30 * sizeof(uint32_t))) {
        return false;
    }

    // ART has two layouts: Runtime::instrumentation_ is embedded on older
    // releases and held through a pointer on newer ART APEX builds. Mirror
    // rustFrida's instruction probe, then validate the discovered object.
    for (size_t i = 0; i < 30; ++i) {
        const uint32_t insn = *reinterpret_cast<const uint32_t *>(
            boot_image_deopt + i * sizeof(uint32_t));
        const uint32_t masked = insn & 0xFF800000u;
        if (masked == 0x91000000u || masked == 0x91400000u) {
            const uint32_t rd = insn & 0x1Fu;
            const uint32_t rn = (insn >> 5) & 0x1Fu;
            const size_t imm12 = (insn >> 10) & 0xFFFu;
            const size_t shift = (insn >> 22) & 0x3u;
            const size_t offset = shift == 1 ? (imm12 << 12) : imm12;
            // Embedded Runtime::instrumentation_ uses the receiver register
            // chosen by the compiler; unlike the pointer form, it need not be
            // based in x0.
            if (rd == 31 || rn == 31 || offset < 0x100 || offset > 0x400) continue;
            if (!IsReadable(runtime + offset, sizeof(uintptr_t))) continue;
            layout->runtime_offset = offset;
            layout->pointer_mode = false;
            return true;
        }
        if ((insn & 0xFFC00000u) == 0xF9400000u) {
            const uint32_t rt = insn & 0x1Fu;
            const uint32_t rn = (insn >> 5) & 0x1Fu;
            const size_t imm12 = (insn >> 10) & 0xFFFu;
            const size_t offset = imm12 * sizeof(uintptr_t);
            if (rt == 0 || rn != 0 || offset < 0x100 || offset > 0x400) continue;
            if (!IsReadable(runtime + offset, sizeof(uintptr_t))) continue;
            const auto instrumentation = *reinterpret_cast<const uintptr_t *>(runtime + offset);
            if (!instrumentation || !IsReadable(instrumentation, sizeof(uintptr_t))) continue;
            layout->runtime_offset = offset;
            layout->pointer_mode = true;
            return true;
        }
    }
    return false;
}

bool ResolveInstrumentation(JNIEnv *env, uintptr_t *instrumentation) {
    if (!instrumentation) return false;
    uintptr_t runtime = 0;
    if (!ResolveRuntime(env, &runtime)) return false;
    InstrumentationLayout layout;
    if (!ProbeInstrumentationLayout(runtime, &layout)) return false;
    auto address = runtime + layout.runtime_offset;
    if (layout.pointer_mode) {
        address = *reinterpret_cast<const uintptr_t *>(address) & kPacStripMask;
    }
    if (!address || !IsReadable(address, sizeof(uintptr_t))) return false;
    *instrumentation = address;
    return true;
}

bool InferAccessFlagsOffset(JNIEnv *env, jobject executable, uintptr_t art_method,
                            size_t method_size, size_t *access_flags_offset) {
    jint modifiers = 0;
    if (!ReadExecutableModifiers(env, executable, &modifiers) ||
        !IsReadable(art_method, method_size)) {
        return false;
    }

    constexpr uint32_t kJavaModifierMask = 0x0000FFFFu;
    const uint32_t expected = static_cast<uint32_t>(modifiers) & kJavaModifierMask;
    size_t fallback = std::numeric_limits<size_t>::max();
    for (size_t offset = sizeof(uint32_t); offset + sizeof(uint32_t) <= method_size;
         offset += sizeof(uint32_t)) {
        const auto flags = *reinterpret_cast<const uint32_t *>(art_method + offset);
        if ((flags & kJavaModifierMask) != expected) continue;
        if (offset == 4) {
            *access_flags_offset = offset;
            return true;
        }
        fallback = offset;
    }
    if (fallback != std::numeric_limits<size_t>::max()) {
        *access_flags_offset = fallback;
        return true;
    }
    return false;
}

bool InferFridaAccessFlagsOffset(uintptr_t art_method, size_t *access_flags_offset) {
    if (!art_method || !access_flags_offset || !IsReadable(art_method, 64)) return false;

    // Source: rustFrida jni_core.rs::probe_art_method_spec_frida. Process's
    // getElapsedCpuTime() is public|static|final|native (0x0119); the listed
    // ART noise bits are ignored while scanning the live ArtMethod.
    constexpr uint32_t kExpectedFlags = 0x0119u;
    constexpr uint32_t kNoiseMask = kAccFastInterpreterToInterpreterInvoke |
                                     kAccPublicApi | kAccNterpInvokeFastPath;
    constexpr uint32_t kRelevantMask = ~kNoiseMask;
    for (size_t offset = 0; offset < 64; offset += sizeof(uint32_t)) {
        const auto flags = *reinterpret_cast<const volatile uint32_t *>(art_method + offset);
        if ((flags & kRelevantMask) == kExpectedFlags) {
            *access_flags_offset = offset;
            return true;
        }
    }
    return false;
}

bool InferNativeDataOffset(uintptr_t art_method, size_t *data_offset) {
    if (!art_method || !data_offset || !IsReadable(art_method, 64)) return false;
    uintptr_t runtime_start = 0;
    uintptr_t runtime_end = 0;
    if (!FindModuleRange("libandroid_runtime.so", &runtime_start, &runtime_end)) return false;
    // Source: rustFrida jni_core.rs::probe_art_method_spec_frida scans every
    // 4-byte slot even though data_ is pointer-sized.
    for (size_t offset = 0; offset + sizeof(uint64_t) <= 64; offset += sizeof(uint32_t)) {
        const uintptr_t candidate = *reinterpret_cast<const uintptr_t *>(art_method + offset);
        const uintptr_t stripped = candidate & kPacStripMask;
        if (stripped >= runtime_start && stripped < runtime_end &&
            IsMappedExecutable(candidate)) {
            *data_offset = offset;
            return true;
        }
    }
    return false;
}

bool ProbeLegacyEntryCandidates(uintptr_t art_method, size_t *entry_offset) {
    if (!art_method || !entry_offset || art_method < 0x1000 ||
        !IsReadable(art_method, 40)) {
        return false;
    }

    const auto read_u64 = [](uintptr_t address) -> uintptr_t {
        if (!IsReadable(address, sizeof(uintptr_t))) return 0;
        return *reinterpret_cast<const volatile uintptr_t *>(address);
    };
    const auto read_u32 = [](uintptr_t address) -> uint32_t {
        if (!IsReadable(address, sizeof(uint32_t))) return 0;
        return *reinterpret_cast<const volatile uint32_t *>(address);
    };
    const auto value_24 = read_u64(art_method + 24);
    const auto value_32 = read_u64(art_method + 32);
    const bool code_24 = IsMappedExecutable(value_24);
    const bool code_32 = IsMappedExecutable(value_32);
    if (code_24 && !code_32) {
        *entry_offset = 24;
        return true;
    }
    if (code_32 && !code_24) {
        *entry_offset = 32;
        return true;
    }
    if (code_24 && code_32) {
        // Source: rustFrida jni_core.rs::probe_entry_point_offset_legacy.
        // When both candidates look executable, the adjacent dex index probe
        // selects the field that belongs to the current ArtMethod.
        const auto current_dex_index = read_u32(art_method + 12);
        const auto next_at_32 = read_u32(art_method + 32 + 12);
        *entry_offset = next_at_32 == current_dex_index + 1 ? 24 : 32;
        return true;
    }
    return false;
}

size_t InferLegacyEntryOffset(JNIEnv *env, uintptr_t target_art_method) {
    // Source: rustFrida jni_core.rs::probe_entry_point_offset_legacy. Vector's
    // normal JNI API has no raw-clone thread, so the Object.hashCode probe is
    // safe here whenever the primary candidates do not identify the field.
    if (target_art_method < 0x1000 || !IsReadable(target_art_method, 40)) {
        LOGW("WxShadow ART legacy layout probe: invalid ArtMethod {}, using entry offset 24",
             reinterpret_cast<void *>(target_art_method));
        return 24;
    }

    size_t entry_offset = 0;
    if (ProbeLegacyEntryCandidates(target_art_method, &entry_offset)) {
        LOGI("WxShadow ART legacy layout probe: ArtMethod {} entry offset {}",
             reinterpret_cast<void *>(target_art_method), entry_offset);
        return entry_offset;
    }

    if (env) {
        auto object_class = env->FindClass("java/lang/Object");
        if (object_class && !env->ExceptionCheck()) {
            auto hash_code = env->GetMethodID(object_class, "hashCode", "()I");
            if (hash_code && !env->ExceptionCheck()) {
                auto reflected = env->ToReflectedMethod(object_class, hash_code, JNI_FALSE);
                if (reflected && !env->ExceptionCheck()) {
                    const auto known_method = DecodeReflectedArtMethod(env, reflected);
                    if (ProbeLegacyEntryCandidates(known_method, &entry_offset)) {
                        env->DeleteLocalRef(reflected);
                        env->DeleteLocalRef(object_class);
                        LOGI("WxShadow ART legacy layout probe: Object.hashCode entry offset {}",
                             entry_offset);
                        return entry_offset;
                    }
                }
                if (reflected) env->DeleteLocalRef(reflected);
            }
            env->ExceptionClear();
            env->DeleteLocalRef(object_class);
        } else {
            env->ExceptionClear();
            if (object_class) env->DeleteLocalRef(object_class);
        }
    }

    LOGW("WxShadow ART legacy layout probe: no executable candidate, using entry offset 24");
    return 24;
}

jobject FindKnownNativeMethod(JNIEnv *env, uintptr_t *art_method) {
    if (!env || !art_method) return nullptr;
    auto process = env->FindClass("android/os/Process");
    if (!process) {
        env->ExceptionClear();
        return nullptr;
    }
    auto method = env->GetStaticMethodID(process, "getElapsedCpuTime", "()J");
    if (!method) {
        env->ExceptionClear();
        env->DeleteLocalRef(process);
        return nullptr;
    }
    auto reflected = env->ToReflectedMethod(process, method, JNI_TRUE);
    if (!reflected) {
        env->ExceptionClear();
        env->DeleteLocalRef(process);
        return nullptr;
    }
    *art_method = DecodeReflectedArtMethod(env, reflected);
    env->DeleteLocalRef(process);
    return reflected;
}

std::string ClassName(JNIEnv *env, jobject clazz) {
    if (!clazz) return {};
    auto class_class = env->FindClass("java/lang/Class");
    if (!class_class) return {};
    auto get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
    if (!get_name) {
        env->DeleteLocalRef(class_class);
        return {};
    }
    auto name = reinterpret_cast<jstring>(env->CallObjectMethod(clazz, get_name));
    if (!name || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_class);
        return {};
    }
    const char *utf = env->GetStringUTFChars(name, nullptr);
    std::string result = utf ? utf : "";
    if (utf) env->ReleaseStringUTFChars(name, utf);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(class_class);
    return result;
}

ValueKind KindFromClass(JNIEnv *env, jobject clazz) {
    const auto name = ClassName(env, clazz);
    if (name == "void") return ValueKind::Void;
    if (name == "boolean") return ValueKind::Boolean;
    if (name == "byte") return ValueKind::Byte;
    if (name == "char") return ValueKind::Char;
    if (name == "short") return ValueKind::Short;
    if (name == "int") return ValueKind::Int;
    if (name == "long") return ValueKind::Long;
    if (name == "float") return ValueKind::Float;
    if (name == "double") return ValueKind::Double;
    return ValueKind::Object;
}

// Source: rustFrida install.rs::is_critical_native_signature_supported.
//
// `@CriticalNative` has a hard ABI requirement: static, with primitive-only parameters and a
// primitive return. Neither reference type (`L...;`) nor array (`[...`) may appear, because the
// critical ABI passes raw arguments in x0-x7/d0-d7 with no JNIEnv*/jclass receiver and no way to
// hand a managed reference back.
//
// This runs during route classification, i.e. before the full parameter_kinds / return_kind
// resolution that fills MethodState. It therefore probes the reflective Executable directly and
// leaves no state behind. A false negative is safe by construction: it only downgrades a
// suspicious flag to "ordinary native", which hooks the method anyway. A false positive is the
// dangerous direction, and cannot happen — asserting the critical ABI requires positively
// observing static + primitive-only, which the critical ABI genuinely needs.
bool CriticalNativeSignatureSupportedFromReflection(JNIEnv *env, jobject reflected_method) {
    if (!env || !reflected_method) return false;

    auto executable_class = env->FindClass("java/lang/reflect/Executable");
    if (!executable_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (executable_class) env->DeleteLocalRef(executable_class);
        return false;
    }

    bool supported = false;
    jobjectArray params = nullptr;
    do {
        auto get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");
        auto get_params = env->GetMethodID(executable_class, "getParameterTypes",
                                           "()[Ljava/lang/Class;");
        if (!get_modifiers || !get_params) {
            env->ExceptionClear();
            break;
        }
        // java.lang.reflect.Constructor has no getReturnType(); it is also never @CriticalNative
        // (the critical ABI is only defined for static methods), so failing closed is correct.
        auto method_class = env->GetObjectClass(reflected_method);
        if (!method_class || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (method_class) env->DeleteLocalRef(method_class);
            break;
        }
        auto get_return_type = env->GetMethodID(method_class, "getReturnType",
                                                "()Ljava/lang/Class;");
        env->DeleteLocalRef(method_class);
        if (!get_return_type) {
            env->ExceptionClear();
            break;
        }

        const jint modifiers = env->CallIntMethod(reflected_method, get_modifiers);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }
        if ((static_cast<uint32_t>(modifiers) & kAccStatic) == 0) break;

        auto return_class = env->CallObjectMethod(reflected_method, get_return_type);
        if (!return_class || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (return_class) env->DeleteLocalRef(return_class);
            break;
        }
        const ValueKind return_kind = KindFromClass(env, return_class);
        env->DeleteLocalRef(return_class);
        if (return_kind == ValueKind::Object) break;

        params = reinterpret_cast<jobjectArray>(
            env->CallObjectMethod(reflected_method, get_params));
        if (!params || env->ExceptionCheck()) {
            env->ExceptionClear();
            break;
        }

        supported = true;
        const jsize count = env->GetArrayLength(params);
        for (jsize i = 0; i < count; ++i) {
            jobject type = env->GetObjectArrayElement(params, i);
            if (!type || env->ExceptionCheck()) {
                env->ExceptionClear();
                if (type) env->DeleteLocalRef(type);
                supported = false;
                break;
            }
            if (KindFromClass(env, type) == ValueKind::Object) supported = false;
            env->DeleteLocalRef(type);
            if (!supported) break;
        }
    } while (false);

    if (params) env->DeleteLocalRef(params);
    env->DeleteLocalRef(executable_class);
    return supported;
}

void ThrowIllegalArgument(JNIEnv *env, const char *message) {
    auto cls = env->FindClass("java/lang/IllegalArgumentException");
    if (cls) {
        env->ThrowNew(cls, message);
        env->DeleteLocalRef(cls);
    }
}

void ThrowInvocationTarget(JNIEnv *env, jthrowable cause) {
    auto cls = env->FindClass("java/lang/reflect/InvocationTargetException");
    if (!cls) return;
    auto ctor = env->GetMethodID(cls, "<init>", "(Ljava/lang/Throwable;)V");
    if (ctor) {
        auto wrapper = env->NewObject(cls, ctor, cause);
        if (wrapper) env->Throw(static_cast<jthrowable>(wrapper));
        if (wrapper) env->DeleteLocalRef(wrapper);
    }
    env->DeleteLocalRef(cls);
}

void SyncOriginalCloneForJni(const MethodState &state) {
    if (!state.original_art_method || !state.original_clone ||
        !IsReadable(state.original_art_method, state.method_size) ||
        !IsReadable(state.original_clone, state.method_size)) {
        return;
    }

    // Source: AOSP art/runtime/reflection.cc::InvokeWithJValues reads
    // ArtMethod::GetInterfaceMethodIfProxy() metadata before invoking, and
    // art/runtime/art_method.cc::GetObsoleteDexCache()/GetNonObsoleteMethod()
    // require a real, current ArtMethod layout. A malloc clone can outlive GC or
    // runtime updates to declaring_class_/access_flags_/data_. Keep the fields
    // that ART consults before Invoke() in sync for the JNI fallback path.
    *reinterpret_cast<volatile uint32_t *>(state.original_clone) =
        *reinterpret_cast<volatile const uint32_t *>(state.original_art_method);

    if (state.access_flags_offset + sizeof(uint32_t) <= state.method_size) {
        *reinterpret_cast<volatile uint32_t *>(state.original_clone +
                                               state.access_flags_offset) =
            *reinterpret_cast<volatile const uint32_t *>(state.original_art_method +
                                                        state.access_flags_offset);
    }

    if (!state.native_entry_hook &&
        state.data_offset + sizeof(uintptr_t) <= state.method_size) {
        *reinterpret_cast<volatile uintptr_t *>(state.original_clone +
                                                state.data_offset) =
            *reinterpret_cast<volatile const uintptr_t *>(state.original_art_method +
                                                         state.data_offset);
    }

}

jvalue ReadArgument(JNIEnv *env, jobject value, ValueKind kind, bool *ok) {
    jvalue result{};
    *ok = true;
    if (kind == ValueKind::Object) {
        result.l = value;
        return result;
    }
    if (!value) {
        ThrowIllegalArgument(env, "null primitive argument");
        *ok = false;
        return result;
    }
    const auto number = env->FindClass("java/lang/Number");
    const auto character = env->FindClass("java/lang/Character");
    const auto boolean = env->FindClass("java/lang/Boolean");
    if (!number || !character || !boolean || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (number) env->DeleteLocalRef(number);
        if (character) env->DeleteLocalRef(character);
        if (boolean) env->DeleteLocalRef(boolean);
        *ok = false;
        return result;
    }
    if (kind == ValueKind::Boolean) {
        auto mid = env->GetMethodID(boolean, "booleanValue", "()Z");
        if (!env->IsInstanceOf(value, boolean) || !mid) {
            ThrowIllegalArgument(env, "Expected Boolean");
            *ok = false;
        } else {
            result.z = env->CallBooleanMethod(value, mid);
        }
    } else if (kind == ValueKind::Char) {
        auto mid = env->GetMethodID(character, "charValue", "()C");
        if (!env->IsInstanceOf(value, character) || !mid) {
            ThrowIllegalArgument(env, "Expected Character");
            *ok = false;
        } else {
            result.c = env->CallCharMethod(value, mid);
        }
    } else {
        if (!env->IsInstanceOf(value, number)) {
            ThrowIllegalArgument(env, "Expected Number");
            *ok = false;
        } else {
            switch (kind) {
            case ValueKind::Byte:
                result.b = env->CallByteMethod(value, env->GetMethodID(number, "byteValue", "()B"));
                break;
            case ValueKind::Short:
                result.s = env->CallShortMethod(value, env->GetMethodID(number, "shortValue", "()S"));
                break;
            case ValueKind::Int:
                result.i = env->CallIntMethod(value, env->GetMethodID(number, "intValue", "()I"));
                break;
            case ValueKind::Long:
                result.j = env->CallLongMethod(value, env->GetMethodID(number, "longValue", "()J"));
                break;
            case ValueKind::Float:
                result.f = env->CallFloatMethod(value, env->GetMethodID(number, "floatValue", "()F"));
                break;
            case ValueKind::Double:
                result.d = env->CallDoubleMethod(value, env->GetMethodID(number, "doubleValue", "()D"));
                break;
            default:
                break;
            }
        }
    }
    env->DeleteLocalRef(number);
    env->DeleteLocalRef(character);
    env->DeleteLocalRef(boolean);
    if (env->ExceptionCheck()) *ok = false;
    return result;
}

jobject BoxReturn(JNIEnv *env, ValueKind kind, const jvalue &value) {
    const char *clazz_name = nullptr;
    const char *sig = nullptr;
    switch (kind) {
    case ValueKind::Boolean: clazz_name = "java/lang/Boolean"; sig = "(Z)Ljava/lang/Boolean;"; break;
    case ValueKind::Byte: clazz_name = "java/lang/Byte"; sig = "(B)Ljava/lang/Byte;"; break;
    case ValueKind::Char: clazz_name = "java/lang/Character"; sig = "(C)Ljava/lang/Character;"; break;
    case ValueKind::Short: clazz_name = "java/lang/Short"; sig = "(S)Ljava/lang/Short;"; break;
    case ValueKind::Int: clazz_name = "java/lang/Integer"; sig = "(I)Ljava/lang/Integer;"; break;
    case ValueKind::Long: clazz_name = "java/lang/Long"; sig = "(J)Ljava/lang/Long;"; break;
    case ValueKind::Float: clazz_name = "java/lang/Float"; sig = "(F)Ljava/lang/Float;"; break;
    case ValueKind::Double: clazz_name = "java/lang/Double"; sig = "(D)Ljava/lang/Double;"; break;
    case ValueKind::Object: return value.l;
    case ValueKind::Void: return nullptr;
    }
    auto clazz = env->FindClass(clazz_name);
    if (!clazz) return nullptr;
    auto value_of = env->GetStaticMethodID(clazz, "valueOf", sig);
    jobject result = nullptr;
    if (value_of) {
        jvalue argument{};
        switch (kind) {
        case ValueKind::Boolean: argument.z = value.z; break;
        case ValueKind::Byte: argument.b = value.b; break;
        case ValueKind::Char: argument.c = value.c; break;
        case ValueKind::Short: argument.s = value.s; break;
        case ValueKind::Int: argument.i = value.i; break;
        case ValueKind::Long: argument.j = value.j; break;
        case ValueKind::Float: argument.f = value.f; break;
        case ValueKind::Double: argument.d = value.d; break;
        default: break;
        }
        result = env->CallStaticObjectMethodA(clazz, value_of, &argument);
    }
    env->DeleteLocalRef(clazz);
    return result;
}

extern "C" int art_router_stack_check(uint64_t replacement) {
    if (hook_managed_reentry_guard_active() != 0) return 0;
    return ShouldReplaceForManagedStack(static_cast<uintptr_t>(replacement)) ? 1 : 0;
}
bool LooksLikeCalleeSaveMethodArray(uintptr_t runtime, uintptr_t suspend_offset) {
    if (suspend_offset < 5 * sizeof(uintptr_t)) return false;
    const auto first = runtime + suspend_offset - 5 * sizeof(uintptr_t);
    uintptr_t previous = 0;
    for (size_t i = 0; i < 6; ++i) {
        if (!IsReadable(first + i * sizeof(uintptr_t), sizeof(uintptr_t))) return false;
        const auto method = *reinterpret_cast<volatile const uintptr_t *>(
            first + i * sizeof(uintptr_t)) & kPacStripMask;
        if (method == 0 || (method & 0x3u) != 0 || method == previous) return false;
        previous = method;
    }
    return true;
}

uintptr_t ResolveCalleeSaveSuspendMethod() {
    uintptr_t runtime = 0;
    if (!ResolveRuntime(g_init_env, &runtime)) return 0;

    // Source reference: rustFrida java_fast_api.rs::resolve_callee_save_suspend_method.
    constexpr uint32_t kCalleeSaveEverythingForSuspendCheck = 5;
    constexpr const char *kCandidates[] = {
        "_ZN3art7Runtime19GetCalleeSaveMethodENS_14CalleeSaveTypeE",
        "_ZN3art7Runtime28GetCalleeSaveMethodUncheckedENS_14CalleeSaveTypeE",
        "_ZNK3art7Runtime19GetCalleeSaveMethodENS_14CalleeSaveTypeE",
        "_ZNK3art7Runtime28GetCalleeSaveMethodENS_14CalleeSaveTypeE",
        "_ZN3art7Runtime19GetCalleeSaveMethodENS0_14CalleeSaveTypeE",
        "_ZNK3art7Runtime19GetCalleeSaveMethodENS0_14CalleeSaveTypeE",
    };
    using GetCalleeSaveMethodFn = uintptr_t (*)(uintptr_t, uint32_t);
    for (const auto *name : kCandidates) {
        auto symbol = ResolveArtSymbol(name);
        if (!symbol) continue;
        const auto method = reinterpret_cast<GetCalleeSaveMethodFn>(symbol)(
            runtime, kCalleeSaveEverythingForSuspendCheck) & kPacStripMask;
        if (method != 0 && IsReadable(method, sizeof(uintptr_t))) {
            LOGI("WxShadow ART callee-save suspend method via {}: {}", name,
                 reinterpret_cast<void *>(method));
            return method;
        }
    }

    // Source reference: rustFrida ART_CALLEE_SAVE_PROFILES, API 31-36.
    // This is a source-defined profile, not a guessed vendor layout.
    char sdk_value[PROP_VALUE_MAX] = {};
    const int sdk = __system_property_get("ro.build.version.sdk", sdk_value) > 0
                        ? atoi(sdk_value)
                        : 0;
    constexpr uintptr_t kApi31CalleeSaveSuspendOffset = 0x28;
    if (sdk >= 31 && sdk <= 36 &&
        LooksLikeCalleeSaveMethodArray(runtime, kApi31CalleeSaveSuspendOffset)) {
        const auto method = *reinterpret_cast<volatile const uintptr_t *>(
            runtime + kApi31CalleeSaveSuspendOffset) & kPacStripMask;
        LOGI("WxShadow ART callee-save suspend method via profile api31-36: {}",
             reinterpret_cast<void *>(method));
        return method;
    }
    LOGW("WxShadow ART callee-save suspend method unavailable");
    return 0;
}

uintptr_t ResolveQuickTestSuspendEntrypoint() {
    uintptr_t thread = 0;
    if (g_thread_current) thread = g_thread_current() & kPacStripMask;
    if (!thread && g_init_env && IsReadable(reinterpret_cast<uintptr_t>(g_init_env) +
                                             sizeof(uintptr_t), sizeof(uintptr_t))) {
        thread = *reinterpret_cast<volatile const uintptr_t *>(
            reinterpret_cast<uintptr_t>(g_init_env) + sizeof(uintptr_t)) & kPacStripMask;
    }
    if (!thread) return 0;

    // Source reference: rustFrida java_fast_api.rs::quick_entrypoint.
    constexpr size_t kQuickEntrypointCount = 174;
    constexpr size_t kQuickTestSuspendIndex = 105;
    constexpr size_t kQuickJniMethodStartIndex = 45;
    constexpr size_t kQuickJniMethodEndIndex = 46;
    constexpr size_t kQuickScanLimit = 16384;
    constexpr size_t kMinLibartPointers = 40;
    uintptr_t art_start = 0;
    uintptr_t art_end = 0;
    if (!FindModuleRange("libart.so", &art_start, &art_end)) return 0;
    const auto in_art = [art_start, art_end](uintptr_t value) {
        const auto address = value & kPacStripMask;
        return address >= art_start && address < art_end;
    };

    const size_t block_size = kQuickEntrypointCount * sizeof(uintptr_t);
    const size_t max_offset = kQuickScanLimit - block_size;
    for (size_t offset = 0; offset <= max_offset; offset += sizeof(uintptr_t)) {
        if (!IsReadable(thread + offset, block_size)) continue;
        const auto base = thread + offset;
        const auto start = *reinterpret_cast<volatile const uintptr_t *>(
            base + kQuickJniMethodStartIndex * sizeof(uintptr_t));
        const auto end = *reinterpret_cast<volatile const uintptr_t *>(
            base + kQuickJniMethodEndIndex * sizeof(uintptr_t));
        if (!in_art(start) || !in_art(end) || offset < 2 * sizeof(uintptr_t)) continue;
        if (!in_art(*reinterpret_cast<volatile const uintptr_t *>(base - 2 * sizeof(uintptr_t))) ||
            !in_art(*reinterpret_cast<volatile const uintptr_t *>(base - sizeof(uintptr_t)))) {
            continue;
        }
        size_t libart_pointers = 0;
        for (size_t i = 0; i < kQuickEntrypointCount; ++i) {
            if (in_art(*reinterpret_cast<volatile const uintptr_t *>(
                    base + i * sizeof(uintptr_t)))) {
                ++libart_pointers;
            }
        }
        if (libart_pointers < kMinLibartPointers) continue;
        const auto entry = *reinterpret_cast<volatile const uintptr_t *>(
            base + kQuickTestSuspendIndex * sizeof(uintptr_t)) & kPacStripMask;
        if (entry != 0) {
            LOGI("WxShadow ART quick test-suspend entrypoint: {} (Thread+{})",
                 reinterpret_cast<void *>(entry), offset);
            return entry;
        }
    }
    LOGW("WxShadow ART quick test-suspend entrypoint unavailable");
    return 0;
}

extern "C" void *art_quick_callee_save_suspend_method() {
    static std::once_flag once;
    static uintptr_t method = 0;
    std::call_once(once, [] { method = ResolveCalleeSaveSuspendMethod(); });
    return reinterpret_cast<void *>(method);
}

extern "C" void *art_quick_test_suspend_entrypoint() {
    std::call_once(g_quick_entrypoints_once,
                   [] { g_quick_test_suspend_entrypoint = ResolveQuickTestSuspendEntrypoint(); });
    return reinterpret_cast<void *>(g_quick_test_suspend_entrypoint);
}

alignas(16) std::array<uint8_t, 64> g_dummy_oat_header{};

size_t g_decode_gc_masks_guard_ready = 0;

/* Resolved (symbol- and module-validated) address of
 * art::CodeInfo::DecodeGcMasksOnly. It is the only function on this device
 * where a NULL OatQuickMethodHeader* is expected as an argument, so it is the
 * only place TryHandleNullOatHeader may substitute g_dummy_oat_header.
 * InitializeDecodeGcMasksGuard locates its header load inside the first
 * kDecodeGcMasksLoadWindow bytes of the function; the signal fallback reuses
 * exactly that window instead of guessing a function size. */
std::atomic<uintptr_t> g_decode_gc_masks_symbol{0};
constexpr uintptr_t kDecodeGcMasksLoadWindow = 0x80;

/**
 * rustFrida parity —— **主动**版 NULL OatQuickMethodHeader 防护。
 *
 * Source reference: rustFrida quickjs-hook/src/jsapi/java/art_controller.rs
 *   install_decode_gc_masks_only_null_guard()  (:2175)
 *   on_decode_gc_masks_only_enter()            (:2119)
 *   find_decode_gc_masks_only_header_load()    (:2125)
 *
 * 被动层 TryHandleNullOatHeader()（本文件下方，与 rustFrida crash_handler.rs:207 逐行同构）
 * 只能在**故障已经发生之后**把 faulting LDR 的 base 换成 dummy header 再重执行。可是这类
 * 故障正好发生在 CC GC 根翻转期间（ConcurrentCopying::ThreadFlipRoots **持有
 * thread_list_lock_**），失败代价极高，实测即 hunter 的「滑一下就卡死」：
 *   art_method.cc:497 Check failed: found_virtual（GC 走栈 GetOatQuickMethodHeader）
 *   → Runtime::Abort → ThreadList::Dump 在同一把锁上 RunCheckpoint 卡住
 *   → 全进程新线程 attach 死锁（InsetsAnimationThread 起不来）
 *   → 主线程 HandlerThread.getLooper 里 Object.wait 等死 → Input dispatching timed out。
 *
 * 主动层在解引用**之前**就把 dummy header 填好：挂 CodeInfo::DecodeGcMasksOnly 内部第一条
 * header 读取指令，进入回调时若 x0（header 指针）为 0 就替换成 g_dummy_oat_header，
 * 故障根本不产生（也就不会在持锁上下文里触发 abort/信号处理）。
 *
 * 目标地址取法：符号 + 函数内第一条 `ldr w?, [x0]`，而不是 rustFrida 的整段 prologue
 * pattern 扫描 —— 后者是 API 36 专用字节，本机 Android 12 实测为：
 *   _ZN3art8CodeInfo17DecodeGcMasksOnlyEPKNS_20OatQuickMethodHeaderE @0x669eec
 *   +0x00  5f 24 03 d5   paciasp
 *   +0x04  fd 0f 1a f8   str x29, [sp, #-0x60]!
 *   …                    （prologue 不动 x0/x1）
 *   +0x34  09 00 40 b9   ldr w9, [x0]   ← 第一条 header 读取，挂这里
 * 用符号解析 + 前向扫描 `ldr w?,[x0]` 对版本变化更宽容，且不会误挂到
 * GetOatQuickMethodHeader 里同形状的 load 上（rustFrida 的原注释也指出了这个风险）。
 *
 * 可用 VECTOR_ART_DECODE_GUARD=0 关闭（做 A/B 对照用）。
 */
static void OnDecodeGcMasksOnlyEnter(HookContext *ctx, void *) {
    if (ctx && ctx->x[0] == 0) {
        ctx->x[0] = reinterpret_cast<uint64_t>(g_dummy_oat_header.data());
    }
}

size_t InitializeDecodeGcMasksGuard() {
    static bool attempted = false;    if (attempted) return g_decode_gc_masks_guard_ready;
    attempted = true;

    if (const char *env = getenv("VECTOR_ART_DECODE_GUARD")) {
        if (env[0] == '0' && env[1] == '\0') {
            LOGI("WxShadow ART DecodeGcMasksOnly guard: disabled by env");
            return 0;
        }
    }

    const auto entry = reinterpret_cast<uintptr_t>(ResolveArtSymbol(
        "_ZN3art8CodeInfo17DecodeGcMasksOnlyEPKNS_20OatQuickMethodHeaderE")) &
        kPacStripMask;
    if (!entry || !IsInArtModule(entry)) {
        LOGW("WxShadow ART DecodeGcMasksOnly guard: symbol unresolved");
        return 0;
    }

    // 函数内第一条 `ldr w?, [x0]`（encoding 0xB9400000 | Rt，imm12 = 0，Rn = 0）。
    // 走到这里说明符号地址正确；再核对 prologue 头两字，避免符号表漂移导致乱挂。
    //
    // Publish the address as soon as it is symbol-validated: TryHandleNullOatHeader
    // must recognise "this fault PC is inside DecodeGcMasksOnly" even when the
    // prologue check below fails (measured on this device: p1=0xa9016ffc), which is
    // precisely the case where no attach guard protects the NULL header.
    g_decode_gc_masks_symbol.store(entry, std::memory_order_release);
    auto *const code = reinterpret_cast<const uint8_t *>(entry);
    const uint32_t p0 = *reinterpret_cast<const uint32_t *>(code);
    const uint32_t p1 = *reinterpret_cast<const uint32_t *>(code + 4);
    if (p0 != 0xd503245fu /* paciasp */ || p1 != 0xf81a0ffdu /* str x29, [sp, #-0x60]! */) {
        LOGW("WxShadow ART DecodeGcMasksOnly guard: prologue mismatch p0={:#x} p1={:#x}", p0, p1);
        return 0;
    }

    uintptr_t target = 0;
    for (size_t off = 8; off + 4 <= kDecodeGcMasksLoadWindow; off += 4) {
        const uint32_t insn = *reinterpret_cast<const uint32_t *>(code + off);
        if ((insn & 0xffc00000u) == 0xb9400000u && ((insn >> 5) & 0x1fu) == 0u &&
            ((insn >> 0) & 0x1fu) <= 30u) {
            target = entry + off;
            break;
        }
    }
    if (!target || !IsMappedExecutable(target)) {
        LOGW("WxShadow ART DecodeGcMasksOnly guard: header load not found in {} entry={}",
             reinterpret_cast<void *>(entry), reinterpret_cast<void *>(target));
        return 0;
    }

    if (hook_attach(reinterpret_cast<void *>(target), OnDecodeGcMasksOnlyEnter, nullptr,
                    nullptr, ArtStealthFlag()) != HOOK_OK) {
        LOGW("WxShadow ART DecodeGcMasksOnly guard: hook_attach failed target={}",
             reinterpret_cast<void *>(target));
        return 0;
    }
    g_decode_gc_masks_guard_ready = 1;
    LOGI("WxShadow ART DecodeGcMasksOnly NULL guard ready entry={} target={} (offset={})",
         reinterpret_cast<void *>(entry), reinterpret_cast<void *>(target),
         static_cast<unsigned long>(target - entry));
    return 1;
}


struct SigchainAction {
    bool (*sc_sigaction)(int, siginfo_t *, void *);
    sigset_t sc_mask;
    uint64_t sc_flags;
};

int Arm64LoadUnsignedBaseRegister(uint32_t instruction) {
    // Source reference: rustFrida art_controller.rs::arm64_load_unsigned_base_reg.
    if ((instruction & 0x3b400000u) != 0x39400000u) return -1;
    const int rn = static_cast<int>((instruction >> 5) & 0x1fu);
    return rn < 31 ? rn : -1;
}

bool TryHandleImplicitSuspend(int signal, siginfo_t *info, void *raw_context) {
    if (signal != SIGSEGV || !info || !raw_context) return false;
    const auto entry = g_implicit_suspend_entrypoint.load(std::memory_order_relaxed);
    const auto trigger_offset =
        g_implicit_suspend_trigger_offset.load(std::memory_order_relaxed);
    if (!entry || trigger_offset == UINTPTR_MAX) return false;

    auto *context = reinterpret_cast<ucontext_t *>(raw_context);
    auto &machine = context->uc_mcontext;
    const auto pc = static_cast<uintptr_t>(machine.pc) & kPacStripMask;
    if (!pc || (pc & 3u) != 0) return false;
    uintptr_t poll_pc = 0;
    const auto instruction = *reinterpret_cast<volatile const uint32_t *>(pc);
    if (instruction == 0xf94002b5u) {
        poll_pc = pc;
    } else {
        const auto next = *reinterpret_cast<volatile const uint32_t *>(pc + 4);
        if (next != 0xf94002b5u) return false;
        poll_pc = pc + 4;
    }

    uintptr_t thread = 0;
    if (g_thread_current) thread = g_thread_current() & kPacStripMask;
    if (!thread) thread = static_cast<uintptr_t>(machine.regs[19]) & kPacStripMask;
    if (thread && (thread & 7u) == 0) {
        const auto trigger = thread + trigger_offset;
        *reinterpret_cast<volatile uintptr_t *>(trigger) = trigger;
        machine.regs[19] = thread;
    }
    machine.regs[30] = poll_pc + 4;
    machine.pc = entry;
    return true;
}

bool TryHandleNullOatHeader(int signal, siginfo_t *info, void *raw_context) {
    if (signal != SIGSEGV || !info || !raw_context) return false;
    const auto fault = reinterpret_cast<uintptr_t>(info->si_addr);
    if (fault >= g_dummy_oat_header.size()) return false;

    auto *context = reinterpret_cast<ucontext_t *>(raw_context);
    auto &machine = context->uc_mcontext;
    const auto pc = static_cast<uintptr_t>(machine.pc) & kPacStripMask;

    /* Ownership gate (measured regression, tombstone_06).
     *
     * Without it, ANY `ldr w?/h?/b?,[Xr]` with Xr==0 whose fault lands in the
     * first 64 bytes was rewritten to the dummy header. ART's WalkStack has
     * exactly that shape on a corrupt quick frame:
     *   0x647a5c  ldr w20, [x24, #4]   // x24 = NULL ArtMethod* -> fault 0x4
     * so the walker was handed g_dummy_oat_header as the method, and the
     * resulting CHECK(existing_entry_point != nullptr) ran PrettyMethod on a
     * fake method whose DexFile* is 0. That turned a (survivable, diagnosable)
     * bad frame into a second, fatal crash inside ART's error path.
     *
     * Only art::CodeInfo::DecodeGcMasksOnly legitimately receives a NULL
     * OatQuickMethodHeader*, so only faults inside that function's header-load
     * window may be repaired. Everything else is left to the real handler. */
    const auto decode_gc_masks = g_decode_gc_masks_symbol.load(std::memory_order_acquire);
    if (!decode_gc_masks || pc < decode_gc_masks ||
        pc >= decode_gc_masks + kDecodeGcMasksLoadWindow) {
        return false;
    }

    if (pc && (pc & 3u) == 0) {
        const auto instruction = *reinterpret_cast<volatile const uint32_t *>(pc);
        const int base = Arm64LoadUnsignedBaseRegister(instruction);
        if (base >= 0 && machine.regs[base] == 0) {
            machine.regs[base] = reinterpret_cast<uintptr_t>(g_dummy_oat_header.data());
            return true;
        }
    }
    if (fault == 0x18 && machine.regs[10] == 0) {
        machine.regs[10] = reinterpret_cast<uintptr_t>(g_dummy_oat_header.data());
        return true;
    }
    return false;
}

std::atomic<unsigned long> g_delivered_sigsegv_count{0};

/**
 * 投递型 SIGSEGV：si_code == SI_USER / SI_TKILL，即 kill()/tgkill() 送来的信号，
 * 而不是取址失败（后者是 SEGV_MAPERR/SEGV_ACCERR）。
 *
 * AOSP ART 的 implicit-suspend 就是这种形态：Thread::ModifySuspendCountInternal 对停在
 * futex/ConditionVariable 上的线程执行 kill(tid, SIGSEGV)，期待处理器接管、让线程就地
 * 进入 suspend 检查（线程根本没有 faulting 地址，tombstone 里正是 `fault addr --------`）。
 * 识别它的价值：这种信号既无处可重定向、也不应被当成崩溃。
 *
 * 已知缺陷（交接文档 2.6）：si_code 只说明「有人 kill 了我」，并不区分
 *   (a) ART 的 implicit-suspend 通知，还是
 *   (b) 真实崩溃之后某个环节把自己的 SIGSEGV 重投回本线程。
 * 两种情况都是 SI_TKILL/SI_USER，所以这里一律 return true 接管，于是 (b) 形态
 * 就连 debuggerd 都走不到（13421：`exited due to signal 11` 但 4 分钟里没有任何
 * crash_dump）。是否真在吞掉真实崩溃，只能靠日志取证，不能靠推理，所以调用方会打印
 * 那份不改行为的证据行。
 */
static bool IsDeliveredSigsegv(const siginfo_t *info) {
    return info != nullptr && (info->si_code == SI_USER || info->si_code == SI_TKILL);
}

/*
 * Undelivered-SIGSEGV 证据行：纯粹为了看清「被我们接管的那条信号是什么」。
 *
 * 约束（信号处理器上下文）：
 *   - 不分配、不加锁、不 fopen(/proc)；只读寄存器与 siginfo。
 *   - 不改返回值、不改任何状态（除了下面这个限频计数器）。
 *
 * 判读方法（下次 hunter 崩后再看）：
 *   真实崩溃被重投 → pc 落在一个「会取址」的指令上，且 si_addr 形如有效地址/小偏移；
 *   ART 挂起通知   → 线程停在 futex/ConditionVariable，pc 在 syscall 或自旋点，
 *                    si_addr 通常为 NULL，且与该线程的 suspend 请求时刻对得上。
 */
static void LogDeliveredSigsegvEvidence(const siginfo_t *info, void *context) {
    const auto *machine = context ? &reinterpret_cast<ucontext_t *>(context)->uc_mcontext : nullptr;
    const auto pc = machine ? static_cast<uintptr_t>(machine->pc) & kPacStripMask : 0;
    const auto sp = machine ? static_cast<uintptr_t>(machine->sp) & kPacStripMask : 0;
    const auto lr = machine ? static_cast<uintptr_t>(machine->regs[30]) & kPacStripMask : 0;
    const auto x0 = machine ? static_cast<uintptr_t>(machine->regs[0]) & kPacStripMask : 0;
    // Only cache-backed probes here. IsMappedExecutable() fopen()s /proc/self/maps,
    // which is not reentrant-safe inside a signal handler that may have interrupted
    // the loader; IsInArtModule() and hook_is_in_exec_pool() use already-built state.
    LOGW("WxShadow SIGSEGV delivered evidence: code={} errno={} addr={} pc={} sp={} lr={} x0={} "
         "pc_in_art={} pc_in_pool={} pid={} tid={}",
         info ? info->si_code : -1, info ? info->si_errno : -1,
         info ? info->si_addr : nullptr, reinterpret_cast<void *>(pc),
         reinterpret_cast<void *>(sp), reinterpret_cast<void *>(lr),
         reinterpret_cast<void *>(x0), IsInArtModule(pc) ? "yes" : "no",
         hook_is_in_exec_pool(pc) ? "yes" : "no", static_cast<long>(getpid()),
         static_cast<long>(gettid()));
}

// ---------------------------------------------------------------------------
// Crash-time snapshot: what was the stack walker doing when it died?
//
// The WalkStack probe shows the chain BEFORE the walk; the crashes happen later,
// inside it. When the faulting PC is WalkStack itself and the register holding the
// frame's method slot is zero, the visitor's own fields say which frame it had
// reached -- information no entry-time probe can have.
//
// StackVisitor layout used here (device libart 0x647af0 reads +0x18):
//   +0x18 cur_shadow_frame_
//   +0x20 cur_quick_frame_
//   +0x28 cur_quick_frame_pc_
// Pure reads, no allocation: this runs in a signal handler.
// ---------------------------------------------------------------------------
std::atomic<uintptr_t> g_walkstack_fn{0};

void MaybeSnapshotCrashWalkStack(const ucontext_t *uc, int si_code) {
#if defined(__aarch64__)
    if (!uc) return;
    const uintptr_t fn = g_walkstack_fn.load(std::memory_order_relaxed);
    if (!fn) return;
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc) & kPacStripMask;
    // WalkStack is a few hundred bytes; a 4 KiB window keeps this comparison
    // cheap while still covering the whole function body.
    if (pc < fn || pc >= fn + 0x1000) return;

    // x0 is the StackVisitor* at entry; inside the loop the visitor is still in
    // x0 (WalkStack reloads it as `mov x0, x26` before walking), and the crashing
    // frames we have seen keep it there. Record raw registers either way so a
    // mismatch is visible rather than hidden.
    const uintptr_t visitor = static_cast<uintptr_t>(uc->uc_mcontext.regs[0]) & kPacStripMask;
    uintptr_t shadow = 0, quick = 0, quick_pc = 0, thread = 0;
    if (visitor && ProbeReadability(visitor + 0x30) == ReadableVerdict::Yes) {
        shadow = *reinterpret_cast<volatile const uintptr_t *>(visitor + 0x18) & kPacStripMask;
        quick = *reinterpret_cast<volatile const uintptr_t *>(visitor + 0x20) & kPacStripMask;
        quick_pc = *reinterpret_cast<volatile const uintptr_t *>(visitor + 0x28) & kPacStripMask;
        thread = *reinterpret_cast<volatile const uintptr_t *>(visitor + 0x8) & kPacStripMask;
    }
    const uintptr_t x24 = static_cast<uintptr_t>(uc->uc_mcontext.regs[24]) & kPacStripMask;
    LOGW("WxShadow ART WalkStack CRASH SNAPSHOT: pc={} code={} tid={} visitor={} thread={} "
         "cur_shadow={} cur_quick={} cur_quick_pc={} x24(method)={} x8={} x10={} "
         "pc_in_art={}",
         reinterpret_cast<void *>(pc), si_code, static_cast<long>(gettid()),
         reinterpret_cast<void *>(visitor), reinterpret_cast<void *>(thread),
         reinterpret_cast<void *>(shadow), reinterpret_cast<void *>(quick),
         reinterpret_cast<void *>(quick_pc), reinterpret_cast<void *>(x24),
         reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[8]) & kPacStripMask),
         reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[10]) & kPacStripMask),
         IsInArtModule(pc) ? "yes" : "no");
    // Dump around the frame ART was actually reading, not around cur_quick_frame_:
    // in every observed crash x0 (and therefore the visitor this handler can read)
    // is a garbage tagged value like 0x7fe0000000000090, while x8/x10 hold the
    // frame whose method slot is zero. x8 is the useful anchor.
    const uintptr_t frame = static_cast<uintptr_t>(uc->uc_mcontext.regs[8]) & kPacStripMask;
    if (frame) {
        // -0x40 .. +0x80 in 8-byte steps: the method slot plus enough of the
        // SaveRefsAndArgs shape around it to see whether this really is a quick
        // frame or something ART was handed by mistake.
        char line[1024] = {};
        size_t used = 0;
        for (int w = -8; w < 16; ++w) {
            uintptr_t v = 0;
            if (!SafeReadU64(frame + (intptr_t)w * 8, &v)) continue;
            const char *map = MappedNameFor(v);
            const int wrote = snprintf(line + used, sizeof(line) - used,
                                       "%s%+d:0x%" PRIxPTR "%s", used ? " " : "", w, v,
                                       map[0] ? map : "-");
            if (wrote <= 0 || used + (size_t)wrote >= sizeof(line)) break;
            used += (size_t)wrote;
        }
        LOGW("WxShadow ART WalkStack CRASH SNAPSHOT   around frame[{}]: {}",
             reinterpret_cast<void *>(frame), line);
        LOGW("WxShadow ART WalkStack CRASH SNAPSHOT   frame_map={} x9={} x11={} x19={} "
             "x20={} x21={} x22={} x23={} x25={} x28={}",
             MappedNameFor(frame)[0] ? MappedNameFor(frame) : "-",
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[9]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[11]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[19]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[20]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[21]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[22]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[23]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[25]) & kPacStripMask),
             reinterpret_cast<void *>(static_cast<uintptr_t>(uc->uc_mcontext.regs[28]) & kPacStripMask));
    }
#endif
}

extern "C" bool ArtSigchainHandler(int signal, siginfo_t *info, void *context) {
    if (TryHandleImplicitSuspend(signal, info, context) ||
        TryHandleNullOatHeader(signal, info, context)) {
        return true;
    }
    // 投递型 SIGSEGV = ART 的 implicit-suspend 触发信号，必须「接管后放行」。
    // 之前这一形态完全没被识别，直接落到 ChainPreviousSigsegvHandler 的兜底重发，
    // 实测 hunter 冷启 3 次里 2 次要死在 Heap::WaitForGcToCompleteLocked 停车处、
    // 以 code 0 (SI_USER) SIGSEGV 收场，且因为走的是 raise 而不是 debuggerd 正常
    // 崩溃路径，经常连 tombstone 都不落盘。
    // Non-behavioural evidence: recorded before any handler decision, so it also
    // covers the delivered-SIGSEGV case that never reaches debuggerd.
    if (signal == SIGSEGV && context) {
        static std::atomic<uint32_t> snap{0};
        if (snap.fetch_add(1, std::memory_order_relaxed) < 8) {
            MaybeSnapshotCrashWalkStack(reinterpret_cast<const ucontext_t *>(context),
                                        info ? info->si_code : -1);
        }
    }
    if (signal == SIGSEGV && IsDeliveredSigsegv(info)) {
        const unsigned long n =
            g_delivered_sigsegv_count.fetch_add(1, std::memory_order_relaxed) + 1;
        // 热路径限频：前 8 次全打（够看清开头几次的 pc 归属），之后 1/64。
        if (n <= 8 || (n & 63u) == 0) {
            const auto pc = context
                                ? static_cast<uintptr_t>(
                                      reinterpret_cast<ucontext_t *>(context)->uc_mcontext.pc) &
                                      kPacStripMask
                                : 0;
            LOGW("WxShadow SIGSEGV delivered (implicit suspend) handled count={} pc={} tid={}",
                 n, reinterpret_cast<void *>(pc), static_cast<long>(gettid()));
            // 同一条信号再打一行证据（不改返回、不改状态）：用来回答「13421 那种
            // signal 11 无 crash_dump 是不是被这一支吞掉的」。
            LogDeliveredSigsegvEvidence(info, context);
        }
        return true;
    }
    return false;
}

void ChainPreviousSigsegvHandler(int signal, siginfo_t *info, void *context) {
    const auto &previous = g_previous_sigsegv_action;
    if ((previous.sa_flags & SA_SIGINFO) != 0) {
        const auto handler = previous.sa_sigaction;
        if (handler && reinterpret_cast<uintptr_t>(handler) !=
                           reinterpret_cast<uintptr_t>(SIG_DFL) &&
                       reinterpret_cast<uintptr_t>(handler) !=
                           reinterpret_cast<uintptr_t>(SIG_IGN)) {
            handler(signal, info, context);
            return;
        }
    } else {
        const auto handler = previous.sa_handler;
        if (handler == SIG_IGN) return;
        if (handler != SIG_DFL && handler != nullptr) {
            handler(signal);
            return;
        }
    }
    // 没有可链的旧处理器。这里刻意不再用 ::signal(SIG_DFL) + ::raise()：
    // ::raise() 会造出 code 0 (SI_USER)、并绕过 debuggerd 的正常崩溃路径，
    // 让现场（tombstone）经常根本不落盘，而且会掩盖真实 siginfo。
    // 改为「先把默认处理装回去，再用 tgkill 重投给当前线程」——内核会给出
    // 真实 siginfo，交给系统崩溃路径（debuggerd 能正常落盘）。
    LOGW("WxShadow SIGSEGV unhandled: no chainable previous handler signal={} code={} addr={}",
         signal, info ? info->si_code : -1, info ? info->si_addr : nullptr);
    if (g_bionic_sigsegv_guard_installed.load(std::memory_order_acquire)) {
        ::sigaction(signal, &g_previous_sigsegv_action, nullptr);
    } else {
        ::signal(signal, SIG_DFL);
    }
    ::tgkill(getpid(), gettid(), signal);
}

extern "C" void ArtBionicSigsegvHandler(int signal, siginfo_t *info, void *context) {
    if (ArtSigchainHandler(signal, info, context)) return;
    ChainPreviousSigsegvHandler(signal, info, context);
}

bool InitializeSignalGuards() {
    if (const char *env = getenv("VECTOR_NO_SIGNAL_GUARD")) {
        if (env[0] != '\0' && !(env[0] == '0' && env[1] == '\0')) {
            LOGI("WxShadow ART SIGSEGV guard: disabled by env");
            return false;
        }
    }
    // Source reference: rustFrida install_managed_implicit_suspend_guard and
    // install_walkstack_sigsegv_guard. Vector has no Recomp backend, so only
    // original quick-code PCs and NULL OAT-header recovery are handled.
    if (g_thread_layout.valid) {
        auto implicit = reinterpret_cast<uintptr_t>(
            ResolveArtSymbol("art_quick_implicit_suspend")) & kPacStripMask;
        if (!implicit || !IsMappedExecutable(implicit) || !IsInArtModule(implicit)) {
            // Source reference: rustFrida java_fast_api.rs::
            // resolve_suspend_poll_entrypoint. Some ART builds expose only the
            // callee-save test-suspend entrypoint, which is still a valid poll
            // target for the same fault redirection path.
            implicit = reinterpret_cast<uintptr_t>(art_quick_test_suspend_entrypoint()) &
                       kPacStripMask;
            if (implicit && IsMappedExecutable(implicit) && IsInArtModule(implicit)) {
                LOGI("WxShadow ART implicit suspend using test-suspend fallback entry={}",
                     reinterpret_cast<void *>(implicit));
            } else {
                implicit = 0;
            }
        }
        if (implicit && IsMappedExecutable(implicit) && IsInArtModule(implicit)) {
            g_implicit_suspend_entrypoint.store(implicit, std::memory_order_relaxed);
            g_implicit_suspend_trigger_offset.store(
                g_thread_layout.suspend_trigger_offset, std::memory_order_relaxed);
            LOGI("WxShadow ART implicit suspend configured entry={} trigger_offset={}",
                 reinterpret_cast<void *>(implicit),
                 g_thread_layout.suspend_trigger_offset);
        }
    }

    if (g_sigchain_guard_installed.exchange(true, std::memory_order_acq_rel)) return true;
    void *handle = dlopen("libsigchain.so", RTLD_NOW | RTLD_LOCAL);
    void *add_symbol = handle ? dlsym(handle, "AddSpecialSignalHandlerFn")
                              : dlsym(RTLD_DEFAULT, "AddSpecialSignalHandlerFn");
    if (!add_symbol) {
        // Source reference: rustFrida art_controller.rs::
        // install_walkstack_sigsegv_guard falls back to bionic sigaction when
        // libsigchain is unavailable, while preserving the previous handler.
        struct sigaction bionic_action {};
        bionic_action.sa_sigaction = ArtBionicSigsegvHandler;
        bionic_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&bionic_action.sa_mask);
        if (::sigaction(SIGSEGV, &bionic_action, &g_previous_sigsegv_action) != 0) {
            g_sigchain_guard_installed.store(false, std::memory_order_release);
            LOGW("WxShadow ART SIGSEGV guard unavailable: libsigchain and bionic sigaction failed");
            return false;
        }
        g_bionic_sigsegv_guard_installed.store(true, std::memory_order_release);
        LOGI("WxShadow ART SIGSEGV guard ready via bionic sigaction fallback");
        return true;
    }

    SigchainAction action{};
    action.sc_sigaction = ArtSigchainHandler;
    sigemptyset(&action.sc_mask);
    action.sc_flags = 0;
    using AddSpecialSignalHandlerFn = void (*)(int, SigchainAction *);
    reinterpret_cast<AddSpecialSignalHandlerFn>(add_symbol)(SIGSEGV, &action);

    void *front_symbol = handle ? dlsym(handle, "EnsureFrontOfChain")
                                : dlsym(RTLD_DEFAULT, "EnsureFrontOfChain");
    if (front_symbol) {
        using EnsureFrontOfChainFn = void (*)(int);
        reinterpret_cast<EnsureFrontOfChainFn>(front_symbol)(SIGSEGV);
    }
    LOGI("WxShadow ART SIGSEGV guard ready via libsigchain");
    return true;
}

extern "C" uint64_t art_quick_top_quick_frame_offset() {
    return g_thread_layout.valid ? g_thread_layout.managed_stack_offset : UINT64_MAX;
}

bool InitializeBaseImpl(JNIEnv *env) {
    if (g_base_initialized) return true;
    g_init_env = env;
    // Resolve the reflection field before probing any method IDs. Android 11+
    // may return an odd/opaque jmethodID, while older releases expose the
    // ArtMethod pointer directly.
    std::call_once(g_art_method_field_once, [env] { ResolveArtMethodField(env); });
    auto object_class = env->FindClass("java/lang/Object");
    if (!object_class) return false;
    auto to_string = env->GetMethodID(object_class, "toString", "()Ljava/lang/String;");
    if (!to_string) {
        env->DeleteLocalRef(object_class);
        return false;
    }
    auto reflected = env->ToReflectedMethod(object_class, to_string, JNI_FALSE);
    const auto target = DecodeReflectedArtMethod(env, reflected);
    if (reflected) env->DeleteLocalRef(reflected);
    if (target == 0) {
        env->DeleteLocalRef(object_class);
        return false;
    }
    // ArtMethod spacing is only the initial readable window. The final
    // data_/entry_point_ offsets come from the known-native Frida probe or its
    // source-defined legacy fallback, matching rustFrida jni_core.rs.
    g_layout.method_size = std::max<size_t>(InferMethodSize(env, target), 40);
    uintptr_t native_target = 0;
    auto native_reflected = FindKnownNativeMethod(env, &native_target);
    bool native_layout_found = false;
    bool access_flags_found = false;
    if (native_target != 0) {
        size_t data_offset = 0;
        if (InferNativeDataOffset(native_target, &data_offset)) {
            g_layout.data_offset = data_offset;
            g_layout.entry_offset = data_offset + sizeof(uintptr_t);
            g_layout.method_size = std::max(g_layout.method_size,
                                            g_layout.entry_offset + sizeof(uintptr_t));
            native_layout_found = true;
        }
    }
    if (native_target != 0) {
        size_t access_flags_offset = 4;
        if (InferFridaAccessFlagsOffset(native_target, &access_flags_offset)) {
            g_layout.access_flags_offset = access_flags_offset;
            access_flags_found = true;
        }
    }
    if (native_reflected) {
        if (!access_flags_found) {
            size_t access_flags_offset = 4;
            if (InferAccessFlagsOffset(env, native_reflected, native_target,
                                       g_layout.method_size, &access_flags_offset)) {
                g_layout.access_flags_offset = access_flags_offset;
                access_flags_found = true;
            }
        }
        env->DeleteLocalRef(native_reflected);
    }

    if (!native_layout_found || !access_flags_found) {
        // Source: rustFrida jni_core.rs::probe_art_method_spec fallback. Keep
        // the source-defined candidate offsets and Object.hashCode secondary
        // probe instead of selecting an arbitrary executable ArtMethod field.
        const auto legacy_entry = InferLegacyEntryOffset(env, target);
        if (legacy_entry < sizeof(uintptr_t)) {
            LOGE("WxShadow ART backend: legacy ArtMethod entry probe returned invalid offset {}",
                 legacy_entry);
            env->DeleteLocalRef(object_class);
            return false;
        }
        g_layout.access_flags_offset = 4;
        g_layout.data_offset = legacy_entry - sizeof(uintptr_t);
        g_layout.entry_offset = legacy_entry;
        const size_t legacy_size = GetAndroidApiLevel() <= 21
                                        ? legacy_entry + 32
                                        : legacy_entry + sizeof(uintptr_t);
        g_layout.method_size = std::max(g_layout.method_size, legacy_size);
        native_layout_found = true;
        access_flags_found = true;
        LOGW("WxShadow ART backend: using rustFrida legacy ArtMethod layout fallback data={} entry={} size={}",
             g_layout.data_offset, g_layout.entry_offset, g_layout.method_size);
    }
    if (g_layout.method_size < 16) {
        LOGE("WxShadow ART backend: ArtMethod layout probe returned undersized method ({})",
             g_layout.method_size);
        env->DeleteLocalRef(object_class);
        return false;
    }
    g_layout.method_size = std::max(
        g_layout.method_size,
        std::max(g_layout.entry_offset + sizeof(uintptr_t),
                 g_layout.access_flags_offset + sizeof(uint32_t)));
    g_layout.jni_trampoline = FindJniTrampoline(env, native_target);
    if (!g_layout.jni_trampoline) {
        LOGE("WxShadow ART backend: cannot resolve JNI trampoline from symbols/ClassLinker/known native");
        env->DeleteLocalRef(object_class);
        return false;
    }
    env->DeleteLocalRef(object_class);

    ExecPoolRange existing_hook_pool{};
    if (hook_engine_get_exec_ranges(&existing_hook_pool, 1) > 0 &&
        existing_hook_pool.base != 0 && existing_hook_pool.size != 0) {
        g_exec_pool = reinterpret_cast<void *>(existing_hook_pool.base);
        LOGI("WxShadow ART backend: reusing existing hook engine pool base={} size={}",
             g_exec_pool, existing_hook_pool.size);
    } else {
        const uintptr_t libart_hint = FindLibartExecutableBase();
        g_exec_pool = hook_mmap_near(reinterpret_cast<void *>(libart_hint), kInitialPoolSize);
        LOGI("WxShadow ART trace: initial exec pool mmap near libart={} base={} size={} pid={}",
             reinterpret_cast<void *>(libart_hint), g_exec_pool, kInitialPoolSize, getpid());
        if (g_exec_pool == MAP_FAILED) {
            g_exec_pool = nullptr;
            LOGE("WxShadow ART backend: executable pool allocation failed");
            return false;
        }
        SetHookExecPoolVmaName(g_exec_pool, kInitialPoolSize);
        if (hook_engine_init(g_exec_pool, kInitialPoolSize) != 0) {
            munmap(g_exec_pool, kInitialPoolSize);
            g_exec_pool = nullptr;
            LOGE("WxShadow ART backend: hook_engine_init failed");
            return false;
        }
    }
    // Source reference: rustFrida art_method/bridge.rs::find_thread_current and
    // art_thread.rs::probe_art_thread_spec. Failure leaves routing available
    // with the same fail-open stack-check semantics used by rustFrida.
    g_thread_current = reinterpret_cast<ThreadCurrentFn>(
        ResolveArtSymbol("_ZN3art6Thread7CurrentEv"));
    g_thread_layout_found = ProbeArtThreadLayout(env);
    if (!g_thread_current) {
        LOGW("WxShadow ART stack guard: art::Thread::Current unavailable");
    }
    g_art_stealth_mode = SelectArtStealthMode();
    hook_set_stealth_mode(ArtStealthFlag());
    DiscoverClassLinkerTrampolines(env);
    LogArtEntryDiagnostic("base-ready",
                          reinterpret_cast<uintptr_t>(
                              ResolveArtSymbol("_ZN3art7Runtime15DisableVerifierEv")));
    g_base_initialized = true;
    LOGI("WxShadow ART bootstrap base ready: stealth={} ArtMethod size={} flags={} data={} "
         "entry={} jni={} thread_current={} thread_layout={}",
         ArtStealthModeName(g_art_stealth_mode), g_layout.method_size,
         g_layout.access_flags_offset, g_layout.data_offset, g_layout.entry_offset,
         g_layout.jni_trampoline, reinterpret_cast<void *>(g_thread_current),
         g_thread_layout_found);
    return true;
}

bool InitializeBootstrapImpl(JNIEnv *env) {
    if (g_bootstrap_initialized) return true;
    // Timing reference: rawVector installs ActivityThread.attach before attach runs and loads
    // modules only after chain.proceed() returns. Hook semantics reference: rustFrida's
    // art_controller.rs Layer 1 shared router is installed on demand for an internal entry.
    // Keep the pre-attach window limited to layout and executable-pool discovery; installing a
    // process-wide interpreter router here would route module class loading through the full ART
    // matrix before rawVector's post-attach module boundary.
    g_bootstrap_initialized = true;
    LOGI("WxShadow ART bootstrap ready: shared_interpreter=deferred");
    return true;
}

bool EnsureInterpreterRouter(JNIEnv *env) {
    std::call_once(g_interpreter_router_once, [env] {
        g_interpreter_router_initialized = InitializeSharedLibartRouter(
            "quick_to_interpreter_bridge", "art_quick_to_interpreter_bridge", env,
            &g_interpreter_bridge_router);
    });
    return g_interpreter_router_initialized;
}


bool CompleteInitializationImpl(JNIEnv *env) {
    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    if (g_initialized) return true;
    const auto disable_verifier = reinterpret_cast<uintptr_t>(
        ResolveArtSymbol("_ZN3art7Runtime15DisableVerifierEv"));
    LOGI("[art-router-trace] DisableVerifier addr={} mapped_exec={} art_module={}",
         reinterpret_cast<void *>(disable_verifier),
         disable_verifier != 0 && IsMappedExecutable(disable_verifier),
         disable_verifier != 0 && IsInArtModule(disable_verifier));
    // Source reference: rustFrida art_controller.rs Layer 1 skips shared
    // generic-JNI and Nterp unless Java.setStealth(1/2) explicitly selected a
    // stealth mode before ART hooks are installed. Vector's auto mode only
    // opts per-method patching into wxshadow; it does not imply process-wide
    // hot-stub routing.
    const bool use_shared_jni_nterp_layer1 = UseSharedJniNterpLayer1();
    uint64_t stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const bool shared_jni_ready = use_shared_jni_nterp_layer1 && InitializeSharedLibartRouter(
        "quick_generic_jni_trampoline", "art_quick_generic_jni_trampoline", env,
        &g_jni_router);
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=shared_jni enabled={} ready={} elapsedUs={}",
             use_shared_jni_nterp_layer1, shared_jni_ready,
             ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const bool shared_nterp_ready =
        use_shared_jni_nterp_layer1 && InitializeSharedNterpRouter(env);
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=shared_nterp enabled={} ready={} elapsedUs={}",
             use_shared_jni_nterp_layer1, shared_nterp_ready,
             ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const bool shared_nterp_clinit_ready =
        use_shared_jni_nterp_layer1 && InitializeSharedNterpWithClinitRouter(env);
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=shared_nterp_clinit enabled={} ready={} elapsedUs={}",
             use_shared_jni_nterp_layer1, shared_nterp_clinit_ready,
             ArtPerfElapsedUs(stage_start));
    }
    if (!use_shared_jni_nterp_layer1) {
        LOGI("WxShadow ART controller: shared JNI/Nterp Layer1 skipped stealth={} auto={} "
             "source={} value={}",
             ArtStealthModeName(g_art_stealth_mode), g_art_stealth_auto_selected,
             g_art_stealth_source.c_str(), g_art_stealth_value.c_str());
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const bool shared_interpreter_ready = EnsureInterpreterRouter(env);
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=shared_interpreter ready={} elapsedUs={}",
             shared_interpreter_ready, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const bool shared_resolution_ready = InitializeSharedLibartRouter(
        "quick_resolution_trampoline", "art_quick_resolution_trampoline", env,
        &g_resolution_router);
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=shared_resolution ready={} elapsedUs={}",
             shared_resolution_ready, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const size_t do_call_count = InitializeDoCallLayer();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=do_call count={} elapsedUs={}",
             do_call_count, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const size_t decode_guard_ready = InitializeDecodeGcMasksGuard();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=decode_gc_masks_guard ready={} elapsedUs={}",
             decode_guard_ready, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const size_t walkstack_probe_ready = InitializeWalkStackProbe();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=walkstack_probe ready={} elapsedUs={}",
             walkstack_probe_ready, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    const size_t gc_hook_count = InitializeGcSyncLayer();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=gc_sync count={} elapsedUs={}",
             gc_hook_count, ArtPerfElapsedUs(stage_start));
    }
    // The OAT-header replacement path is required by the system_server
    // replacement lifecycle. In ordinary apps it adds process-wide ART metadata
    // probes and inline patches during the first attach, exactly where MIUI's
    // startup watchdog may request a heap snapshot. Keep app startup on the
    // per-method router; install the heavier guard only in system_server.
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    // system_server installs this to keep malloc-backed replacement methods out of
    // the OAT lookup. App processes install the same hook for diagnostics only
    // when the frame probe is on: OnGetOatQuickMethodHeader then logs a method
    // pointer that landed inside the calling thread's stack, and otherwise passes
    // straight through, so app behaviour equals "not hooked".
    g_oat_header_diagnostic_only = !g_is_system_process;
    const bool oat_header_ready =
        (g_is_system_process || FrameProbeEnabled()) && InitializeOatHeaderLayer();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=oat_header ready={} elapsedUs={}",
             oat_header_ready, ArtPerfElapsedUs(stage_start));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    // rustFrida art_controller.rs:1022-1029：该补丁是**无条件**调用的（在 lean /
    // skip_global_sync_hooks 门控**之外**）。原因（rf 注释原文）："libart.so 内联了
    // GetOatQuickMethodHeader 的 data_!=-1 检查，hook_replace 只拦截非内联调用。
    // 内联点需要单独 patch"。
    //
    // 此前本行条件是 oat_header_ready（= g_is_system_process && InitializeOatHeaderLayer()），
    // 导致 **app 进程（hunter 等）的内联点未打补丁** → ART 走栈/oat 查询落到
    // FindOatMethodFor 失败 → LOG(FATAL) → 拼 PrettyMethod() 时对 NULL DexFile 取址
    // SEGV（tombstone_09：fault addr 0x48，栈 AppendPrettyMethod ← PrettyMethod ←
    // GetOatQuickMethodHeader ← WalkStack ← ConcurrentCopying::ThreadFlipVisitor）。
    //
    // 该补丁是一次性二进制 patch（不是 hook，不改变 stealth/不引入 wxshadow 页翻转），
    // 因此在 app 进程执行是安全的。exclude 传 GetOatQuickMethodHeader **本体**地址，
    // 避免模式扫描把本体当成内联副本 patch。
    uint64_t oat_patch_exclude = g_oat_header_hook_target;
    if (oat_patch_exclude == 0) {
        for (const char *name : {"_ZN3art9ArtMethod23GetOatQuickMethodHeaderEm",
                                 "_ZN3art9ArtMethod23GetOatQuickMethodHeaderEj"}) {
            const auto addr =
                reinterpret_cast<uintptr_t>(ResolveArtSymbol(name)) & kPacStripMask;
            if (addr) {
                oat_patch_exclude = addr;
                break;
            }
        }
    }
    const int oat_inline_patched =
        oat_patch_exclude != 0 ? hook_patch_inlined_oat_header_checks(oat_patch_exclude) : 0;
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=oat_inline_patch count={} elapsedUs={}",
             oat_inline_patched, ArtPerfElapsedUs(stage_start));
    }
    if (oat_patch_exclude == 0) {
        LOGW("WxShadow ART inlined OAT header patch skipped: GetOatQuickMethodHeader unavailable");
    } else if (oat_inline_patched < 0) {
        LOGW("WxShadow ART inlined OAT header patch failed: {} exclude={}",
             oat_inline_patched, reinterpret_cast<void *>(oat_patch_exclude));
    } else {
        LOGI("WxShadow ART inlined OAT header patch ready: count={} exclude={}",
             oat_inline_patched, reinterpret_cast<void *>(oat_patch_exclude));
    }
    stage_start = perf_start != 0 ? ArtPerfNowNs() : 0;
    // 不再限定 system_server：rustFrida 对所有进程都装
    // （install_managed_implicit_suspend_guard / install_walkstack_sigsegv_guard）。
    // 此前只在 system_server 安装，导致 app 进程（hunter 等）既没有 implicit-suspend
    // 接管、也没有伪造帧故障的被动修复层：
    //   - ART 的 implicit-suspend 触发信号（kill(tid, SIGSEGV)，si_code = SI_USER）
    //     无人接管 → 冷启期 3 次里有 2 次以 code 0 (SI_USER) SIGSEGV 收场；
    //   - 我方伪造帧触发的 NULL OatQuickMethodHeader 故障不再被 TryHandleNullOatHeader 修回。
    // 本函数本身只在 ART hook 层真正初始化的进程里跑，所以不会波及无关进程；
    // VECTOR_NO_SIGNAL_GUARD=1 可关闭。
    const bool signal_guard_ready = InitializeSignalGuards();
    if (stage_start != 0) {
        LOGI("[perf] art_controller component=signal_guard ready={} elapsedUs={}",
             signal_guard_ready, ArtPerfElapsedUs(stage_start));
    }
    g_initialized = true;
    LOGI("WxShadow ART backend ready: stealth={} ArtMethod size={} flags={} data={} entry={} jni={} "
         "thread_current={} thread_layout={} shared_jni_nterp_layer1={} "
         "shared_nterp={} shared_nterp_clinit={} "
         "shared_jni={} shared_interpreter={} shared_resolution={} do_call={} gc_sync={} "
         "oat_header={} oat_inline_patch={} signal_guard={}",
         ArtStealthModeName(g_art_stealth_mode), g_layout.method_size,
         g_layout.access_flags_offset, g_layout.data_offset, g_layout.entry_offset,
         g_layout.jni_trampoline, reinterpret_cast<void *>(g_thread_current),
         g_thread_layout_found,
         use_shared_jni_nterp_layer1, shared_nterp_ready, shared_nterp_clinit_ready,
         shared_jni_ready, shared_interpreter_ready, shared_resolution_ready,
         do_call_count, gc_hook_count, oat_header_ready, oat_inline_patched, signal_guard_ready);
    if (perf_start != 0) {
        LOGI("[perf] art_controller end stealth={} elapsedUs={}",
             ArtStealthModeName(g_art_stealth_mode), ArtPerfElapsedUs(perf_start));
    }
    {
        // 诊断：导出 ART method router 命中/未命中状态，用于排查 AppAttachHooker 不触发。
        // 未命中时 router 会记录最后一个 x0（进入桥的 ArtMethod）与 miss 计数；
        // 若此进程从未有解释方法经过桥，miss=0，说明桥补丁未生效。
        uint64_t miss = 0, last_miss_x0 = 0, hit = 0, last_hit_x0 = 0;
        uint64_t qh = 0, rh = 0, dch = 0, last_dc_x0 = 0;
        hook_art_router_get_debug(&last_miss_x0, &miss);
        hook_art_router_get_hit_debug(&hit, &last_hit_x0);
        hook_art_router_get_route_stats(&qh, &rh, &dch, &last_dc_x0, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr);
        LOGI("WxShadow ART router debug: miss={} last_miss_x0={} hit={} last_hit_x0={} "
             "quk_hit={} repl_hit={} docall_hit={} last_docall_x0={}",
             miss, reinterpret_cast<void *>(last_miss_x0), hit,
             reinterpret_cast<void *>(last_hit_x0), qh, rh, dch,
             reinterpret_cast<void *>(last_dc_x0));

        /* 诊断（2026-09-13）：miss 探针按属性开启。
         * 用于定位「hook 安装报 success 但回调零调用」——探针会在 router 未命中
         * 时打印进入的 X0 及其与 router 表 key 的对比，从而判定调用是否根本
         * 没到 router，或到了但 key 不匹配（副本 ArtMethod）。
         * 默认关闭；打开时仅消耗有限预算，耗尽后热路径回到原状。 */
        std::string probe_prop;
        if (ReadArtStealthProperty("debug.vector.art.router_probe", &probe_prop) &&
            probe_prop != "0") {
            hook_art_router_set_miss_probe_budget(ART_ROUTER_PROBE_DEFAULT_BUDGET);
            LOGI("WxShadow ART router miss probe armed: budget={}",
                 ART_ROUTER_PROBE_DEFAULT_BUDGET);
        }
    }
    return true;
}

#endif  // defined(__aarch64__)

}  // namespace

bool IsWxShadowStealthMode() {
#if defined(__aarch64__)
    return g_art_stealth_mode == ArtStealthMode::WxShadow;
#else
    return false;
#endif
}

void SetSystemProcess(bool is_system) {
#if defined(__aarch64__)
    g_is_system_process = is_system;
    LOGI("WxShadow ART process context: system={}", is_system);
#else
    (void)is_system;
#endif
}

void SetManagerProcess(bool is_manager) {
#if defined(__aarch64__)
    g_is_manager_process = is_manager;
    LOGI("WxShadow ART manager process context: manager={}", is_manager);
#else
    (void)is_manager;
#endif
}

// ---------------------------------------------------------------------------
// Frame-probe diagnostic (debug.vector.art.frame_probe=1)
//
// The stack walker reaches a hooked method through a chain that has exactly one
// geometric contract:
//   GenericJNI frame (224 B, method = replacement, native)
//     <- published as managed_stack_.top_quick_frame_
//   our thunk frame (368 B = 16 B method slot + 352 B HookContext), directly
//     below it, because the trampoline calls through with sp == its own frame.
// If ART ever advances from the JNI frame by anything other than 224, or from
// our frame by anything other than 368, the "method" it reads next is one of
// the registers we saved, not an ArtMethod. Every observed crash is consistent
// with that: a non-native method with a garbage dex_method_index_.
//
// This probe therefore records, per thunk entry, the four values needed to
// verify the contract, plus whether top_quick_frame really is frame + 368.
// It is off unless the property is set, and it never allocates or takes locks.
// ---------------------------------------------------------------------------
constexpr size_t kNativeThunkMethodSlotSize = 16;
constexpr size_t kNativeThunkFrameSize = 368;
// The ART router's frame is a SaveRefsAndArgs shape: SUB SP,#0xE0 (224).
constexpr size_t kRouterFrameSize = 224;
// router frame +136..+216 are the slots FAKE_CORE_SPILL_MASK declares (x20..x28)
// plus the return-PC slot. Distinct from the inline thunk's 352/280 layout.
constexpr size_t kRouterSlotWindowOffset = 136;
constexpr size_t kFrameProbeEntries = 32;
// 7 scalars + 8 ctx/route scalars + tail[8] + reserved + router_slots[11] + 8 chain hops of 7
constexpr size_t kFrameProbeStride = 91;
// ManagedStack::link_ follows top_quick_frame_ on API >= 23 (rustFrida
// art_thread.rs get_managed_stack_spec). The offset is relative to the
// ManagedStack, which is embedded at Thread+managed_stack_offset.
constexpr size_t kManagedStackLinkOffset = 8;
// How many ManagedStack fragments to follow before giving up. A hook nested a
// few levels deep is normal; a long chain means we are chasing garbage.
constexpr size_t kMaxManagedStackFragments = 8;

// One fragment of the walker's chain, captured in the same ring entry.
struct FrameChainHop {
    uint64_t fragment;       // ManagedStack* itself
    uint64_t top_raw;        // fragment->top_quick_frame_ (tagged)
    uint64_t frame;          // top_raw with bit0 cleared
    uint64_t frame_method;   // *(frame) - the ArtMethod the walker would use
    uint64_t frame_size;     // size ART advances by, when derivable; else 0
    uint64_t flags;          // bit0: readable, bit1: equals our baked slot
    uint64_t reserved;
};

struct FrameProbeEntry {
    // --- what the probe could always see -------------------------------------
    uint64_t seq;
    uint64_t frame_base;      // the frame this entry is about (see flags bit4/bit5)
    uint64_t top_quick_frame; // ManagedStack::top_quick_frame_ at call time
    uint64_t method_slot;     // RAW u64 at frame_base -- never synthesised
    uint64_t jni_frame_expected;
    uint64_t tid;
    uint64_t flags;
    // --- route / HookContext identity ---------------------------------------
    // The old probe inferred the frame from `ctx - 16` on every route. That is
    // only valid for the stack-resident contexts; the ART router keeps its ctx
    // in a per-thread TLS table, where `ctx - 16` is unrelated memory. These
    // fields record the ctx itself plus the route, so slot == 0 can finally be
    // attributed instead of guessed (handover 6.1).
    uint64_t ctx;             // HookContext* handed to the callback
    uint64_t ctx_sp;          // ctx->sp
    uint64_t ctx_pc;          // ctx->pc
    uint64_t ctx_delta;       // ctx_sp - ctx  (352 = stack frame, other = TLS ctx)
    uint64_t ctx_x0;          // ctx->x[0]: the method dispatch routes on
    uint64_t ctx_x0_class;    // ProbeMethodClass bits for ctx_x0
    uint64_t slot_class;      // ProbeMethodClass bits for method_slot
    // Router-only: the real 224-byte frame sp, 0 on every other route.
    uint64_t router_frame;
    // Frame tail captured AT CALL TIME. Reading these afterwards is useless:
    // the stack is already unwound and reused (verified on device). These are
    // frame+0x160..0x1a0, i.e. the region ART would land on if it advances by
    // 384 bytes instead of our declared 368.
    uint64_t tail[8];
    uint64_t reserved;
    // Router frame+136 .. +216: exactly the slots FAKE_CORE_SPILL_MASK declares
    // (x20..x28) plus the return-PC slot at +216. Captured at call time so the
    // spill-mask A/B can be judged from data instead of from crash shape.
    uint64_t router_slots[11];
    // The walker's ManagedStack chain, captured at call time. This is the part
    // that matters now: the probe already proved our own [sp+0] holds a valid
    // ArtMethod, yet the crashes dereference a stack address as a method, so the
    // bad frame is somewhere in this chain and not in our slot.
    FrameChainHop chain[kMaxManagedStackFragments];
};

static_assert(sizeof(FrameProbeEntry) == kFrameProbeStride * sizeof(uint64_t),
              "probe entry must stay stride-aligned");

// Externally visible so a post-mortem reader can find it without symbols.
alignas(64) FrameProbeEntry g_native_thunk_frame_probe[kFrameProbeEntries]{};
std::atomic<size_t> g_native_thunk_frame_probe_count{0};
std::atomic<bool> g_frame_probe_state{false};  // false = not yet checked

uintptr_t CurrentArtThreadForProbe() {
    uintptr_t tls = 0;
#if defined(__aarch64__)
    asm volatile("mrs %0, tpidr_el0" : "=r"(tls));
#else
    tls = reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
#endif
    if (!tls) return 0;
    return *reinterpret_cast<volatile const uintptr_t *>(tls + 7 * sizeof(uintptr_t)) &
           kPacStripMask;
}

bool FrameProbeEnabled() {
    const bool cached = g_frame_probe_state.load(std::memory_order_relaxed);
    if (cached) return true;
    // Enabled by default because this build is a diagnostic drop: the whole point
    // is that the geometry is recorded before a crash takes the process away.
    // debug.vector.art.frame_probe=0 turns it off again.
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.vector.art.frame_probe", value) > 0 && value[0] == '0') {
        return false;
    }
    g_frame_probe_state.store(true, std::memory_order_relaxed);
    LOGI("WxShadow ART frame probe enabled (set debug.vector.art.frame_probe=0 to disable)");
    return true;
}

// ---------------------------------------------------------------------------
// What is in a slot that ART is about to treat as an ArtMethod*?
//
// The crashes in this family are all "ART read a non-ArtMethod out of a frame
// slot". Recording the raw value is not enough to act on it: 0x0, a stack
// address, a Thread* and a real method look the same in a log of hex numbers.
// These bits classify the value cheaply. Crucially they use only caches built
// once (LoadReadableRanges for readability, hook_is_in_exec_pool for our own
// code, a call_once module range for libart) -- no fopen("/proc/self/maps") --
// because this runs on the GC thread's stack-walk path.
// ---------------------------------------------------------------------------
constexpr uint64_t kProbeMethodZero = 1u << 0;
constexpr uint64_t kProbeMethodReadable = 1u << 1;
constexpr uint64_t kProbeMethodInPool = 1u << 2;
constexpr uint64_t kProbeMethodNullEntry = 1u << 3;
constexpr uint64_t kProbeMethodInArt = 1u << 4;
constexpr uint64_t kProbeMethodStackLike = 1u << 5;
// The cache could not answer for this address (truncated maps, or a mapping made
// after the cache was built). Distinct from "not readable" on purpose: a value
// classified with this bit set tells us nothing about whether ART could use it.
constexpr uint64_t kProbeMethodOutOfCache = 1u << 6;

uint64_t ClassifyProbeMethod(uintptr_t value, uintptr_t anchor) {
    if (value == 0) return kProbeMethodZero;
    const uintptr_t v = value & kPacStripMask;
    uint64_t bits = 0;
    switch (ProbeReadability(v)) {
    case ReadableVerdict::Yes:
        bits |= kProbeMethodReadable;
        break;
    case ReadableVerdict::OutOfCache:
        bits |= kProbeMethodOutOfCache;
        break;
    case ReadableVerdict::No:
        break;
    }
    if (hook_is_in_exec_pool(v)) bits |= kProbeMethodInPool;
    const auto &art = CachedArtModuleRange();
    if (art.valid && v >= art.lo && v < art.hi) bits |= kProbeMethodInArt;
    // Same "this thread's stack" test the chain walk uses. A method that lands
    // here is the tombstone_10 shape, never legitimate.
    if (anchor != 0) {
        const uintptr_t delta = v > anchor ? v - anchor : anchor - v;
        if (delta < (8u * 1024u * 1024u)) bits |= kProbeMethodStackLike;
    }
    // entry_ lives at data_+8 once the layout probe has run. Check the exact
    // address being read, not just `v`: a method at the end of a mapping would
    // otherwise fault here, and this runs on the GC thread's stack-walk path.
    const size_t entry_offset = g_layout.entry_offset;
    if (entry_offset != 0 && entry_offset + sizeof(uintptr_t) <= 64 &&
        IsCachedReadable(v + entry_offset)) {
        const auto entry = *reinterpret_cast<volatile const uintptr_t *>(v + entry_offset);
        if (entry == 0) bits |= kProbeMethodNullEntry;
    }
    return bits;
}

// ============================================================================
// Forensic: what does ART's OWN stack walk see, at the moment it walks?
//
// The crashes converge on one sequence inside art::StackVisitor::WalkStack
// (device libart, no symbols needed -- the instructions are enough):
//
//   0x647a48  ldr  x8,  [x25]          ; ManagedStack fragment->top_quick_frame_
//   0x647a4c  ands x10, x8, #~1        ; drop TaggedQuickFrame bit 0 -> frame
//   0x647a50  stp  x10, xzr, [x19,#0x20] ; cur_quick_frame_ = frame
//   0x647a58  ldr  x24, [x10]          ; *(frame) = the frame's ArtMethod*
//   0x647a5c  ldr  w20, [x24, #0x4]    ; access_flags_   <-- fault addr 0x4
//
// Measured camps (same build, same app):
//   0x647a5c with x24 = 0x7fe0000000000090  (reproduced twice, identical pc+x0)
//   0x284240 (PrettyMethod) with x24 = art::Thread* of this very thread
//
// So the value ART reads out of a frame's method slot is not an ArtMethod. The
// open question is WHICH ManagedStack fragment supplies that frame -- ours, or
// one ART advanced onto. Hooking WalkStack and dumping the chain at entry answers
// that directly, because the chain at entry is the chain the loop then walks.
//
// Safety: attach with on_enter only, so the hook tail-jumps to the original and
// no extra frame exists while ART walks. The callback only reads memory through
// the already-cached maps (never fopen) and never calls back into ART.
// ============================================================================
std::atomic<uint32_t> g_walkstack_probe_count{0};

// ---------------------------------------------------------------------------
// Safe read for the forensic probe.
//
// The probe used to gate every dereference on the cached /proc/self/maps
// snapshot. That cache is taken ONCE, but hunter creates threads throughout its
// lifetime and every new thread adds a stack mapping afterwards -- measured 197
// stack mappings in one of these processes. A stack that appeared after the
// snapshot is absent from it, so ProbeReadability() answered "no" and the probe
// bailed at its first guard. Result: 20 of 20 logged probes were
// "BAIL reason=3" and not a single healthy walk was ever recorded, i.e. the
// instrument was blind to most real walks.
//
// process_vm_readv on ourselves is the correct primitive here: on an unmapped
// source it returns -1/EFAULT instead of raising SIGSEGV, so it needs no map
// bookkeeping at all and cannot go stale. It costs a syscall per word, which is
// fine for a probe that already logs.
// ---------------------------------------------------------------------------
const char *DescribeProbeMethod(uintptr_t value, uintptr_t anchor) {
    const uint64_t bits = ClassifyProbeMethod(value, anchor);
    if (bits & kProbeMethodZero) return "zero";
    if (bits & kProbeMethodOutOfCache) return "out-of-cache";
    if (!(bits & kProbeMethodReadable)) return "UNREADABLE";
    if (bits & kProbeMethodInPool) return "vector-pool";
    if (bits & kProbeMethodNullEntry) return "null-entry";
    if (bits & kProbeMethodInArt) return "libart-data";
    if (bits & kProbeMethodStackLike) return "STACK";
    return "ok";
}

void OnWalkStackEnter(HookContext *ctx, void *) {
#if defined(__aarch64__)
    // Count every entry BEFORE any guard. A probe that can return at five
    // different places without leaving a trace is indistinguishable from a
    // probe that never ran -- that ambiguity cost a whole device round: the
    // truncated maps cache made this function return at the first check, and the
    // resulting silence looked exactly like "the hook is not installed".
    const uint32_t entered = g_walkstack_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;

    // One line per distinct early-return reason, once each.
    auto bail = [entered, ctx](int reason) {
        static std::atomic<uint32_t> seen{0};
        const uint32_t mask = 1u << reason;
        uint32_t cur = seen.load(std::memory_order_relaxed);
        while (!(cur & mask) &&
               !seen.compare_exchange_weak(cur, cur | mask, std::memory_order_relaxed)) {
        }
        if (cur & mask) return;
        LOGW("WxShadow ART WalkStack probe BAIL reason={} n={} ctx={} x0={} tid={}",
             reason, entered, reinterpret_cast<void *>(ctx),
             reinterpret_cast<void *>(ctx ? ctx->x[0] : 0), static_cast<long>(gettid()));
    };

    if (!ctx) { bail(1); return; }
    const uintptr_t self = ctx->x[0];
    if (!self) { bail(2); return; }

    // All dereferences below go through SafeReadU64 rather than the maps
    // snapshot: the snapshot cannot know about stacks created after it was
    // taken, and that is where most visitors live (see SafeReadU64 comment).
    uintptr_t thread = 0;
    if (!SafeReadU64(self + 0x8, &thread)) { bail(3); return; }
    if (!thread) { bail(4); return; }
    if (!g_thread_layout.valid) { bail(5); return; }
    const uintptr_t thread_frag = thread + g_thread_layout.managed_stack_offset;
    uintptr_t probe_word = 0;
    if (!SafeReadU64(thread_frag, &probe_word) ||
        !SafeReadU64(thread_frag + kManagedStackLinkOffset, &probe_word)) {
        bail(6);
        return;
    }
    uintptr_t fragment = thread_frag;

    // Collect first, then decide whether to log: logging during a GC checkpoint
    // is a syscall on the GC thread, so only do it when something is wrong.
    uintptr_t frags[kMaxManagedStackFragments] = {};
    uintptr_t frames[kMaxManagedStackFragments] = {};
    uintptr_t methods[kMaxManagedStackFragments] = {};
    uintptr_t raw_tops[kMaxManagedStackFragments] = {};
    uintptr_t shadows[kMaxManagedStackFragments] = {};
    uintptr_t nexts[kMaxManagedStackFragments] = {};
    size_t hops = 0;
    // A hop is only BAD when it is a real quick frame whose method slot holds
    // something ART cannot use. Two cases that look alarming but are NOT bad,
    // both now confirmed against the device disassembly, and both of which used
    // to consume the 16-entry rate-limit budget without cause:
    //   * top_quick_frame_ == 0: WalkStack does `ands x10,x8,#~1` then `b.eq
    //     0x647aa0`, i.e. it deliberately falls through to the transitions /
    //     shadow-frame case. That is a legal state, not corruption.
    //   * a method whose entry_ == 0 but which is otherwise a readable ArtMethod
    //     in a boot image (measured 0x70e06c60: declaring=0, flags=0, idx=-1,
    //     entry=0). Such placeholders exist; they are worth reporting as their own
    //     class, not as the crash signature.
    bool bad = false;
    bool saw_zero_frame = false;
    bool bad_frame_unmapped = false;
    for (size_t i = 0; i < kMaxManagedStackFragments; ++i) {
        if (!fragment) break;
        uintptr_t raw = 0;
        uintptr_t next = 0;
        uintptr_t shadow = 0;
        if (!SafeReadU64(fragment, &raw) ||
            !SafeReadU64(fragment + kManagedStackLinkOffset, &next) ||
            !SafeReadU64(fragment + 8, &shadow)) {
            break;
        }
        const uintptr_t frame = raw & ~uintptr_t{1};
        frags[hops] = fragment;
        raw_tops[hops] = raw;
        frames[hops] = frame;
        // top_shadow_frame_ sits right after top_quick_frame_ in ManagedStack.
        shadows[hops] = shadow & kPacStripMask;
        nexts[hops] = next & kPacStripMask;
        uintptr_t method = 0;
        if (frame) SafeReadU64(frame, &method);
        methods[hops] = method;
        ++hops;

        if (frame == 0) {
            // Legal: only shadow frames, or a transition frame. Not corruption.
            saw_zero_frame = true;
            const uintptr_t next_clean = next & kPacStripMask;
            if (!next_clean || next_clean == fragment) break;
            fragment = next_clean;
            continue;
        }
        const uint64_t bits = ClassifyProbeMethod(method, frame);
        // A quick frame must name a readable ArtMethod. Anything else -- slot 0,
        // unreadable, or outside every known mapping -- is the crash signature.
        const bool unusable = method == 0 || !(bits & kProbeMethodReadable);
        if (unusable) bad = true;
        // The crash signature seen on 3289 is stronger than "bad method": the
        // frame pointer ITSELF is not in any readable mapping (frame_map=-), so
        // ART is holding something that was never a quick frame. Record those
        // separately -- a frame that is merely corrupt is a different bug from a
        // slot that points outside mapped memory entirely.
        if (ProbeReadability(frame) == ReadableVerdict::No) bad_frame_unmapped = true;
        const uintptr_t next_clean = next & kPacStripMask;
        if (!next_clean || next_clean == fragment) break;
        fragment = next_clean;
    }
    if (!bad && saw_zero_frame) {
        // Everything else was fine and only the legal zero-frame case appeared.
        bad = false;
    }
    const bool suspicious = bad;

    // entered is this entry's number, already taken before the guards.
    const uint32_t n = entered;
    if (!suspicious) {
        // Count healthy walks on their own. Using the entry counter here was a
        // mistake: n keeps growing for the life of the process, so `n <= 3` is
        // only ever true in the first microseconds and the healthy path stayed
        // silent forever -- indistinguishable from "the probe never sees a good
        // walk", which is exactly the wrong conclusion to draw from silence.
        static std::atomic<uint32_t> healthy{0};
        const uint32_t h = healthy.fetch_add(1, std::memory_order_relaxed) + 1;
        if (h <= 3 || (h % 4096u) == 0) {
            LOGI("WxShadow ART WalkStack probe: healthy walk h={} n={} tid={} hops={} "
                 "frame0={} method0={} class0={} zero_frames={}",
                 h, n, static_cast<long>(gettid()), static_cast<int>(hops),
                 reinterpret_cast<void *>(frames[0]), reinterpret_cast<void *>(methods[0]),
                 DescribeProbeMethod(methods[0], frames[0]),
                 saw_zero_frame ? "yes" : "no");
        }
        return;
    }

    // Suspicious: this walk is about to hand ART a non-ArtMethod. Rate limited,
    // because a corrupted chain is walked once per GC checkpoint.
    static std::atomic<uint32_t> reported{0};
    const uint32_t r = reported.fetch_add(1, std::memory_order_relaxed) + 1;
    if (r > 16) return;
    LOGW("WxShadow ART WalkStack probe BAD CHAIN: n={} r={} tid={} hops={} thread={} "
         "thread_frag={} frame_unmapped={}",
         n, r, static_cast<long>(gettid()), static_cast<int>(hops),
         reinterpret_cast<void *>(thread),
         reinterpret_cast<void *>(thread + g_thread_layout.managed_stack_offset),
         bad_frame_unmapped ? "YES" : "no");
    uintptr_t prev_frame = 0;
    for (size_t i = 0; i < hops; ++i) {
        // frag / next are what the previous revision lacked: without them it was
        // impossible to say whether the bad frame belonged to a fragment ART was
        // handed, or was one it advanced onto from a good fragment.
        LOGW("WxShadow ART WalkStack probe   hop[{}] frag={} top_raw={} frame={} adv={} "
             "shadow={} link={} method={} class={} at_top={}",
             i, reinterpret_cast<void *>(frags[i]),
             reinterpret_cast<void *>(raw_tops[i]),
             reinterpret_cast<void *>(frames[i]),
             i ? reinterpret_cast<void *>(frames[i] - prev_frame) : nullptr,
             reinterpret_cast<void *>(shadows[i]),
             reinterpret_cast<void *>(nexts[i]),
             reinterpret_cast<void *>(methods[i]),
             DescribeProbeMethod(methods[i], frames[i]),
             (frames[i] && frames[i] == (raw_tops[i] & ~uintptr_t{1})) ? "yes" : "no");
        // 16 words instead of 4, every word labelled with the mapping that owns
        // it. The earlier version printed "0x72969a3c" with no way to tell an OAT
        // entry point from a stack slot or a heap pointer.
        if (frames[i]) {
            char line[512] = {};
            size_t used = 0;
            for (size_t w = 0; w < 16; ++w) {
                uintptr_t v = 0;
                if (!SafeReadU64(frames[i] + w * 8, &v)) break;
                const char *map = MappedNameFor(v);
                const int wrote = snprintf(line + used, sizeof(line) - used, "%s0x%" PRIxPTR "%s",
                                           w ? " " : "", v, map[0] ? map : "-");
                if (wrote <= 0 || used + (size_t)wrote >= sizeof(line)) break;
                used += (size_t)wrote;
            }
            LOGW("WxShadow ART WalkStack probe     frame+0..15={}", line);
        }
        prev_frame = frames[i];
    }
    // When the frame pointer is not in any mapping, the fragment that carries it
    // is the only place the writer left a trace. Dump the fragment itself: its
    // top_quick_frame_/top_shadow_frame_/link_ plus the words before and after,
    // which reveal whether this is a ManagedStack that was partially rewritten
    // (a torn update) rather than a fragment that was never valid.
    if (bad_frame_unmapped) {
        for (size_t i = 0; i < hops; ++i) {
            if (frames[i] == 0) continue;
            if (ProbeReadability(frames[i]) != ReadableVerdict::No) continue;
            char line[768] = {};
            size_t used = 0;
            for (int w = -4; w < 12; ++w) {
                uintptr_t v = 0;
                if (!SafeReadU64(frags[i] + (intptr_t)w * 8, &v)) continue;
                const char *map = MappedNameFor(v);
                const int wrote = snprintf(line + used, sizeof(line) - used,
                                           "%s%+d:0x%" PRIxPTR "%s", used ? " " : "", w, v,
                                           map[0] ? map : "-");
                if (wrote <= 0 || used + (size_t)wrote >= sizeof(line)) break;
                used += (size_t)wrote;
            }
            LOGW("WxShadow ART WalkStack probe UNMAPPED FRAME: hop={} frag={} frag_map={} "
                 "bad_frame={} frag[{}..{}]={}",
                 i, reinterpret_cast<void *>(frags[i]),
                 MappedNameFor(frags[i])[0] ? MappedNameFor(frags[i]) : "-",
                 reinterpret_cast<void *>(frames[i]), -4, 11, line);
        }
    }
#else
    (void)ctx;
#endif
}

size_t InitializeWalkStackProbe() {
#if defined(__aarch64__)
    static int attempted = 0;
    if (attempted) return g_walkstack_probe_count.load() ? 1 : 0;
    attempted = 1;

    // Off unless asked for: this sits on every stack walk, including GC
    // checkpoints, so it is a targeted instrument rather than a default-on probe.
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.vector.art.walkstack_probe", prop) <= 0 ||
        prop[0] != '1') {
        LOGI("WxShadow ART WalkStack probe: off (setprop debug.vector.art.walkstack_probe 1)");
        return 0;
    }

    constexpr const char *kSymbol =
        "_ZN3art12StackVisitor9WalkStackILNS0_16CountTransitionsE1EEEvb";
    const auto target = reinterpret_cast<uintptr_t>(ResolveArtSymbol(kSymbol)) & kPacStripMask;
    if (!target || !IsMappedExecutable(target) || !IsInArtModule(target)) {
        LOGW("WxShadow ART WalkStack probe: symbol unresolved target={}",
             reinterpret_cast<void *>(target));
        return 0;
    }
    // on_enter only: the thunk tail-jumps to the original, so ART walks a stack
    // that does not contain this hook. Wrap mode would add a frame below SP and
    // make the observation describe the observer.
    if (hook_attach(reinterpret_cast<void *>(target), OnWalkStackEnter, nullptr, nullptr,
                    ArtStealthFlag()) != HOOK_OK) {
        LOGW("WxShadow ART WalkStack probe: hook_attach failed target={}",
             reinterpret_cast<void *>(target));
        return 0;
    }
    g_walkstack_fn.store(target, std::memory_order_relaxed);
    LOGI("WxShadow ART WalkStack probe ready target={} managed_stack_off={}",
         reinterpret_cast<void *>(target), g_thread_layout.managed_stack_offset);
    return 1;
#else
    return 0;
#endif
}

// Which hook route produced this probe entry. The three shapes differ in frame
// size (224 / 352 / 368) and in whether a method slot exists at all, so every
// log line names the route rather than leaving the reader to guess from numbers.
const char *ProbeRouteName(bool native_entry_route, bool router_route) {
    if (router_route) return "art_router";
    return native_entry_route ? "native_entry" : "trampoline";
}

void ProbeNativeThunkFrame(HookContext *ctx, bool native_entry_route) {
#if defined(__aarch64__)
    if (!ctx) return;
    if (!FrameProbeEnabled()) return;

    // The native trampoline frame starts 16 bytes before the HookContext
    // (reserved method slot). The registered-native-entry route has no such
    // slot, so the base is different.
    //
    // Expected top_quick_frame, MEASURED rather than derived. Two independent
    // drops agree on the same relation, which is why it is trustworthy:
    //     declared 368 -> untagged delta 384 = 368 + 16
    //     declared 384 -> untagged delta 400 = 384 + 16
    // i.e. the caller publishes a frame base 16 bytes above the sp it calls us
    // with (its JNI arg scratch), so the distance includes that 16 on top of
    // our own frame. The first revision omitted it and therefore reported a
    // constant +16 "MISMATCH" that looked like a frame-size bug; it was a
    // probe bug and led to an unnecessary 368->384 change (now reverted).
    // The registered-native-entry route has no JNI trampoline above it, so it
    // carries no extra 16.
    //
    // Where does this ctx actually live? Three shapes reach this probe:
    //   * inline engine: 352-byte stack frame, ctx IS the frame;
    //   * native thunk:  368-byte stack frame, ctx sits after a 16-byte slot;
    //   * ART router quick path: the context is a slot in a per-thread TLS table
    //     and `ctx - 16` points at unrelated TLS memory.
    // The previous revision only knew the first two, so on the router route it
    // read the TLS block and reported `slot=0x0` -- the exact value the handover
    // could not attribute. Ask the engine instead of inferring from an offset.
    const uintptr_t router_frame = hook_art_router_ctx_frame_sp(ctx);
    const bool router_route = router_frame != 0;

    const uintptr_t frame =
        router_route ? router_frame
                     : reinterpret_cast<uintptr_t>(ctx) -
                           (native_entry_route ? 0 : kNativeThunkMethodSlotSize);
    const uintptr_t declared = router_route
                                   ? kRouterFrameSize
                                   : (native_entry_route ? 352 : kNativeThunkFrameSize);
    const uintptr_t caller_gap = (router_route || native_entry_route) ? 0 : 16;
    const uintptr_t expected_jni_frame = frame + declared + caller_gap;
    // Always read the word instead of synthesising 0 for a route: on the
    // registered-native route this is the last saved register before the
    // context, and the classification below says what it actually is.
    const uintptr_t slot = *reinterpret_cast<volatile const uintptr_t *>(frame);
    const uint64_t slot_class = ClassifyProbeMethod(slot, frame);
    const uint64_t ctx_x0_class = ClassifyProbeMethod(ctx->x[0], frame);
    uintptr_t top = 0;
    const uintptr_t thread = CurrentArtThreadForProbe();
    if (thread && g_thread_layout.valid) {
        top = *reinterpret_cast<volatile const uintptr_t *>(
            thread + g_thread_layout.managed_stack_offset);
    }

    // Capture what ART would call the next quick frame: dereference the frame
    // pointer the walker publishes. This is the decisive bit -- it says whether
    // ART is looking at OUR frame at all. If *(top) is not the replacement
    // ArtMethod we baked into [frame+0], then the bogus method the walker
    // reports comes from somewhere else entirely (a different frame, or a frame
    // ART advanced onto), and no amount of fixing our own slot can help.
    //
    // Must NOT call IsReadable() here: it fopen()s /proc/self/maps, and this runs
    // on the GC-thread stack-walk path. Only dereference when the pointer is
    // inside the same region family as our own frame (i.e. the thread's stack),
    // which is where a quick frame can legitimately be.
    uintptr_t top_method = 0;
    if (top) {
        const uintptr_t top_frame = top & ~uintptr_t{1};
        // Same thread stack, within a sane distance above our own frame. That is
        // the only place a quick frame pointing at us can live, and it is mapped
        // by construction because we are running on that stack right now.
        const bool plausible_frame =
            top_frame > frame && (top_frame - frame) < (64u * 1024u);
        if (plausible_frame) {
            top_method = *reinterpret_cast<volatile const uintptr_t *>(top_frame);
        }
    }

    const size_t index = g_native_thunk_frame_probe_count.fetch_add(
                             1, std::memory_order_relaxed) % kFrameProbeEntries;
    auto &entry = g_native_thunk_frame_probe[index];
    entry.seq = index;
    entry.frame_base = frame;
    entry.top_quick_frame = top;
    entry.method_slot = slot;
    entry.jni_frame_expected = expected_jni_frame;
    entry.tid = static_cast<uint64_t>(gettid());
    entry.ctx = reinterpret_cast<uint64_t>(ctx);
    entry.ctx_sp = ctx->sp;
    entry.ctx_pc = ctx->pc;
    entry.ctx_delta = ctx->sp - reinterpret_cast<uint64_t>(ctx);
    entry.ctx_x0 = ctx->x[0];
    entry.ctx_x0_class = ctx_x0_class;
    entry.slot_class = slot_class;
    entry.router_frame = router_frame;
    // flags bit0: top_quick_frame agrees with the frame size we declare
    // flags bit1: thread/ManagedStack readable
    // flags bit2: registered-native-entry route (no method slot, 352 by design)
    // flags bit3: the walker's own frame holds our replacement method (i.e. ART
    //             is looking at THIS frame, not some other one)
    // flags bit4: ctx is a router TLS context, so frame_base came from the
    //             engine's TLS table, NOT from ctx-16
    // flags bit5: ctx_sp - ctx == 352, i.e. a stack-resident context whose
    //             layout matches emit_save_hook_context
    // flags bit0 compares the UNTAGGED frame pointer: TopQuickFrame carries a
    // tag in bit0 (and, as measured, the raw value is odd), so comparing the raw
    // pointer against an aligned expected address was always false and produced a
    // spurious MISMATCH on every single call.
    entry.flags = ((top & ~uintptr_t{1}) == expected_jni_frame ? 1u : 0u) |
                  (thread ? 2u : 0u) | (native_entry_route ? 4u : 0u) |
                  (!native_entry_route && slot && top_method == slot ? 8u : 0u) |
                  (router_route ? 16u : 0u) |
                  (entry.ctx_delta == sizeof(HookContext) ? 32u : 0u);

    // Capture the tail now: after the thunk returns the stack is reused, so a
    // post-mortem read records whatever took its place (verified on device).
    // The router frame is 224 bytes; reading a 368-byte tail past it would
    // describe its caller, not us, so that route records router_slots instead.
    // Clear the other window: ring entries are reused, and a stale value from a
    // previous entry would read as if it belonged to this frame.
    if (router_route) {
        for (size_t i = 0; i < 8; ++i) entry.tail[i] = 0;
        for (size_t i = 0; i < 11; ++i) {
            entry.router_slots[i] = *reinterpret_cast<volatile const uint64_t *>(
                frame + kRouterSlotWindowOffset + i * sizeof(uint64_t));
        }
    } else {
        for (size_t i = 0; i < 11; ++i) entry.router_slots[i] = 0;
        for (size_t i = 0; i < 8; ++i) {
            entry.tail[i] = *reinterpret_cast<volatile const uint64_t *>(
                frame + 352 + i * sizeof(uint64_t));
        }
    }

    // ---------------------------------------------------------------------
    // Walk the ManagedStack chain the stack walker will follow.
    //
    // Our own slot is already known to hold a valid ArtMethod, so the bogus
    // method the crashes dereference has to come from another hop. Record, per
    // fragment: the frame pointer, the ArtMethod stored there, and whether that
    // method is one of ours. A hop whose method is a stack address is exactly
    // the tombstone_10 shape (PrettyMethod on 0x77ff0a4790).
    // ---------------------------------------------------------------------
    for (auto &hop : entry.chain) hop = FrameChainHop{};
    if (thread && g_thread_layout.valid) {
        uintptr_t fragment = thread + g_thread_layout.managed_stack_offset;
        for (size_t i = 0; i < kMaxManagedStackFragments; ++i) {
            if (!fragment) break;
            auto &hop = entry.chain[i];
            hop.fragment = fragment;
            const uintptr_t raw = *reinterpret_cast<volatile const uintptr_t *>(fragment);
            hop.top_raw = raw;
            const uintptr_t hop_frame = raw & ~uintptr_t{1};
            hop.frame = hop_frame;
            // Only dereference when it looks like this thread's stack, same rule
            // as above: no IsReadable(), this path must not touch /proc.
            const bool plausible =
                hop_frame > 0x1000 &&
                (hop_frame > frame ? (hop_frame - frame) < (8u * 1024u * 1024u)
                                   : (frame - hop_frame) < (8u * 1024u * 1024u));
            if (plausible) {
                hop.frame_method = *reinterpret_cast<volatile const uintptr_t *>(hop_frame);
                hop.flags = 1u;
                if (!native_entry_route && slot &&
                    (hop.frame_method == slot ||
                     (hop.frame_method & kPacStripMask) == (slot & kPacStripMask))) {
                    hop.flags |= 2u;
                }
            }
            // Advance to the previous ManagedStack fragment.
            const uintptr_t next =
                *reinterpret_cast<volatile const uintptr_t *>(fragment +
                                                              kManagedStackLinkOffset);
            // link_ is a real pointer (not a frame pointer), but it can be
            // tagged on some builds; keep the low 48 bits like every other read.
            const uintptr_t next_clean = next & kPacStripMask;
            if (!next_clean || next_clean == fragment) break;
            fragment = next_clean;
        }
    }

    // Log the first few entries whether or not the geometry holds: an absent
    // warning would otherwise be ambiguous between "geometry is fine" and
    // "probe never ran".
    {
        static std::atomic<uint32_t> sampled{0};
        const uint32_t n = sampled.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 6) {
            LOGI("WxShadow ART frame probe: route={} top_quick_frame={} frame={} "
                 "declared={} expected={} delta={} slot={} top_method={} same={} tid={} n={}",
                 ProbeRouteName(native_entry_route, router_route),
                 reinterpret_cast<void *>(top), reinterpret_cast<void *>(frame),
                 static_cast<unsigned long>(declared),
                 reinterpret_cast<void *>(expected_jni_frame),
                 static_cast<long>(top - frame), reinterpret_cast<void *>(slot),
                 reinterpret_cast<void *>(top_method),
                 (top_method && top_method == slot) ? "yes" : "NO",
                 static_cast<long>(gettid()), n);
        }
    }
    // The handover's open question (6.1): a probe entry said `slot=0x0` and it
    // was impossible to tell whether that slot belonged to a native trampoline
    // frame at all. Log every entry whose slot is not something ART could use as
    // an ArtMethod, together with the identity of the context that produced it.
    //   ctx_delta = ctx_sp - ctx : 352 => stack-resident HookContext
    //                              other => not our frame (router TLS ctx)
    //   slot_class / x0_class    : see kProbeMethod* bits
    {
        static std::atomic<uint32_t> attributed{0};
        const bool unusable =
            slot == 0 ||
            !(slot_class & (kProbeMethodReadable | kProbeMethodInPool));
        if (unusable) {
            const uint32_t n = attributed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 24) {
                LOGW("WxShadow ART frame probe SLOT: route={} tid={} ctx={} ctx_sp={} "
                     "ctx_delta={} frame={} slot={} slot_class={:#x} "
                     "ctx_x0={} x0_class={:#x} flags={:#x} n={}",
                     ProbeRouteName(native_entry_route, router_route),
                     static_cast<long>(gettid()), reinterpret_cast<void *>(ctx),
                     reinterpret_cast<void *>(ctx->sp),
                     static_cast<long>(entry.ctx_delta),
                     reinterpret_cast<void *>(frame), reinterpret_cast<void *>(slot),
                     slot_class, reinterpret_cast<void *>(ctx->x[0]), ctx_x0_class,
                     entry.flags, n);
                if (router_route) {
                    LOGW("WxShadow ART frame probe   router_slots[+136..+216]: "
                         "x20={} x21={} x22={} x23={} x24={} x25={} x26={} x27={} "
                         "x28={} pad={} ret_pc={}",
                         reinterpret_cast<void *>(entry.router_slots[0]),
                         reinterpret_cast<void *>(entry.router_slots[1]),
                         reinterpret_cast<void *>(entry.router_slots[2]),
                         reinterpret_cast<void *>(entry.router_slots[3]),
                         reinterpret_cast<void *>(entry.router_slots[4]),
                         reinterpret_cast<void *>(entry.router_slots[5]),
                         reinterpret_cast<void *>(entry.router_slots[6]),
                         reinterpret_cast<void *>(entry.router_slots[7]),
                         reinterpret_cast<void *>(entry.router_slots[8]),
                         reinterpret_cast<void *>(entry.router_slots[9]),
                         reinterpret_cast<void *>(entry.router_slots[10]));
                }
            }
        }
    }
    // Geometry violation only: compare the untagged pointer, otherwise the tag
    // bit makes this fire on every call. The router route declares its own
    // 224-byte geometry, so it is excluded the same way native_entry is.
    if (!router_route && thread && top && (top & ~uintptr_t{1}) != expected_jni_frame) {
        static std::atomic<uint32_t> reported{0};
        const uint32_t n = reported.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8) {
            LOGW("WxShadow ART frame probe GEOMETRY: route={} top_quick_frame={} frame={} "
                 "declared={} expected={} delta={} slot={} tid={} n={}",
                 ProbeRouteName(native_entry_route, router_route),
                 reinterpret_cast<void *>(top), reinterpret_cast<void *>(frame),
                 static_cast<unsigned long>(declared),
                 reinterpret_cast<void *>(expected_jni_frame),
                 static_cast<long>((top & ~uintptr_t{1}) - frame),
                 reinterpret_cast<void *>(slot), static_cast<long>(gettid()), n);
        }
    }

    // Dump the ManagedStack chain. This replaces the earlier "METHOD LOST"
    // alarm, which compared a top-byte-tagged slot value with the hardware-
    // ignored read-back: under TBI (tagged_addr_ctrl=1) b4000079918d3d90 and
    // 79918d3d90 are the same address, so that alarm fired on healthy frames.
    // What actually matters is where in the chain the method stops being an
    // ArtMethod.
    {
        static std::atomic<uint32_t> chain_logged{0};
        const uint32_t n = chain_logged.fetch_add(1, std::memory_order_relaxed) + 1;
        // Log the first few calls, then only calls whose chain contains a hop
        // holding a non-ArtMethod-looking value (a stack address).
        bool suspicious = false;
        const uintptr_t lo = frame > (8u * 1024u * 1024u) ? frame - (8u * 1024u * 1024u) : 0;
        const uintptr_t hi = frame + (8u * 1024u * 1024u);
        for (const auto &hop : entry.chain) {
            if (!hop.fragment || !(hop.flags & 1u)) continue;
            if (hop.flags & 2u) continue;  // it is one of our own ArtMethods
            const uintptr_t m = hop.frame_method & kPacStripMask;
            // A "method" that lands in this thread's own stack is the direct
            // signature of the tombstone_10 crash (PrettyMethod on a stack
            // address), and is never legitimate.
            if (m >= lo && m <= hi) suspicious = true;
        }
        if (n <= 4 || suspicious) {
            LOGW("WxShadow ART frame probe CHAIN: route={} tid={} our_frame={} slot={} "
                 "slot_class={:#x} flags={:#x} hops={} susp={} n={}",
                 ProbeRouteName(native_entry_route, router_route),
                 static_cast<long>(gettid()), reinterpret_cast<void *>(frame),
                 reinterpret_cast<void *>(slot), slot_class, entry.flags,
                 static_cast<int>(kMaxManagedStackFragments),
                 suspicious ? "yes" : "no", n);
            for (size_t i = 0; i < kMaxManagedStackFragments; ++i) {
                const auto &hop = entry.chain[i];
                if (!hop.fragment) break;
                LOGW("WxShadow ART frame probe   hop[{}] frag={} top={} frame={} method={} "
                     "class={:#x} ours={}",
                     i, reinterpret_cast<void *>(hop.fragment),
                     reinterpret_cast<void *>(hop.top_raw),
                     reinterpret_cast<void *>(hop.frame),
                     reinterpret_cast<void *>(hop.frame_method),
                     ClassifyProbeMethod(hop.frame_method, frame),
                     (hop.flags & 2u) ? "yes" : "no");
            }
        }
    }
#else
    (void)ctx;
    (void)native_entry_route;
#endif
}

size_t ReadNativeThunkFrameProbe(uint64_t *out, size_t capacity) {
    if (!out) return 0;
    const size_t written = std::min(g_native_thunk_frame_probe_count.load(), kFrameProbeEntries);
    const size_t n = std::min(written, capacity / kFrameProbeStride);
    auto *words = reinterpret_cast<const uint64_t *>(g_native_thunk_frame_probe);
    std::memcpy(out, words, n * kFrameProbeStride * sizeof(uint64_t));
    return n;
}

MethodState::~MethodState() {
#if defined(__aarch64__)
    UnregisterActiveState(this);
    // JNI references are owned by the process-lifetime HookItem registry. The
    // registry intentionally keeps hooks alive, so no destructor-time JNI call
    // is needed here.
    if (native_entry_hook) {
        if (native_entry_installed && native_entry_target) {
            hook_remove(reinterpret_cast<void *>(native_entry_target));
        }
    } else if (quick_trampoline) {
        hook_art_router_table_remove(original_art_method);
        if (owns_router_hook) {
            if (router_target) hook_remove(reinterpret_cast<void *>(router_target));
            else if (original_entry) hook_remove(reinterpret_cast<void *>(original_entry));
        }
    }
    if (original_entry_mutated && original_art_method &&
        entry_offset + sizeof(uintptr_t) <= method_size &&
        IsReadable(original_art_method, entry_offset + sizeof(uintptr_t))) {
        // Source: rustFrida java_hook_api/uninstall.rs restores the original
        // ArtMethod fields after removing the router-table replacement.
        *reinterpret_cast<volatile uintptr_t *>(original_art_method + entry_offset) =
            original_entry;
        hook_flush_cache(reinterpret_cast<void *>(original_art_method + entry_offset),
                         sizeof(uintptr_t));
        LOGD("WxShadow ART shared entry restored: method={} entry={}",
             reinterpret_cast<void *>(original_art_method),
             reinterpret_cast<void *>(original_entry));
    }
    // The native replacement thunk is tracked separately from the inline
    // router. It is not a function hook entry, so remove_redirect() releases
    // its executable-pool record without touching ArtMethod memory.
    if (original_art_method) hook_remove_redirect(original_art_method);
    if (original_clone) free(reinterpret_cast<void *>(original_clone));
    if (replacement) free(reinterpret_cast<void *>(replacement));
#endif
}

// ---------------------------------------------------------------------------
// rustFrida 对齐：原生入口回调的 ABI 判定 + $orig 原寄存器直调
// ---------------------------------------------------------------------------

// rustFrida install.rs:133 is_critical_native_signature_supported ——
// @CriticalNative 只能用于 static、基本类型参数/返回；不满足时该 flag 视为
// ART noise（install.rs:315-319）。Vector 的 ValueKind 把数组归入 Object。
static bool IsCriticalNativeSignatureSupported(const MethodState &state) {
    if (!state.is_static) return false;
    if (state.return_kind == ValueKind::Object) return false;
    for (const auto kind : state.parameter_kinds) {
        if (kind == ValueKind::Object) return false;
    }
    return true;
}

// 把 trampoline 直调后的原始返回寄存器转成 jvalue，供 BoxReturn 装箱。
static jvalue ReturnValueFromRaw(const MethodState &state, const HookContext *ctx,
                                 uint64_t raw) {
    jvalue result{};
    switch (state.return_kind) {
    case ValueKind::Boolean: result.z = static_cast<jboolean>(raw); break;
    case ValueKind::Byte: result.b = static_cast<jbyte>(raw); break;
    case ValueKind::Char: result.c = static_cast<jchar>(raw); break;
    case ValueKind::Short: result.s = static_cast<jshort>(raw); break;
    case ValueKind::Int: result.i = static_cast<jint>(raw); break;
    case ValueKind::Long: result.j = static_cast<jlong>(raw); break;
    case ValueKind::Float: {
        const uint32_t bits = static_cast<uint32_t>(ctx->d[0]);
        std::memcpy(&result.f, &bits, sizeof(result.f));
        break;
    }
    case ValueKind::Double: std::memcpy(&result.d, &ctx->d[0], sizeof(result.d)); break;
    case ValueKind::Object: result.l = reinterpret_cast<jobject>(raw); break;
    case ValueKind::Void: break;
    }
    return result;
}

// dispatch 记录的实参 vs hooker 传回的实参：逐元素相等即视为「原样透传」。
// 此时用保存的寄存器直调原函数（rustFrida original_call.rs:1137 的 $orig()
// 无参路径），避开 JNI 重入 ArtMethod。
static bool ArgsMatchObserved(JNIEnv *env, const MethodState &state,
                              jobjectArray observed, jobjectArray supplied) {
    if (!observed || !supplied) return observed == supplied;
    const size_t offset = state.is_static ? 0 : 1;   // dispatch 数组含 receiver
    const jsize count = static_cast<jsize>(state.parameter_kinds.size());
    if (env->GetArrayLength(observed) != count + static_cast<jsize>(offset) ||
        env->GetArrayLength(supplied) != count) {
        return false;
    }
    for (jsize i = 0; i < count; ++i) {
        jobject a = env->GetObjectArrayElement(observed, i + static_cast<jsize>(offset));
        jobject b = env->GetObjectArrayElement(supplied, i);
        const ValueKind kind = state.parameter_kinds[static_cast<size_t>(i)];
        bool same = false;
        if (kind == ValueKind::Object) {
            same = env->IsSameObject(a, b) == JNI_TRUE;
        } else {
            bool ok_a = false;
            bool ok_b = false;
            const jvalue va = ReadArgument(env, a, kind, &ok_a);
            const jvalue vb = ReadArgument(env, b, kind, &ok_b);
            if (ok_a && ok_b) {
                switch (kind) {
                case ValueKind::Boolean: same = va.z == vb.z; break;
                case ValueKind::Byte: same = va.b == vb.b; break;
                case ValueKind::Char: same = va.c == vb.c; break;
                case ValueKind::Short: same = va.s == vb.s; break;
                case ValueKind::Int: same = va.i == vb.i; break;
                case ValueKind::Long: same = va.j == vb.j; break;
                case ValueKind::Float: same = va.f == vb.f; break;
                case ValueKind::Double: same = va.d == vb.d; break;
                default: same = false; break;
                }
            }
            env->ExceptionClear();
        }
        if (a) env->DeleteLocalRef(a);
        if (b) env->DeleteLocalRef(b);
        if (!same) return false;
    }
    return true;
}

// hooker 执行期间的 dispatch 帧栈（thread-local）：把当前 HookContext* 暴露给
// InvokeOriginal，对齐 rustFrida 将 hook_ctx_ptr 绑到回调对象。
//
// 注意：这里必须使用 POD 定长数组，不能使用 std::vector 等带析构函数的容器。
// 本 so 由 Zygisk 的自定义加载器映射，linker 里没有对应的 soinfo；带析构的
// thread_local 在首次访问时会调用 __cxa_thread_atexit_impl(&__dso_handle)，
// linker 查不到该 soinfo 就直接 abort：
//   'increment_dso_handle_reference_counter: Couldn't find soinfo by dso_handle=...'
// （实测在 system_server 首个 dispatch 上触发，导致开机 boot loop）。
// POD 数组无需析构，因此不会注册 TLS dtor。
namespace {
struct DispatchFrame {
    jmethodID target = nullptr;
    HookContext *ctx = nullptr;
    jobjectArray values = nullptr;
};
constexpr size_t kMaxDispatchFrames = 16;
thread_local DispatchFrame g_dispatch_frames[kMaxDispatchFrames];
thread_local size_t g_dispatch_frame_count = 0;
}  // namespace

void PushDispatchFrame(jmethodID target, HookContext *ctx, jobjectArray values) {
    if (g_dispatch_frame_count >= kMaxDispatchFrames) return;
    g_dispatch_frames[g_dispatch_frame_count++] = DispatchFrame{target, ctx, values};
}

void PopDispatchFrame(jmethodID target) {
    (void)target;
    if (g_dispatch_frame_count > 0) --g_dispatch_frame_count;
}

bool LookupDispatchFrame(jmethodID target, HookContext **ctx, jobjectArray *values) {
    if (g_dispatch_frame_count == 0) return false;
    // 先按 target 精确匹配；jmethodID 可能有低位标记导致键不完全相等，
    // 此时取栈顶（当前正在执行的 hooker 所属帧）仍然是语义正确的。
    size_t index = g_dispatch_frame_count - 1;
    for (size_t i = g_dispatch_frame_count; i-- > 0;) {
        if (g_dispatch_frames[i].target == target) {
            index = i;
            break;
        }
    }
    const DispatchFrame &found = g_dispatch_frames[index];
    if (ctx) *ctx = found.ctx;
    if (values) *values = found.values;
    return true;
}

bool Initialize(JNIEnv *env) {
#if defined(__aarch64__)
    return CompleteInitialization(env);
#else
    (void)env;
    return false;
#endif
}

bool InitializeBootstrap(JNIEnv *env) {
#if defined(__aarch64__)
    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    if (!env) return false;
    std::call_once(g_base_init_once, [env] { g_base_initialized = InitializeBaseImpl(env); });
    if (!g_base_initialized) return false;
    std::call_once(g_bootstrap_init_once,
                   [env] { g_bootstrap_initialized = InitializeBootstrapImpl(env); });
    if (perf_start != 0) {
        LOGI("[perf] art_bootstrap end ready={} stealth={} elapsedUs={}",
             g_bootstrap_initialized, ArtStealthModeName(g_art_stealth_mode),
             ArtPerfElapsedUs(perf_start));
    }
    return g_bootstrap_initialized;
#else
    (void)env;
    return false;
#endif
}

bool CompleteInitialization(JNIEnv *env) {
#if defined(__aarch64__)
    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    if (!InitializeBootstrap(env)) return false;
    std::call_once(g_controller_init_once,
                   [env] { g_initialized = CompleteInitializationImpl(env); });
    if (perf_start != 0) {
        LOGI("[perf] art_complete end ready={} elapsedUs={}",
             g_initialized, ArtPerfElapsedUs(perf_start));
    }
    return g_initialized;
#else
    (void)env;
    return false;
#endif
}

bool IsAvailable() {
#if defined(__aarch64__)
    return g_bootstrap_initialized;
#else
    return false;
#endif
}

uintptr_t DecodeArtMethod(JNIEnv *env, jobject reflected_method) {
#if defined(__aarch64__)
    return DecodeReflectedArtMethod(env, reflected_method);
#else
    (void)env;
    (void)reflected_method;
    return 0;
#endif
}

std::unique_ptr<MethodState> Install(JNIEnv *env, jobject reflected_method,
                                     NativeHookCallback callback, uintptr_t key,
                                     int stealth_override) {
#if !defined(__aarch64__)
    (void)env; (void)reflected_method; (void)callback; (void)key; (void)stealth_override;
    return nullptr;
#else
    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    if (!env || !reflected_method || !callback || !InitializeBootstrap(env)) return nullptr;
    std::lock_guard lock(g_install_mutex);

    // Scoped per-install stealth override. A non-negative override (e.g. Normal for framework
    // hooks whose code pages embed literal pools and livelock under WxShadow) is visible to
    // InstallArtRouterWithFallback via ArtStealthFlag() and restored on scope exit.
    ScopedStealthOverride scoped_override(stealth_override);

    auto state = std::make_unique<MethodState>();
    state->original_art_method = DecodeReflectedArtMethod(env, reflected_method);
    if (!state->original_art_method ||
        !IsReadable(state->original_art_method, g_layout.method_size)) {
        return nullptr;
    }
    state->method_size = g_layout.method_size;
    state->access_flags_offset = g_layout.access_flags_offset;
    state->data_offset = g_layout.data_offset;
    state->entry_offset = g_layout.entry_offset;
    if (state->access_flags_offset + sizeof(uint32_t) > state->method_size ||
        state->data_offset + sizeof(uintptr_t) > state->method_size ||
        state->entry_offset + sizeof(uintptr_t) > state->method_size) {
        LOGE("WxShadow ART install rejected invalid layout: size={} flags={} data={} entry={}",
             state->method_size, state->access_flags_offset, state->data_offset,
             state->entry_offset);
        return nullptr;
    }
    const uint32_t original_flags = *reinterpret_cast<const uint32_t *>(
        state->original_art_method + state->access_flags_offset);
    const uintptr_t original_data = *reinterpret_cast<const uintptr_t *>(
        state->original_art_method + state->data_offset);
    state->original_entry = *reinterpret_cast<uintptr_t *>(
        state->original_art_method + state->entry_offset);
    if (!IsMappedExecutable(state->original_entry)) {
        LOGI("WxShadow ART route skipped for non-executable entry: method={} entry={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(state->original_entry));
        return nullptr;
    }

    // Reference: rustFrida install.rs::is_registered_native_entry_candidate and
    // the NativeEntry branch. A normal registered JNI method is hooked at data_,
    // not through the ART quick router. @CriticalNative 走同一入口（data_），
    // 但 ABI 不同：x0 起就是基本类型参数，且 $orig 只能用保存寄存器直调
    // trampoline（rustFrida callback/hook.rs:398 + original_call.rs:1096）。
    //
    // rustFrida install.rs:311-319 对齐：flag 与签名必须同时成立才算 @CriticalNative。
    // 整个 Install 的 parameter_kinds/return_kind 解析在路由分类之后，但路由分类本身
    // 要拿这个结论，所以这里先做一次**只读**的签名探测。不是重复劳动：探测失败只会
    // 让一个可疑 flag 降级成普通 native（即多 hook 一个方法），不会误拒。
    const bool critical_signature_supported =
        CriticalNativeSignatureSupportedFromReflection(env, reflected_method);
    const bool has_critical_flag = (original_flags & kAccCriticalNative) != 0;
    if (has_critical_flag && !critical_signature_supported) {
        LOGI("WxShadow ART critical-native flag ignored as ART noise: method={} flags=0x{:x}",
             reinterpret_cast<void *>(state->original_art_method), original_flags);
    }
    const bool data_is_executable = IsMappedExecutable(original_data);
    const bool data_is_in_art = IsInArtModule(original_data & kPacStripMask);
    const auto native_route = ClassifyNativeRoute(
        original_flags, original_data, data_is_executable, data_is_in_art,
        critical_signature_supported);
    const bool is_native_method = native_route != NativeRouteKind::NotNative;
    const bool registered_native = native_route == NativeRouteKind::RegisteredNative;
    const bool critical_native = is_native_method && has_critical_flag;
    if (native_route == NativeRouteKind::UnsupportedNative) {
        LOGI("WxShadow ART native route deferred to LSPlant: method={} data={} entry={} registered={} critical={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(original_data),
             reinterpret_cast<void *>(state->original_entry), registered_native,
             critical_native);
        return nullptr;
    }
    if (registered_native) {
        void *resolved_native = resolve_art_trampoline(
            reinterpret_cast<void *>(original_data), nullptr);
        if (!resolved_native) resolved_native = reinterpret_cast<void *>(original_data);
        if (!IsMappedExecutable(reinterpret_cast<uintptr_t>(resolved_native))) return nullptr;
        state->native_entry_hook = true;
        state->native_entry_target = reinterpret_cast<uintptr_t>(resolved_native);
        state->native_entry_critical = critical_native;
        state->callback = callback;
        state->callback_user_data = reinterpret_cast<void *>(key);
        LOGI("ART native entry route: method={} target={} critical={} flags=0x{:x}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(resolved_native), critical_native, original_flags);
    }

    // rustFrida's route_has_independent_code covers compiled AOT/JIT entries
    // and unregistered/shared native entries. Ordinary non-native shared
    // entries instead contribute only a table entry to the process-wide
    // interpreter/resolution router after any required internal downgrade.
    void *resolved_entry = resolve_art_trampoline(
        reinterpret_cast<void *>(state->original_entry), env);
    if (!resolved_entry) resolved_entry = reinterpret_cast<void *>(state->original_entry);
    const auto resolved_entry_address =
        reinterpret_cast<uintptr_t>(resolved_entry) & kPacStripMask;
    const auto original_entry_address = state->original_entry & kPacStripMask;
    // Source: rustFrida art_method/resolution.rs::is_art_quick_entrypoint and
    // java_hook_api/install.rs::shared_native_art_entry. A native method whose
    // data_ is not an independently registered executable still needs the
    // Layer 3 replacement route when its entry is an ART-internal/shared
    // entry. It must not be folded into the ordinary non-native shared-stub
    // downgrade path.
    const auto shared_router_for_entry = [](uintptr_t entry) -> const SharedNterpRouter * {
        if (IsSharedRouterEntry(entry, g_nterp_router)) return &g_nterp_router;
        if (IsSharedRouterEntry(entry, g_nterp_clinit_router)) return &g_nterp_clinit_router;
        if (IsSharedRouterEntry(entry, g_jni_router)) return &g_jni_router;
        if (IsSharedRouterEntry(entry, g_interpreter_bridge_router)) {
            return &g_interpreter_bridge_router;
        }
        if (IsSharedRouterEntry(entry, g_resolution_router)) return &g_resolution_router;
        return nullptr;
    };
    const bool has_independent_code =
        !IsInArtModule(original_entry_address) &&
        shared_router_for_entry(original_entry_address) == nullptr;
    const bool shared_native_art_entry = is_native_method &&
                                         !has_independent_code &&
                                         !registered_native;
    const bool route_has_independent_code = has_independent_code ||
                                            shared_native_art_entry;
    const SharedNterpRouter *shared_router = nullptr;
    bool downgrade_shared_entry = false;
    if (!state->native_entry_hook && !route_has_independent_code) {
        // Source: rustFrida install_support.rs::is_already_routed and the
        // normal Java hook call with allow_internal_entry_downgrade=true.
        // Exact entries already covered by a process-wide router stay intact.
        // An unclassified libart entry is internally downgraded to the
        // interpreter bridge after the clone/replacement copies are prepared.
        shared_router = shared_router_for_entry(original_entry_address);
        if (!shared_router) {
            shared_router = shared_router_for_entry(resolved_entry_address);
        }
        if (!shared_router && g_interpreter_bridge_router.source != 0 &&
            g_interpreter_bridge_router.trampoline != 0) {
            shared_router = &g_interpreter_bridge_router;
            downgrade_shared_entry = true;
        }
        if (!shared_router && EnsureInterpreterRouter(env)) {
            shared_router = &g_interpreter_bridge_router;
            downgrade_shared_entry = true;
            LOGD("WxShadow ART dynamic shared router prepared on demand: method={} source={}",
                 reinterpret_cast<void *>(state->original_art_method),
                 reinterpret_cast<void *>(state->original_entry));
        }
    }
    const bool uses_shared_router = shared_router != nullptr;
    if (perf_start != 0) {
        // 诊断：定位 DexFile.openDexFile 类 hook 安装秒级卡顿（2026-09-04 复现 0.6~1.8s）。
        // 本点覆盖：DecodeArtMethod / native route 分类 / entry 解析 / shared router 决策。
        LOGI("[perf] art_method_install stage=route method={} entry={} shared_router={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(state->original_entry), uses_shared_router,
             ArtPerfElapsedUs(perf_start));
    }
    if (!state->native_entry_hook && !route_has_independent_code && !uses_shared_router) {
        LOGI("WxShadow ART route skipped for unclassified shared libart entry: method={} source={} target={} native={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(state->original_entry), resolved_entry,
             is_native_method);
        return nullptr;
    }

    auto executable_class = env->FindClass("java/lang/reflect/Executable");
    if (!executable_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (executable_class) env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    auto get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");
    auto get_declaring_class = env->GetMethodID(
        executable_class, "getDeclaringClass", "()Ljava/lang/Class;");
    auto get_params = env->GetMethodID(executable_class, "getParameterTypes", "()[Ljava/lang/Class;");
    if (!get_modifiers || !get_declaring_class || !get_params || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    const jint modifiers = env->CallIntMethod(reflected_method, get_modifiers);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    state->is_static = (modifiers & kAccStatic) != 0;
    auto params = reinterpret_cast<jobjectArray>(env->CallObjectMethod(reflected_method, get_params));
    if (!params || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    const auto count = env->GetArrayLength(params);
    state->parameter_kinds.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto type = env->GetObjectArrayElement(params, i);
        if (!type || env->ExceptionCheck()) {
            env->ExceptionClear();
            if (type) env->DeleteLocalRef(type);
            env->DeleteLocalRef(params);
            env->DeleteLocalRef(executable_class);
            return nullptr;
        }
        state->parameter_kinds.push_back(KindFromClass(env, type));
        env->DeleteLocalRef(type);
    }
    env->DeleteLocalRef(params);

    auto method_class = env->GetObjectClass(reflected_method);
    if (!method_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (method_class) env->DeleteLocalRef(method_class);
        env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    auto get_return_type = env->GetMethodID(method_class, "getReturnType", "()Ljava/lang/Class;");
    if (get_return_type) {
        auto return_class = env->CallObjectMethod(reflected_method, get_return_type);
        if (return_class && !env->ExceptionCheck()) {
            state->return_kind = KindFromClass(env, return_class);
            env->DeleteLocalRef(return_class);
        } else {
            if (return_class) env->DeleteLocalRef(return_class);
            env->ExceptionClear();
            state->return_kind = ValueKind::Void;
        }
    } else {
        // java.lang.reflect.Constructor has no getReturnType(). Its native
        // callback must use the JNI void return convention.
        env->ExceptionClear();
        state->return_kind = ValueKind::Void;
    }
    env->DeleteLocalRef(method_class);

    // rustFrida install.rs:314-319 对齐：flag 与签名必须同时成立才算 @CriticalNative，
    // 否则该位是 ART noise（MIUI 等构建可能误置）。
    //
    // 路由分类处已经用反射做过一次同样的判定（CriticalNativeSignatureSupportedFromReflection），
    // 这里是第二道防线，使用的证据不同：此刻 MethodState 的 parameter_kinds / return_kind
    // 已经由完整解析填好，比反射探测更权威。两者共用同一个语义定义
    //（IsCriticalNativeSignatureSupported），不会产生分歧；只有在前置探测因异常而
    // fail-closed 降级、而完整解析又成功时，这里才会真正生效。
    if (state->native_entry_critical && !IsCriticalNativeSignatureSupported(*state)) {
        LOGI("ART native entry critical flag ignored as ART noise: method={} flags=0x{:x}",
             reinterpret_cast<void *>(state->original_art_method), original_flags);
        state->native_entry_critical = false;
    }

    auto declaring_class = env->CallObjectMethod(reflected_method, get_declaring_class);
    if (!declaring_class || env->ExceptionCheck()) {
        env->ExceptionClear();
        if (declaring_class) env->DeleteLocalRef(declaring_class);
        env->DeleteLocalRef(executable_class);
        return nullptr;
    }
    state->declaring_class = env->NewGlobalRef(declaring_class);
    env->DeleteLocalRef(declaring_class);
    env->DeleteLocalRef(executable_class);
    if (!state->declaring_class) return nullptr;
    if (perf_start != 0) {
        // 诊断：覆盖 Executable 反射段（getModifiers/getParameterTypes/getReturnType/declaring）。
        LOGI("[perf] art_method_install stage=reflect method={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method),
             ArtPerfElapsedUs(perf_start));
    }

    state->original_clone = reinterpret_cast<uintptr_t>(malloc(state->method_size));
    if (!state->original_clone || !IsReadable(state->original_art_method, state->method_size)) return nullptr;
    memcpy(reinterpret_cast<void *>(state->original_clone),
           reinterpret_cast<const void *>(state->original_art_method), state->method_size);
    if (state->native_entry_hook) {
        LOGD("WxShadow ART registered-native route prepared: method={} target={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(state->native_entry_target));
        if (perf_start != 0) {
            LOGI("[perf] art_method_install end method={} route=registered_native elapsedUs={}",
                 reinterpret_cast<void *>(state->original_art_method),
                 ArtPerfElapsedUs(perf_start));
        }
        return state;
    }
    state->replacement = reinterpret_cast<uintptr_t>(malloc(state->method_size));
    if (!state->replacement) return nullptr;
    memcpy(reinterpret_cast<void *>(state->replacement),
           reinterpret_cast<const void *>(state->original_art_method), state->method_size);

    // Keep original_clone/replacement based on the pre-downgrade ArtMethod,
    // matching rustFrida's install order. The live ArtMethod entry is changed
    // only after both copies exist, and MethodState::~MethodState restores it
    // if any later install or activation step fails.
    if (perf_start != 0) {
        // 诊断：覆盖 clone（original_clone/replacement malloc+memcpy）。
        LOGI("[perf] art_method_install stage=clone method={} downgrade={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method), downgrade_shared_entry,
             ArtPerfElapsedUs(perf_start));
    }
    if (downgrade_shared_entry &&
        !DowngradeMethodEntryToInterpreterBridge(*state)) {
        LOGW("WxShadow ART shared-entry downgrade unavailable: method={} "
             "bridge_source={} trampoline={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(g_interpreter_bridge_router.source),
             reinterpret_cast<void *>(g_interpreter_bridge_router.trampoline));
        return nullptr;
    }

    // Multiple ArtMethods can point at the same compiled entrypoint.
    // The C engine returns the existing router in that case; remember whether
    // this state owns the inline hook so failure cleanup cannot remove a route
    // still used by another method.
    const bool router_preexisting = !uses_shared_router &&
                                    hook_get_trampoline(resolved_entry) != nullptr;

    if (perf_start != 0) {
        // 诊断：覆盖 entry 降级（interpreter bridge）。
        LOGI("[perf] art_method_install stage=downgrade method={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method),
             ArtPerfElapsedUs(perf_start));
    }
    const uint32_t replacement_flags =
        (original_flags & ~(kAccCriticalNative | kAccFastNative | kAccNterpFastPath)) |
        kAccNative | CompileDontBotherFlag();
    *reinterpret_cast<uint32_t *>(state->replacement + state->access_flags_offset) = replacement_flags;

    // The engine key must be the real ArtMethod pointer for stack-walk
    // sentinels and router lookup; callback userdata remains the raw JNI id
    // used by Vector's HookItem registry.
    void *thunk = hook_create_native_trampoline(state->original_art_method, callback,
                                                 reinterpret_cast<void *>(key), 0,
                                                 state->replacement);
    if (perf_start != 0) {
        // 诊断：覆盖 hook_create_native_trampoline（g_engine 锁 + exec pool hook_alloc + thunk 生成）。
        LOGI("[perf] art_method_install stage=thunk method={} thunk={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method), thunk,
             ArtPerfElapsedUs(perf_start));
    }
    if (!thunk) return nullptr;
    *reinterpret_cast<uintptr_t *>(state->replacement + state->data_offset) =
        reinterpret_cast<uintptr_t>(thunk);
    *reinterpret_cast<uintptr_t *>(state->replacement + state->entry_offset) =
        reinterpret_cast<uintptr_t>(g_layout.jni_trampoline);
    hook_flush_cache(reinterpret_cast<void *>(state->replacement), state->method_size);

    if (uses_shared_router) {
        state->quick_trampoline = shared_router->trampoline;
        state->router_target = shared_router->target;
        state->owns_router_hook = false;
        LOGD("WxShadow ART method registered for shared router: method={} target={}",
             reinterpret_cast<void *>(state->original_art_method),
             reinterpret_cast<void *>(state->router_target));
        if (downgrade_shared_entry) {
            LOGI("WxShadow ART dynamic shared router active: method={} entry={} "
                 "target_art_method_entry_external=false",
                 reinterpret_cast<void *>(state->original_art_method),
                 reinterpret_cast<void *>(g_interpreter_bridge_router.source));
        }
    } else {
        void *hooked_target = nullptr;
        state->quick_trampoline = reinterpret_cast<uintptr_t>(InstallArtRouterWithFallback(
            reinterpret_cast<void *>(state->original_entry),
            static_cast<uint32_t>(state->entry_offset), env, &hooked_target,
            0, 0, 0, 0));
        if (!state->quick_trampoline) return nullptr;
        state->router_target = reinterpret_cast<uintptr_t>(
            hooked_target ? hooked_target : reinterpret_cast<void *>(state->original_entry));
        state->owns_router_hook = !router_preexisting;
        LOGD("WxShadow ART per-method router installed: source={} target={}",
             reinterpret_cast<void *>(state->original_entry),
             reinterpret_cast<void *>(state->router_target));
    }
    if (perf_start != 0) {
        LOGI("[perf] art_method_install end method={} route=art_router stealth={} elapsedUs={}",
             reinterpret_cast<void *>(state->original_art_method),
             ArtStealthModeName(g_art_stealth_mode), ArtPerfElapsedUs(perf_start));
    }
    return state;
#endif
}

bool Activate(MethodState &state) {
#if defined(__aarch64__)
    const uint64_t perf_start = ArtPerfEnabled() ? ArtPerfNowNs() : 0;
    // Publish complete callback state atomically with respect to concurrent
    // installs; the router table itself has no internal writer lock.
    std::lock_guard lock(g_install_mutex);
    if (state.native_entry_hook) {
        if (!state.native_entry_target || !state.callback || !state.original_clone) return false;
        void *trampoline = hook_replace(
            reinterpret_cast<void *>(state.native_entry_target), state.callback,
            state.callback_user_data, ArtStealthFlag());
        if (!trampoline) return false;
        state.quick_trampoline = reinterpret_cast<uintptr_t>(trampoline);
        state.native_entry_installed = true;
        *reinterpret_cast<uintptr_t *>(state.original_clone + state.data_offset) =
            state.quick_trampoline;
        *reinterpret_cast<uintptr_t *>(state.original_clone + state.entry_offset) =
            reinterpret_cast<uintptr_t>(g_layout.jni_trampoline);
        hook_flush_cache(reinterpret_cast<void *>(state.original_clone), state.method_size);
        LOGD("WxShadow ART registered-native entry installed: method={} target={} trampoline={}",
             reinterpret_cast<void *>(state.original_art_method),
             reinterpret_cast<void *>(state.native_entry_target), trampoline);
        if (perf_start != 0) {
            LOGI("[perf] art_method_activate end method={} route=registered_native elapsedUs={}",
                 reinterpret_cast<void *>(state.original_art_method),
                 ArtPerfElapsedUs(perf_start));
        }
        return true;
    }
    if (state.original_art_method == 0 || state.replacement == 0 ||
        hook_art_router_table_add(state.original_art_method, state.replacement) != 0) {
        return false;
    }
    RegisterActiveState(&state);
    if (perf_start != 0) {
        LOGI("[perf] art_method_activate end method={} route=art_router elapsedUs={}",
             reinterpret_cast<void *>(state.original_art_method),
             ArtPerfElapsedUs(perf_start));
    }
    return true;
#else
    (void)state;
    return false;
#endif
}

jobject InvokeOriginal(JNIEnv *env, const MethodState &state, jobject receiver,
                       jobjectArray args) {
#if !defined(__aarch64__)
    (void)env; (void)state; (void)receiver; (void)args;
    return nullptr;
#else
    if (!env || !state.original_clone || !state.declaring_class) return nullptr;
    if (!state.is_static && !receiver) {
        env->ThrowNew(env->FindClass("java/lang/NullPointerException"), "null receiver");
        return nullptr;
    }
    const jsize count = args ? env->GetArrayLength(args) : 0;
    if (count != static_cast<jsize>(state.parameter_kinds.size())) {
        ThrowIllegalArgument(env, "args.length does not match parameter count");
        return nullptr;
    }

    // 注：managed / native-entry 两条路的 $orig 都走 rustFrida 的「寄存器直调重定位
    // trampoline」（前者见下方 quick 寄存器分支，后者见紧跟的 native-entry 分支）。
    // 早前在此处写的「quick 直调不适用」结论已修正（见下方分支注释）。

    // rustFrida original_call.rs:1096/1137 对齐：native-entry hook 的 $orig 优先
    // 用「进入时的原始寄存器」直调保存的 trampoline，而不是经 JNI 重入 ArtMethod。
    // 崩溃实证（tombstone_24/25）：SystemProperties 原生入口的 data_ 会被多种调用
    // 约定进入（AOT JNI stub / generic JNI trampoline），按 JNI 布局重入会把垃圾
    // 参数传给原函数（__strncmp 段错误）。寄存器直调与入口 ABI 无关，天然正确。
    if (state.native_entry_hook && state.quick_trampoline != 0) {
        HookContext *dispatch_ctx = nullptr;
        jobjectArray observed = nullptr;
        if (LookupDispatchFrame(reinterpret_cast<jmethodID>(state.original_art_method),
                                &dispatch_ctx, &observed) &&
            dispatch_ctx) {
            // critical: ABI 决定寄存器就是参数本身，任何 Java 侧实参都只能忽略；
            // 普通 native: 仅在实参原样透传时直调（改参仍走 JNI 路径）。
            const bool pass_through =
                state.native_entry_critical || ArgsMatchObserved(env, state, observed, args);
            if (pass_through) {
                const uint64_t raw = hook_invoke_trampoline(
                    dispatch_ctx, reinterpret_cast<void *>(state.quick_trampoline));
                return BoxReturn(env, state.return_kind,
                                 ReturnValueFromRaw(state, dispatch_ctx, raw));
            }
        }
    }

    // ============================================================
    // rustFrida original_call.rs:1082 对齐 —— managed（非 native-entry）hook 的
    // $orig 优先用 dispatch 帧里保存的 quick 寄存器直调重定位 trampoline，
    // 不再经 JNI 重入。
    //
    // 此前（Part A 实验）判定「quick 寄存器直调不适用」的判据是错的：当时在
    // native-entry 上下文的 dispatch 里测到 x0 = JNIEnv，就以为所有 hook 都是
    // JNI ABI。实测 Java（managed）hook 经 router thunk（224B 帧）进入时是
    // quick ABI：x0 = ArtMethod，x1..x7 / d0..d7 = 实参（router prologue 就是
    // 按这个约定保存寄存器的）。
    //
    // 为什么必须直调而不是 JNI 重入：JNI 重入会让被 patch 的入口带着 JNI-ABI
    // 寄存器（x0 = JNIEnv）被再次进入，thunk 顶部的 $orig bypass 按
    // slot->method == x0 匹配必然失败，于是落到 prologue 把 JNIEnv 当成
    // ArtMethod 写进帧槽 → ART 走栈（GC VisitRoots / DumpJavaStack）读它 →
    //   stack.cc:834] Check failed: existing_entry_point != nullptr
    //   runtime.cc:669] Runtime aborting --- recursively
    // 实测 hunter 冷启期以 SIGABRT 收场。直调不新建任何帧、不重新进入被 patch
    // 的入口，帧槽永远只会是真实 ArtMethod。
    //
    // 与 rustFrida 的差异仅一处：rustFrida 无条件用保存的寄存器（JS 侧改参会被
    // 忽略）；Vector 用 ArgsMatchObserved 校验 Java 侧实参是否原样透传，改过参
    // 仍回落 JNI 路径，语义更保守。
    if (!state.native_entry_hook && state.quick_trampoline != 0 &&
        state.original_art_method != 0) {
        HookContext *dispatch_ctx = nullptr;
        jobjectArray observed = nullptr;
        if (LookupDispatchFrame(reinterpret_cast<jmethodID>(state.original_art_method),
                                &dispatch_ctx, &observed) &&
            dispatch_ctx && dispatch_ctx->x[0] == state.original_art_method) {
            const bool pass_through = ArgsMatchObserved(env, state, observed, args);
            if (pass_through) {
                const uint64_t raw = hook_invoke_trampoline(
                    dispatch_ctx, reinterpret_cast<void *>(state.quick_trampoline));
                return BoxReturn(env, state.return_kind,
                                 ReturnValueFromRaw(state, dispatch_ctx, raw));
            }
        }
    }

    std::vector<jvalue> values(static_cast<size_t>(count));
    std::vector<jobject> object_args;
    object_args.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
        auto value = env->GetObjectArrayElement(args, i);
        bool ok = false;
        values[static_cast<size_t>(i)] = ReadArgument(env, value, state.parameter_kinds[i], &ok);
        if (state.parameter_kinds[i] == ValueKind::Object && value) {
            object_args.push_back(value);
        } else if (value) {
            env->DeleteLocalRef(value);
        }
        if (!ok || env->ExceptionCheck()) {
            for (auto object : object_args) env->DeleteLocalRef(object);
            return nullptr;
        }
    }

    struct LocalRefGuard {
        JNIEnv *env;
        std::vector<jobject> &refs;
        ~LocalRefGuard() {
            for (auto ref : refs) {
                if (ref) env->DeleteLocalRef(ref);
            }
        }
    } object_ref_guard{env, object_args};

    struct OriginalBypassGuard {
        uintptr_t method = 0;
        bool active = false;
        ~OriginalBypassGuard() {
            if (active) (void)orig_bypass_consume_current_thread(method);
        }
    } original_bypass{state.original_art_method, false};
    bool use_live_original = false;
    if (!state.native_entry_hook && state.original_art_method != 0 &&
        state.quick_trampoline != 0) {
        original_bypass.active =
            orig_bypass_set_current_thread(state.original_art_method,
                                            state.quick_trampoline) == 0;
        use_live_original = original_bypass.active;
    }

    if (!use_live_original) {
        SyncOriginalCloneForJni(state);
    }

    jvalue result{};
    auto method = reinterpret_cast<jmethodID>(
        use_live_original ? state.original_art_method : state.original_clone);
    if (state.is_static) {
        switch (state.return_kind) {
        case ValueKind::Boolean: result.z = env->CallStaticBooleanMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Byte: result.b = env->CallStaticByteMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Char: result.c = env->CallStaticCharMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Short: result.s = env->CallStaticShortMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Int: result.i = env->CallStaticIntMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Long: result.j = env->CallStaticLongMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Float: result.f = env->CallStaticFloatMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Double: result.d = env->CallStaticDoubleMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Object: result.l = env->CallStaticObjectMethodA((jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Void: env->CallStaticVoidMethodA((jclass)state.declaring_class, method, values.data()); break;
        }
    } else {
        switch (state.return_kind) {
        case ValueKind::Boolean: result.z = env->CallNonvirtualBooleanMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Byte: result.b = env->CallNonvirtualByteMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Char: result.c = env->CallNonvirtualCharMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Short: result.s = env->CallNonvirtualShortMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Int: result.i = env->CallNonvirtualIntMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Long: result.j = env->CallNonvirtualLongMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Float: result.f = env->CallNonvirtualFloatMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Double: result.d = env->CallNonvirtualDoubleMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Object: result.l = env->CallNonvirtualObjectMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        case ValueKind::Void: env->CallNonvirtualVoidMethodA(receiver, (jclass)state.declaring_class, method, values.data()); break;
        }
    }
    if (auto exception = env->ExceptionOccurred()) {
        env->ExceptionClear();
        ThrowInvocationTarget(env, exception);
        env->DeleteLocalRef(exception);
        return nullptr;
    }
    return BoxReturn(env, state.return_kind, result);
#endif
}

bool Deoptimize(JNIEnv *env, const MethodState &state) {
#if defined(__aarch64__)
    if (!env || !state.original_art_method) return false;
    auto enable = ResolveArtSymbol(
        "_ZN3art15instrumentation15Instrumentation20EnableDeoptimizationEv");
    auto deopt = ResolveArtSymbol(
        "_ZN3art15instrumentation15Instrumentation10DeoptimizeEPNS_9ArtMethodE");
    uintptr_t instrumentation = 0;
    if (!deopt || !ResolveInstrumentation(env, &instrumentation)) return false;
    if (enable) {
        using EnableFn = void (*)(uintptr_t);
        reinterpret_cast<EnableFn>(enable)(instrumentation);
    }
    using DeoptFn = void (*)(uintptr_t, uintptr_t);
    reinterpret_cast<DeoptFn>(deopt)(instrumentation, state.original_art_method);
    return true;
#else
    (void)env;
    (void)state;
    return false;
#endif
}

}  // namespace vector::native::art
