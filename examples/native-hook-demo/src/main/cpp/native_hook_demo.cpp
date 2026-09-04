// RegisterNatives hook demo for the Vector framework.
//
// This module reproduces, in native C++, the same address-resolution semantics as rustFrida's
// QuickJS `Jni.addr("RegisterNatives")` (quickjs-hook/src/jsapi/jni/jni_boot.js) and then hooks the
// resolved JNI function through Vector's NativeAPIEntries.hookFunc.
//
// rustFrida resolution chain reproduced here (see _getAddress / _getAddressByIndex in jni_boot.js
// and ensure_jni_initialized in jsapi/java/jni_core.rs):
//   1. dl_iterate_phdr -> libart.so 的 load bias + PT_DYNAMIC
//   2. 解析 .dynsym/.dynstr 找 JNI_GetCreatedJavaVMs       -> JavaVM*
//      (比 dlsym: APEX libart 不进入 app namespace，dlsym(RTLD_DEFAULT) 也会 undefined symbol)
//   3. deref(*vm)  -> JNIInvokeInterface function table
//   4. table[6]    -> GetEnv(vm, &env, JNI_VERSION_1_6)   (JNI function table index)
//   5. if GetEnv fails (thread not attached) table[4] -> AttachCurrentThread(vm, &env, NULL)
//   6. deref(*env) -> JNINativeInterface function table
//   7. table[215]  -> RegisterNatives (JNI function table index, matching _JNI_INDEX 215)
//
// Once resolved, the hook is installed with Vector's hookFunc(original, replace, backup). Vector's
// ABI is raw replacement + backup trampoline (hook_install), so `replace` shares RegisterNatives'
// exact signature and the caller obtains the original impl through `backup`.
//
// Expected logcat (tag VectorNativeHookDemo):
//   [register-natives] resolved addr=... via=env-table (JNIEnv table[215])
//   [register-natives] hooked addr=... trampoline=...
//   [register-natives] clazz=... methods=1 jni=Foo.bar (II)V fn=0x...
//   [register-natives] calling original RegisterNatives -> ret=0

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <jni.h>
#include <link.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#define LOG_TAG "VectorNativeHookDemo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// ABI mirror of vector::NativeAPIEntries (native/include/core/native_api.h).
// ---------------------------------------------------------------------------
struct NativeAPIEntries {
    uint32_t version;
    int (*hookFunc)(void *func, void *replace, void **backup);
    int (*unhookFunc)(void *func);
};

using NativeOnModuleLoaded = void (*)(const char *name, void *handle);
using NativeInit = NativeOnModuleLoaded (*)(const NativeAPIEntries *entries);

