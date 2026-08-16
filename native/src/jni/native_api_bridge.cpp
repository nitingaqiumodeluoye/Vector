#include "core/native_api.h"
#include "jni/jni_bridge.h"
#include "jni/jni_hooks.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

namespace vector::native::jni {

namespace {

using RegisterNativesFn = jint (*)(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                                   jint method_count);

RegisterNativesFn g_original_register_natives = nullptr;
std::once_flag g_register_natives_probe_once;
std::atomic_bool g_register_natives_probe_attempted{false};
std::atomic_bool g_register_natives_probe_ok{false};

std::string JStringToUtf(JNIEnv *env, jstring value) {
    if (!value) return "<null>";
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return "<string-error>";
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::string GetClassName(JNIEnv *env, jclass clazz) {
    if (!env || !clazz) return "<null>";
    jclass class_class = env->FindClass("java/lang/Class");
    if (!class_class) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return "<class-error>";
    }
    jmethodID get_name = env->GetMethodID(class_class, "getName", "()Ljava/lang/String;");
    if (!get_name) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(class_class);
        return "<name-error>";
    }
    auto name = static_cast<jstring>(env->CallObjectMethod(clazz, get_name));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_class);
        return "<exception>";
    }
    std::string out = JStringToUtf(env, name);
    if (name) env->DeleteLocalRef(name);
    env->DeleteLocalRef(class_class);
    return out;
}

std::string Basename(const std::string &path) {
    if (path.empty()) return "";
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool ParseMapsLine(const std::string &line, uintptr_t *start, uintptr_t *end,
                   uintptr_t *file_offset, std::string *path) {
    const size_t dash = line.find('-');
    if (dash == std::string::npos) return false;
    const size_t first_space = line.find(' ', dash + 1);
    if (first_space == std::string::npos) return false;

    std::istringstream fields(line.substr(first_space + 1));
    std::string perms;
    std::string offset_hex;
    std::string dev;
    std::string inode;
    if (!(fields >> perms >> offset_hex >> dev >> inode)) return false;

    try {
        *start = static_cast<uintptr_t>(std::stoull(line.substr(0, dash), nullptr, 16));
        *end = static_cast<uintptr_t>(
            std::stoull(line.substr(dash + 1, first_space - dash - 1), nullptr, 16));
        *file_offset = static_cast<uintptr_t>(std::stoull(offset_hex, nullptr, 16));
    } catch (...) {
        return false;
    }

    std::string rest;
    std::getline(fields, rest);
    const size_t path_start = rest.find_first_not_of(' ');
    *path = path_start == std::string::npos ? std::string{} : rest.substr(path_start);
    return true;
}

std::string DescribeAddress(void *address) {
    const uintptr_t needle = reinterpret_cast<uintptr_t>(address);
    std::ifstream maps("/proc/self/maps");
    if (!maps) return fmt::format("{}", address);

    std::string line;
    std::string hit_path;
    uintptr_t hit_start = 0;
    uintptr_t hit_file_offset = 0;
    while (std::getline(maps, line)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t file_offset = 0;
        std::string path;
        if (!ParseMapsLine(line, &start, &end, &file_offset, &path)) continue;
        if (needle >= start && needle < end) {
            hit_path = path;
            hit_start = start;
            hit_file_offset = file_offset;
            break;
        }
    }
    if (hit_path.empty()) return fmt::format("{}", address);

    uintptr_t module_base = hit_start - hit_file_offset;
    maps.clear();
    maps.seekg(0);
    uintptr_t lowest_zero_offset = std::numeric_limits<uintptr_t>::max();
    while (std::getline(maps, line)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        uintptr_t file_offset = 0;
        std::string path;
        if (!ParseMapsLine(line, &start, &end, &file_offset, &path)) continue;
        if (path == hit_path && file_offset == 0 && start < lowest_zero_offset) {
            lowest_zero_offset = start;
        }
    }
    if (lowest_zero_offset != std::numeric_limits<uintptr_t>::max()) {
        module_base = lowest_zero_offset;
    }

    return fmt::format("{}+0x{:x}", Basename(hit_path), needle - module_base);
}

jint ReplacementRegisterNatives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                                jint method_count) {
    const std::string class_name = GetClassName(env, clazz);
    LOGI("[CoreRegisterNatives] {} ({} methods)", class_name, method_count);
    if (methods && method_count > 0) {
        for (jint i = 0; i < method_count; ++i) {
            const auto &method = methods[i];
            const char *name = method.name ? method.name : "<null>";
            const char *signature = method.signature ? method.signature : "<null>";
            LOGI("[CoreRegisterNatives]   {} {} -> {}", name, signature,
                 DescribeAddress(method.fnPtr));
        }
    }
    if (!g_original_register_natives) {
        LOGE("[CoreRegisterNatives] original trampoline is null");
        return JNI_ERR;
    }
    return g_original_register_natives(env, clazz, methods, method_count);
}

bool InstallRegisterNativesProbe(JNIEnv *env, const char *reason) {
    std::call_once(g_register_natives_probe_once, [env, reason]() {
        g_register_natives_probe_attempted.store(true, std::memory_order_release);
        if (!env || !env->functions || !env->functions->RegisterNatives) {
            LOGE("[CoreRegisterNatives] JNIEnv RegisterNatives unavailable reason={}", reason);
            return;
        }
        void *target = reinterpret_cast<void *>(env->functions->RegisterNatives);
        void *backup = nullptr;
        const int rc = HookInline(target, reinterpret_cast<void *>(ReplacementRegisterNatives),
                                  &backup);
        if (rc != 0 || !backup) {
            LOGE("[CoreRegisterNatives] hook install failed rc={} backup={} target={} reason={}",
                 rc, backup, target, reason);
            return;
        }
        g_original_register_natives = reinterpret_cast<RegisterNativesFn>(backup);
        g_register_natives_probe_ok.store(true, std::memory_order_release);
        LOGI("[CoreRegisterNatives] hook installed target={} backup={} reason={}", target, backup,
             reason);
    });

    if (g_register_natives_probe_attempted.load(std::memory_order_acquire) &&
        !g_register_natives_probe_ok.load(std::memory_order_acquire)) {
        LOGW("[CoreRegisterNatives] hook is not active reason={}", reason);
    }
    return g_register_natives_probe_ok.load(std::memory_order_acquire);
}

}  // namespace

VECTOR_DEF_NATIVE_METHOD(void, NativeAPI, recordNativeEntrypoint, jstring jstr) {
    lsplant::JUTFString str(env, jstr);
    LOGW("Ignoring native entrypoint '{}': module native libraries are disabled; use Vector core "
         "native hook APIs.",
         str.get());
}

VECTOR_DEF_NATIVE_METHOD(jboolean, NativeAPI, installRegisterNativesProbe) {
    return InstallRegisterNativesProbe(env, "NativeAPI.installRegisterNativesProbe") ? JNI_TRUE
                                                                                    : JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
    VECTOR_NATIVE_METHOD(NativeAPI, recordNativeEntrypoint, "(Ljava/lang/String;)V"),
    VECTOR_NATIVE_METHOD(NativeAPI, installRegisterNativesProbe, "()Z")};

void RegisterNativeApiBridge(JNIEnv *env) { REGISTER_VECTOR_NATIVE_METHODS(NativeAPI); }

}  // namespace vector::native::jni