namespace {

// ARMv8.3 pointer authentication strips the upper bits of a signed pointer. mirror rustFrida's
// PAC_STRIP_MASK before dereferencing or handing an address to the hook engine.
constexpr uintptr_t kPacStripMask = 0x00FFFFFFFFFFFFFFULL;

uintptr_t StripPac(uintptr_t value) {
    if (value & 0xFF00000000000000ULL) {
        value &= kPacStripMask;
    }
    return value;
}

// RegisterNatives signature (JNINativeInterface index 215).
using RegisterNativesFn = jint (*)(JNIEnv *env, jclass clazz,
                                   const JNINativeMethod *methods, jint nMethods);

// JNI invocation table indices (JNIInvokeInterface). GetEnv = 6, AttachCurrentThread = 4.
constexpr size_t kInvokeGetEnv = 6;
constexpr size_t kInvokeAttachCurrentThread = 4;
// JNI function table index for RegisterNatives (matches rustFrida _JNI_INDEX 215).
constexpr size_t kJniRegisterNatives = 215;

std::mutex g_hook_mutex;
NativeAPIEntries g_entries{};
RegisterNativesFn g_original_register_natives = nullptr;
void *g_original_target = nullptr;  // address we passed to hookFunc (PAC-stripped)
std::atomic<bool> g_hooked{false};

// --- RegisterNatives 热路径开销控制（v21）-----------------------------------
//
// 2026-09-04 com.xff.launch 案例：bindApplication 前期 MIUI/chromium 会高频重复
// RegisterNatives 同一批类，旧实现每次都做 FindClass/读 /proc/self/cmdline/
// 逐方法 LOGI/JNI 进 Java 构建对象数组，分配风暴触发 3 次 blocking GC 并抑制
// JIT，导致宿主主线程整条 bindApplication 调用栈退化为纯解释执行（消息耗时
// 15s；禁用模块后同路径仅 1.0s）。本节的三项缓存把重复注册收敛为 O(1)：
//   1. g_seen_register_natives：指纹去重，重复注册不打日志、不进 Java 回调；
//   2. Class/getName 反射元数据全局引用（进程内不变）；
//   3. GetCurrentProcessName 结果缓存（模块实例与进程同生命周期）。
struct RegisterNativesKey {
    size_t hash;          // className 字符串指纹
    uintptr_t firstFn;    // 首方法指针（PAC-stripped）：区分同类不同方法表的重注册
    jint nMethods;
    bool operator==(const RegisterNativesKey &o) const {
        return hash == o.hash && firstFn == o.firstFn && nMethods == o.nMethods;
    }
};
struct RegisterNativesKeyHash {
    size_t operator()(const RegisterNativesKey &k) const {
        return k.hash ^ (k.firstFn * 0x9E3779B97F4A7C15ULL) ^ static_cast<size_t>(k.nMethods);
    }
};
std::mutex g_seen_mutex;
std::unordered_set<RegisterNativesKey, RegisterNativesKeyHash> g_seen_register_natives;

std::mutex g_reflect_mutex;
jclass g_class_clazz = nullptr;          // global ref: java/lang/Class
jmethodID g_get_name_method = nullptr;   // Class.getName

jclass EnsureClassReflectionCache(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(g_reflect_mutex);
    if (g_class_clazz == nullptr) {
        jclass local = env->FindClass("java/lang/Class");
        if (local != nullptr) {
            g_class_clazz = static_cast<jclass>(env->NewGlobalRef(local));
            env->DeleteLocalRef(local);
            g_get_name_method =
                env->GetMethodID(g_class_clazz, "getName", "()Ljava/lang/String;");
        }
    }
    return g_class_clazz;
}

// Java VM and class references for calling back to Java layer
JavaVM *g_vm = nullptr;
jclass g_hook_data_bridge_class = nullptr;
jmethodID g_on_register_natives_method = nullptr;
std::mutex g_java_mutex;

// --- rustFrida-resolution helpers -------------------------------------------------
//
// On Android, libart.so is an APEX module whose exported symbols (JNI_GetCreatedJavaVMs is
// GLOBAL PROTECTED in .dynsym at st_value=0x45f3c8) are NOT reachable through dlopen("libart.so")
// or dlsym(RTLD_DEFAULT): the app's classloader-namespace permits only /data:/mnt/expand, and
// RTLD_DEFAULT does not expose APEX symbols into the app's lookup scope. This is exactly why
// rustFrida's libart_dlsym (resolve.rs) parses the ELF symbol table by hand instead of calling
// dlopen/dlsym. We reproduce that here with dl_iterate_phdr + PT_DYNAMIC parsing.

namespace {

struct LibartElfInfo {
    uintptr_t base = 0;
    const ElfW(Dyn) *dynamic = nullptr;
    size_t dynamic_count = 0;
};

int FindLibartCallback(struct dl_phdr_info *info, size_t /*size*/, void *data) {
    auto *out = static_cast<LibartElfInfo *>(data);
    if (out->base != 0) return 0;  // already found
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    if (strstr(name, "libart.so") == nullptr) return 0;

    out->base = static_cast<uintptr_t>(info->dlpi_addr);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) *phdr = &info->dlpi_phdr[i];
        if (phdr->p_type == PT_DYNAMIC) {
            // PT_DYNAMIC p_vaddr is load-bias-relative; dlpi_addr IS the load bias.
            out->dynamic = reinterpret_cast<const ElfW(Dyn) *>(out->base + phdr->p_vaddr);
            out->dynamic_count = phdr->p_memsz / sizeof(ElfW(Dyn));
            break;
        }
    }
    return 0;
}

// Resolve a symbol from libart.so's dynamic symbol table, mirroring rustFrida
// elf_find_symbols_in_dynamic_memory (DT_SYMTAB traversal without gnu/sysv hash).
uintptr_t DlsymLibartDynamic(const char *name) {
    LibartElfInfo info;
    dl_iterate_phdr(&FindLibartCallback, &info);
    if (info.base == 0 || info.dynamic == nullptr) {
        LOGE("cannot locate libart.so via dl_iterate_phdr");
        return 0;
    }

    const ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    size_t strsz = 0;
    // rustFrida dynamic_ptr(): DT_SYMTAB/DT_STRTAB d_ptr are load-bias-relative in the in-memory
    // PT_DYNAMIC array (the linker leaves them unrelocated there); add load_bias when the value is
    // below it. DT_STRSZ is a size, used verbatim.
    auto dynamic_ptr = [info](uint64_t value) -> uintptr_t {
        return value >= info.base ? static_cast<uintptr_t>(value)
                                  : static_cast<uintptr_t>(info.base + value);
    };
    for (size_t i = 0; i < info.dynamic_count; ++i) {
        const ElfW(Dyn) *d = &info.dynamic[i];
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_SYMTAB) {
            symtab = reinterpret_cast<const ElfW(Sym) *>(dynamic_ptr(d->d_un.d_ptr));
        } else if (d->d_tag == DT_STRTAB) {
            strtab = reinterpret_cast<const char *>(dynamic_ptr(d->d_un.d_ptr));
        } else if (d->d_tag == DT_STRSZ) {
            strsz = static_cast<size_t>(d->d_un.d_val);
        }
    }
    if (symtab == nullptr || strtab == nullptr || strsz == 0) {
        LOGE("libart dynamic symbol table incomplete (symtab=%p strtab=%p strsz=%zu)",
             static_cast<const void *>(symtab), static_cast<const void *>(strtab), strsz);
        return 0;
    }

    // Traverse dynsym. There is no explicit nsyms from PT_DYNAMIC alone, but the symbol table
    // ends where .dynstr begins (symtab < strtab), so bound the scan by that distance and also
    // stop when st_name runs past the string table.
    size_t max_syms = (reinterpret_cast<uintptr_t>(strtab) >
                       reinterpret_cast<uintptr_t>(symtab))
                          ? (reinterpret_cast<uintptr_t>(strtab) -
                             reinterpret_cast<uintptr_t>(symtab)) /
                                sizeof(ElfW(Sym))
                          : 65536;
    for (size_t i = 0; i < max_syms; ++i) {
        const ElfW(Sym) *sym = &symtab[i];
        if (sym->st_name == 0) continue;
        if (sym->st_name >= strsz) break;
        const char *sym_name = strtab + sym->st_name;
        if (strcmp(sym_name, name) == 0) {
            // .dynsym st_value is load-bias-relative for ET_DYN; dlpi_addr is the load bias.
            uintptr_t addr = info.base + sym->st_value;
            LOGI("[register-natives] libart.dynsym %s -> base=%p st_value=%p addr=%p",
                 name, reinterpret_cast<void *>(info.base),
                 reinterpret_cast<void *>(static_cast<uintptr_t>(sym->st_value)),
                 reinterpret_cast<void *>(addr));
            return addr;
        }
    }
    LOGE("symbol '%s' not found in libart .dynsym", name);
    return 0;
}

}  // namespace

// GetJavaVM via JNI_GetCreatedJavaVMs (mirrors rustFrida get_or_init_vm).
JavaVM *GetJavaVm() {
    static JavaVM *cached_vm = nullptr;
    static std::mutex vm_mutex;
    {
        std::lock_guard<std::mutex> lock(vm_mutex);
        if (cached_vm != nullptr) return cached_vm;
    }

    using JniGetCreatedJavaVMs = jint (*)(JavaVM **, jsize, jsize *);
    uintptr_t get_vms_addr = DlsymLibartDynamic("JNI_GetCreatedJavaVMs");
    if (get_vms_addr == 0) {
        LOGE("failed to resolve JNI_GetCreatedJavaVMs from libart .dynsym");
        return nullptr;
    }
    auto get_vms = reinterpret_cast<JniGetCreatedJavaVMs>(get_vms_addr);

    JavaVM *vm = nullptr;
    jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || count == 0 || vm == nullptr) {
        LOGE("JNI_GetCreatedJavaVMs failed (count=%d)", static_cast<int>(count));
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(vm_mutex);
    cached_vm = vm;
    return vm;
}

// GetEnv via the JNIInvokeInterface table[6] (mirrors rustFrida get_current_thread_env).
JNIEnv *GetThreadEnv(JavaVM *vm) {
    if (vm == nullptr) return nullptr;
    auto table = *reinterpret_cast<void ***>(vm);  // *JavaVM -> JNIInvokeInterface
    if (table == nullptr) return nullptr;

    using GetEnvFn = jint (*)(JavaVM *, void **, jint);
    auto get_env = reinterpret_cast<GetEnvFn>(table[kInvokeGetEnv]);
    if (get_env == nullptr) return nullptr;

    void *env = nullptr;
    if (get_env(vm, &env, JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
        return reinterpret_cast<JNIEnv *>(env);
    }
    return nullptr;
}

// AttachCurrentThread via the JNIInvokeInterface table[4] (mirrors rustFrida attach_current_thread).
JNIEnv *AttachThread(JavaVM *vm) {
    if (vm == nullptr) return nullptr;
    auto table = *reinterpret_cast<void ***>(vm);
    if (table == nullptr) return nullptr;

    using AttachFn = jint (*)(JavaVM *, void **, void *);
    auto attach = reinterpret_cast<AttachFn>(table[kInvokeAttachCurrentThread]);
    if (attach == nullptr) return nullptr;

    void *env = nullptr;
    if (attach(vm, &env, nullptr) == JNI_OK && env != nullptr) {
        return reinterpret_cast<JNIEnv *>(env);
    }
    return nullptr;
}

// Resolve RegisterNatives exactly like rustFrida _getAddress(name):
//   JNI_GetCreatedJavaVMs -> GetEnv (table[6]) -> fallback AttachCurrentThread (table[4])
//   -> *env -> JNINativeInterface -> table[215].
uintptr_t ResolveRegisterNativesAddress() {
    JavaVM *vm = GetJavaVm();
    if (vm == nullptr) {
        LOGE("cannot obtain JavaVM*");
        return 0;
    }

    JNIEnv *env = GetThreadEnv(vm);
    if (env == nullptr) {
        LOGW("GetEnv returned no env; attempting AttachCurrentThread");
        env = AttachThread(vm);
    }
    if (env == nullptr) {
        LOGE("unable to obtain JNIEnv* for current thread");
        return 0;
    }

    auto fn_table = *reinterpret_cast<void ***>(env);  // *JNIEnv -> JNINativeInterface
    if (fn_table == nullptr) {
        LOGE("JNIEnv function table is null");
        return 0;
    }

    uintptr_t raw = reinterpret_cast<uintptr_t>(fn_table[kJniRegisterNatives]);
    return StripPac(raw);
}

// --- Java callback helpers -------------------------------------------------------

// Initialize Java references for callbacks
bool InitializeJavaReferences(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(g_java_mutex);
    
    if (g_hook_data_bridge_class != nullptr) {
        return true;  // Already initialized
    }
    
    // Get VM reference
    if (g_vm == nullptr) {
        env->GetJavaVM(&g_vm);
        if (g_vm == nullptr) {
            LOGE("Failed to get JavaVM");
            return false;
        }
    }
    
    // Find HookDataBridge class
    jclass local_class = env->FindClass("com/example/nathookdemo/HookDataBridge");
    if (local_class == nullptr) {
        LOGE("Failed to find HookDataBridge class");
        return false;
    }
    
    g_hook_data_bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(local_class));
    if (g_hook_data_bridge_class == nullptr) {
        LOGE("Failed to create global reference for HookDataBridge");
        return false;
    }
    env->DeleteLocalRef(local_class);
    
    // Find callback method
    g_on_register_natives_method = env->GetStaticMethodID(
        g_hook_data_bridge_class,
        "onRegisterNativesIntercepted",
        "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[JI)V");
    
    if (g_on_register_natives_method == nullptr) {
        LOGE("Failed to find onRegisterNativesIntercepted method");
        return false;
    }
    
    LOGI("Java references initialized successfully");
    return true;
}

// Get JNIEnv for current thread, attaching if necessary
JNIEnv *GetJNIEnvForCallback() {
    if (g_vm == nullptr) {
        return nullptr;
    }
    
    JNIEnv *env = nullptr;
    jint result = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    
    if (result == JNI_EDETACHED) {
        // Thread not attached, attach it
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("Failed to attach thread");
            return nullptr;
        }
    } else if (result != JNI_OK) {
        LOGE("GetEnv failed with code: %d", result);
        return nullptr;
    }
    
    return env;
}

// Call Java callback with hook data
void CallJavaCallback(const char *processName, const char *className,
                      const JNINativeMethod *methods, jint nMethods, jint result) {
    std::lock_guard<std::mutex> lock(g_java_mutex);
    
    if (g_hook_data_bridge_class == nullptr || g_on_register_natives_method == nullptr) {
        LOGW("Java references not initialized, skipping callback for %s.%s",
             processName ? processName : "?", className ? className : "?");
        return;
    }
    
    JNIEnv *env = GetJNIEnvForCallback();
    if (env == nullptr) {
        LOGE("Failed to get JNIEnv for callback");
        return;
    }
    
    // Create Java strings
    jstring j_process_name = env->NewStringUTF(processName != nullptr ? processName : "");
    jstring j_class_name = env->NewStringUTF(className != nullptr ? className : "");
    
    // Create method arrays
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) {
        LOGE("FindClass(String) failed in callback");
        env->ExceptionClear();
        env->DeleteLocalRef(j_process_name);
        env->DeleteLocalRef(j_class_name);
        return;
    }
    
    jint safeCount = nMethods > 0 ? nMethods : 0;
    jobjectArray j_method_names = env->NewObjectArray(safeCount, stringClass, nullptr);
    jobjectArray j_signatures = env->NewObjectArray(safeCount, stringClass, nullptr);
    jlongArray j_function_pointers = env->NewLongArray(safeCount);
    env->DeleteLocalRef(stringClass);
    
    if (j_method_names == nullptr || j_signatures == nullptr || j_function_pointers == nullptr) {
        LOGE("Failed to allocate Java arrays for callback");
        env->ExceptionClear();
        if (j_method_names) env->DeleteLocalRef(j_method_names);
        if (j_signatures) env->DeleteLocalRef(j_signatures);
        if (j_function_pointers) env->DeleteLocalRef(j_function_pointers);
        env->DeleteLocalRef(j_process_name);
        env->DeleteLocalRef(j_class_name);
        return;
    }
    
    // Fill arrays
    for (jint i = 0; i < safeCount; i++) {
        const char *rawName = methods != nullptr ? methods[i].name : nullptr;
        const char *rawSig = methods != nullptr ? methods[i].signature : nullptr;
        const void *rawPtr = methods != nullptr ? methods[i].fnPtr : nullptr;
        
        jstring name = env->NewStringUTF(rawName != nullptr ? rawName : "");
        jstring sig = env->NewStringUTF(rawSig != nullptr ? rawSig : "");
        jlong fn_ptr = static_cast<jlong>(StripPac(
            reinterpret_cast<uintptr_t>(rawPtr)));
        
        env->SetObjectArrayElement(j_method_names, i, name);
        env->SetObjectArrayElement(j_signatures, i, sig);
        env->SetLongArrayRegion(j_function_pointers, i, 1, &fn_ptr);
        
        if (name) env->DeleteLocalRef(name);
        if (sig) env->DeleteLocalRef(sig);
    }
    
    LOGI("Calling HookDataBridge callback: process=%s class=%s methods=%d ret=%d",
         processName != nullptr ? processName : "?",
         className != nullptr ? className : "?",
         static_cast<int>(safeCount), static_cast<int>(result));
    
    // Call Java method
    env->CallStaticVoidMethod(
        g_hook_data_bridge_class,
        g_on_register_natives_method,
        j_process_name,
        j_class_name,
        j_method_names,
        j_signatures,
        j_function_pointers,
        result);
    
    // Check for exceptions
    if (env->ExceptionCheck()) {
        LOGE("Exception in Java callback");
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    
    // Clean up local references
    env->DeleteLocalRef(j_process_name);
    env->DeleteLocalRef(j_class_name);
    env->DeleteLocalRef(j_method_names);
    env->DeleteLocalRef(j_signatures);
    env->DeleteLocalRef(j_function_pointers);
}

// Get current process name. The module instance lives as long as the process, so the
// cmdline read is cached after the first call (magic statics are thread-safe).
std::string GetCurrentProcessName() {
    static const std::string cached = [] {
        // Read /proc/self/cmdline to get process name
        FILE *fp = fopen("/proc/self/cmdline", "r");
        if (fp == nullptr) {
            return std::string("unknown");
        }

        char buffer[256] = {0};
        if (fgets(buffer, sizeof(buffer), fp) == nullptr) {
            fclose(fp);
            return std::string("unknown");
        }
        fclose(fp);

        // cmdline is null-terminated
        return std::string(buffer);
    }();
    return cached;
}

// --- RegisterNatives replacement ---------------------------------------------------

jint HookedRegisterNatives(JNIEnv *env, jclass clazz,
                           const JNINativeMethod *methods, jint nMethods) {
    // 原方法指针先行取出：任何提前返回路径都必须完成本次注册（hook 只做观察，不拦截）。
    RegisterNativesFn original;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        original = g_original_register_natives;
    }
    if (original == nullptr) {
        LOGE("[register-natives] original RegisterNatives is unavailable");
        return JNI_ERR;
    }

    // 类名：反射元数据已缓存，此处只剩 1 次 CallObjectMethod。
    const char *className = nullptr;
    jstring classNameStr = nullptr;
    jclass classClazz = EnsureClassReflectionCache(env);
    jmethodID getNameMethod = g_get_name_method;
    if (classClazz != nullptr && getNameMethod != nullptr) {
        classNameStr = static_cast<jstring>(env->CallObjectMethod(clazz, getNameMethod));
        if (classNameStr != nullptr) {
            className = env->GetStringUTFChars(classNameStr, nullptr);
        }
    }

    // 指纹去重（热路径第一道闸）：同一进程对同一类以相同方法表重复注册直接短路，
    // 不打逐方法日志、不进 Java 回调（Java 侧 HookDataBridge/HookReportReceiver 的
    // 指纹去重保留为第二道闸，防广播/Provider 双路重复）。
    const RegisterNativesKey key{
        std::hash<std::string_view>{}(className != nullptr ? className : "?"),
        (methods != nullptr && nMethods > 0)
            ? StripPac(reinterpret_cast<uintptr_t>(methods[0].fnPtr))
            : 0,
        nMethods};
    {
        std::lock_guard<std::mutex> lock(g_seen_mutex);
        if (!g_seen_register_natives.insert(key).second) {
            if (className != nullptr) {
                env->ReleaseStringUTFChars(classNameStr, className);
            }
            if (classNameStr != nullptr) {
                env->DeleteLocalRef(classNameStr);
            }
            return original(env, clazz, methods, nMethods);
        }
    }

    // 仅首次注册输出 INFO 摘要；逐方法明细降为 DEBUG（默认不输出），避免
    // chromium 这类单次注册数百方法时刷出数百行日志。
    LOGI("[register-natives] intercepted clazz=%p methods=%d name=%s",
         clazz, static_cast<int>(nMethods), className != nullptr ? className : "?");
    for (jint i = 0; i < nMethods; ++i) {
        const char *name = methods[i].name ? methods[i].name : "(null)";
        const char *sig = methods[i].signature ? methods[i].signature : "(null)";
        LOGD("[register-natives]   [%d] name='%s' sig='%s' fn=%p", static_cast<int>(i),
             name, sig, reinterpret_cast<void *>(StripPac(
                            reinterpret_cast<uintptr_t>(methods[i].fnPtr))));
    }

    // Get current process name（已缓存）
    std::string processName = GetCurrentProcessName();

    // Call Java callback
    CallJavaCallback(processName.c_str(), className, methods, nMethods, 0);

    // Clean up
    if (className != nullptr) {
        env->ReleaseStringUTFChars(classNameStr, className);
    }
    if (classNameStr != nullptr) {
        env->DeleteLocalRef(classNameStr);
    }

    jint ret = original(env, clazz, methods, nMethods);
    LOGI("[register-natives] calling original RegisterNatives -> ret=%d",
         static_cast<int>(ret));
    return ret;
}

bool InstallHook() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_hooked.load()) return true;
    if (g_entries.hookFunc == nullptr) {
        LOGE("hookFunc unavailable (NativeAPIEntries not initialized)");
        return false;
    }

    uintptr_t addr = ResolveRegisterNativesAddress();
    if (addr == 0) {
        LOGE("failed to resolve RegisterNatives address");
        return false;
    }
    LOGI("[register-natives] resolved addr=%p via=env-table (JNIEnv table[%zu])",
         reinterpret_cast<void *>(addr), kJniRegisterNatives);

    void *backup = nullptr;
    int rc = g_entries.hookFunc(reinterpret_cast<void *>(addr),
                                reinterpret_cast<void *>(&HookedRegisterNatives), &backup);
    if (rc != 0 || backup == nullptr) {
        LOGE("hookFunc failed rc=%d backup=%p", rc, backup);
        return false;
    }

    g_original_target = reinterpret_cast<void *>(addr);
    g_original_register_natives =
        reinterpret_cast<RegisterNativesFn>(StripPac(reinterpret_cast<uintptr_t>(backup)));
    g_hooked.store(true);
    LOGI("[register-natives] hooked addr=%p trampoline=%p", reinterpret_cast<void *>(addr),
         backup);
    return true;
}

bool RemoveHook() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (!g_hooked.load()) return true;
    if (g_entries.unhookFunc == nullptr || g_original_target == nullptr) {
        LOGW("unhookFunc unavailable or missing target");
        return false;
    }
    int rc = g_entries.unhookFunc(g_original_target);
    if (rc != 0) {
        LOGE("unhookFunc failed rc=%d", rc);
        return false;
    }
    g_original_register_natives = nullptr;
    g_original_target = nullptr;
    g_hooked.store(false);
    LOGI("[register-natives] unhooked");
    return true;
}

}  // namespace

// Vector resolves this exported entrypoint after the linker loads the registered module ELF.
// The returned callback is retained for the module's lifetime and is notification-only (it does
// not install further hooks); keep the NativeOnModuleLoaded contract for ABI compatibility.
extern "C" NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) {
        LOGE("cannot hook RegisterNatives: NativeAPIEntries is null");
        return nullptr;
    }
    if (entries->version < 2) {
        LOGW("NativeAPIEntries version=%u; expected >= 2", entries->version);
    }

    g_entries = *entries;
    
    // Initialize Java references
    JNIEnv *env = GetJNIEnvForCallback();
    if (env != nullptr && InitializeJavaReferences(env)) {
        LOGI("Java callback references initialized");
    } else {
        LOGW("Failed to initialize Java callback references");
    }
    
    if (!InstallHook()) {
        LOGE("RegisterNatives hook install failed");
    }

    // Notification-only callback kept to preserve the rawVector NativeOnModuleLoaded contract.
    return [](const char *name, void *handle) {
        LOGD("[module-load] name='%s' handle=%p", name ? name : "(null)", handle);
    };
}

// JNI bridge used by the module's own process UI. startRecording installs the hook (idempotent),
// stopRecording removes it — both are safe to call after native_init already installed it.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_nathookdemo_NativeBridge_startRecording(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    LOGI("NativeBridge.startRecording() -> installing RegisterNatives hook");
    
    // Initialize Java references if not already done
    InitializeJavaReferences(env);
    
    return InstallHook() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_nathookdemo_NativeBridge_stopRecording(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    LOGI("NativeBridge.stopRecording() -> removing RegisterNatives hook");
    return RemoveHook() ? JNI_TRUE : JNI_FALSE;
}
