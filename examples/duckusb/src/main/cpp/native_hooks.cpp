#include <jni.h>
#include <sys/system_properties.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "vector_native.h"
#include "Logger.h"

// Ported from DuckUSB (github.com/Bouteillepleine/DuckUSB) — see README.md.
//
// Changes from the upstream file, all of them forced by Vector's module ABI:
//
//  1. `entries->hook_func` -> `entries->hookFunc` (same struct, CamelCase in Vector).
//  2. The JNI method below is bound through RegisterNatives inside native_init instead of
//     being reachable by symbol name. Under LSPosed the library is loaded by the Android
//     linker with System.loadLibrary(), which resolves Java_com_..._setProps automatically.
//     Vector links the ELF from sealed SharedMemory through its own loader and never registers
//     it with the linker, so no JNI symbol is discoverable by name and the Java declaration
//     fails with UnsatisfiedLinkError until we bind it ourselves.
//  3. The JavaVM is bootstrapped from libart's dynamic symbol table (JNI_GetCreatedJavaVMs)
//     rather than taken from a JNIEnv argument, because Vector's native_init signature
//     carries no env. This mirrors rawVector's get_or_init_vm / rustFrida's resolve.rs; the
//     Android linker will not hand out APEX symbols through dlsym, so the ELF is walked by
//     hand. See GetJavaVm() below.
//  4. native_init installs the hooks only once per process. Vector's hook engine REJECTS a
//     duplicate target (it does not chain hooks), so a second install attempt would fail
//     loudly rather than layer up. The guard also documents the intent: the prop overrides are
//     data (republished through setProps), the hooks are structure.
//
// Upstream's own notes on the design are kept below: the hooks are lock-free, and NOT hooking
// __system_property_find is deliberate.

// ---------------------------------------------------------------------------------------------
// JavaVM bootstrap (Vector has no linker: get it from libart's .dynsym, not from dlsym)
// ---------------------------------------------------------------------------------------------

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
            out->dynamic = reinterpret_cast<const ElfW(Dyn) *>(out->base + phdr->p_vaddr);
            out->dynamic_count = phdr->p_memsz / sizeof(ElfW(Dyn));
            break;
        }
    }
    return 0;
}

/**
 * Resolve a symbol from libart.so's in-memory dynamic symbol table.
 *
 * Mirrors rawVector's get_or_init_vm / rustFrida's elf_find_symbols_in_dynamic_memory: walk
 * PT_DYNAMIC for DT_SYMTAB/DT_STRTAB/DT_STRSZ and scan .dynsym. libart is an APEX module and its
 * symbols are not in the app's lookup scope, so dlopen("libart.so") / dlsym(RTLD_DEFAULT, ...)
 * both come up empty.
 */
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
    // DT_SYMTAB/DT_STRTAB d_ptr are load-bias-relative in the in-memory PT_DYNAMIC array;
    // DT_STRSZ is a size and is used verbatim.
    auto dynamic_ptr = [&info](uint64_t value) -> uintptr_t {
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
        LOGE("libart dynamic symbol table incomplete");
        return 0;
    }

    // No explicit nsyms from PT_DYNAMIC alone, but .dynsym ends where .dynstr begins.
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
        if (strcmp(strtab + sym->st_name, name) == 0) {
            return info.base + sym->st_value;
        }
    }
    LOGE("symbol '%s' not found in libart .dynsym", name);
    return 0;
}

JavaVM *g_vm = nullptr;

/** Cached JavaVM; bootstraps through libart's dynamic symbol table on first use. */
JavaVM *GetJavaVm() {
    if (g_vm != nullptr) return g_vm;
    using JniGetCreatedJavaVMs = jint (*)(JavaVM **, jsize, jsize *);
    uintptr_t addr = DlsymLibartDynamic("JNI_GetCreatedJavaVMs");
    if (addr == 0) return nullptr;
    auto get_vms = reinterpret_cast<JniGetCreatedJavaVMs>(addr);
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || count == 0 || vm == nullptr) {
        LOGE("JNI_GetCreatedJavaVMs failed (count=%d)", static_cast<int>(count));
        return nullptr;
    }
    g_vm = vm;
    return vm;
}

/** Current thread's JNIEnv, attaching the thread if it is not attached yet. */
JNIEnv *GetJNIEnv() {
    JavaVM *vm = GetJavaVm();
    if (vm == nullptr) return nullptr;
    JNIEnv *env = nullptr;
    jint result = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("GetJNIEnv: AttachCurrentThread failed");
            return nullptr;
        }
    } else if (result != JNI_OK) {
        LOGE("GetJNIEnv: GetEnv failed code=%d", static_cast<int>(result));
        return nullptr;
    }
    return env;
}

}  // namespace


// Prop overrides pushed from Kotlin (NativeProps.setProps), published LOCK-FREE.
//
// The hooks below intercept libc property reads. liblog (LOGD) and every JNI call
// ALSO read system properties internally — so if a hook took a lock while setProps
// (or a log line) held it, the same thread would re-enter and self-deadlock. That
// exact bug froze SystemUI. So: NO mutex. The snapshot is immutable and published
// via an atomic pointer; readers load-acquire it and never block. Old snapshots are
// intentionally leaked (setProps runs at most a couple times per process), which keeps
// any pointer a concurrent hook may still be reading valid forever — no use-after-free.
using PropMap = std::unordered_map<std::string, std::string>;

// bionic exposes a property two ways: by NAME (__system_property_get) and by HANDLE —
// __system_property_find gives you an opaque prop_info*, which you then read through
// __system_property_read_callback (or the legacy __system_property_read). Both routes
// need covering, so the snapshot carries both indexes: by name, and by the prop_info*
// each name resolves to. Handles are stable for the life of a property, so resolving
// them once at publish time is sound; a property that does not exist yet simply has no
// handle entry and is still covered by the name route.
struct Override {
    const prop_info *pi;
    std::string value;
};
struct Snapshot {
    PropMap byName;
    std::vector<Override> byInfo;  // at most a handful; linear scan beats hashing here
};
static std::atomic<const Snapshot *> gSnap{nullptr};

static int (*orig_system_property_get)(const char *name, char *value) = nullptr;
static void (*orig_system_property_read_callback)(
        const prop_info *pi,
        void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial),
        void *cookie) = nullptr;
static int (*orig_system_property_read)(const prop_info *pi, char *name, char *value) = nullptr;

/** Our value for this handle, or nullptr if we don't spoof it. */
static const std::string *spoofed_for(const Snapshot *s, const prop_info *pi) {
    if (!s || !pi) return nullptr;
    for (const Override &o : s->byInfo) {
        if (o.pi == pi) return &o.value;
    }
    return nullptr;
}

/** Bounded copy into a caller-supplied PROP_VALUE_MAX buffer. Never strcpy in a libc hook. */
static int copy_bounded(char *dst, const std::string &src) {
    size_t n = src.size();
    if (n > PROP_VALUE_MAX - 1) n = PROP_VALUE_MAX - 1;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
    return static_cast<int>(n);
}

static int hooked_system_property_get(const char *name, char *value) {
    const Snapshot *s = gSnap.load(std::memory_order_acquire);
    if (name && value && s) {
        auto it = s->byName.find(name);
        if (it != s->byName.end()) return copy_bounded(value, it->second);
    }
    // A hook whose backup never got installed must not jump through a null pointer.
    if (!orig_system_property_get) {
        if (value) value[0] = '\0';
        return 0;
    }
    return orig_system_property_get(name, value);
}

// Re-invoke the caller's callback with our value, keeping the REAL name and serial that
// bionic hands us. Substituting the value alone keeps the property looking entirely normal:
// it still exists, still has a plausible serial, and only reads differently.
struct CbCtx {
    void (*cb)(void *cookie, const char *name, const char *value, uint32_t serial);
    void *cookie;
    const char *value;
};
static void spoof_trampoline(void *cookie, const char *name, const char *value, uint32_t serial) {
    (void) value;  // the real value, deliberately dropped
    auto *ctx = static_cast<CbCtx *>(cookie);
    ctx->cb(ctx->cookie, name, ctx->value, serial);
}

// THE path that matters on modern Android: SystemProperties.get() and libbase's
// GetProperty() both resolve a handle and read it through this. The previous build hooked
// __system_property_find and returned nullptr for our keys, which made every caller on this
// route treat the property as ABSENT — so a native detector read sys.usb.state as "" rather
// than "mtp". An empty sys.usb.state is not a smaller lie than the truth, it is a stranger
// one. Now the handle resolves normally and only the value is rewritten.
static void hooked_system_property_read_callback(
        const prop_info *pi,
        void (*callback)(void *cookie, const char *name, const char *value, uint32_t serial),
        void *cookie) {
    if (!orig_system_property_read_callback) return;
    const std::string *spoof = spoofed_for(gSnap.load(std::memory_order_acquire), pi);
    if (spoof && callback) {
        CbCtx ctx{callback, cookie, spoof->c_str()};
        orig_system_property_read_callback(pi, spoof_trampoline, &ctx);
        return;
    }
    orig_system_property_read_callback(pi, callback, cookie);
}

// Legacy handle read. Rarely used now, but leaving it unhooked would make it the one route
// that still reports the truth — a partial spoof is its own tell.
static int hooked_system_property_read(const prop_info *pi, char *name, char *value) {
    if (!orig_system_property_read) return 0;
    int n = orig_system_property_read(pi, name, value);
    const std::string *spoof = spoofed_for(gSnap.load(std::memory_order_acquire), pi);
    if (spoof && value) return copy_bounded(value, *spoof);
    return n;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_duckusb_NativeProps_setProps(JNIEnv *env, jobject /*thiz*/, jobject props) {
    // Build a fresh snapshot with NO lock held. The JNI calls here read properties
    // internally and re-enter the hooks above — that is fine now: the hooks are
    // lock-free and simply see the previous snapshot until we publish this one.
    auto *next = new Snapshot();
    if (props != nullptr) {
        jclass mapClass = env->FindClass("java/util/Map");
        jmethodID entrySetMethod = env->GetMethodID(mapClass, "entrySet", "()Ljava/util/Set;");
        jobject entrySet = env->CallObjectMethod(props, entrySetMethod);

        jclass setClass = env->FindClass("java/util/Set");
        jmethodID iteratorMethod = env->GetMethodID(setClass, "iterator", "()Ljava/util/Iterator;");
        jobject iterator = env->CallObjectMethod(entrySet, iteratorMethod);

        jclass iteratorClass = env->FindClass("java/util/Iterator");
        jmethodID hasNextMethod = env->GetMethodID(iteratorClass, "hasNext", "()Z");
        jmethodID nextMethod = env->GetMethodID(iteratorClass, "next", "()Ljava/lang/Object;");

        jclass entryClass = env->FindClass("java/util/Map$Entry");
        jmethodID getKeyMethod = env->GetMethodID(entryClass, "getKey", "()Ljava/lang/Object;");
        jmethodID getValueMethod = env->GetMethodID(entryClass, "getValue", "()Ljava/lang/Object;");

        while (env->CallBooleanMethod(iterator, hasNextMethod)) {
            jobject entry = env->CallObjectMethod(iterator, nextMethod);
            auto keyString = static_cast<jstring>(env->CallObjectMethod(entry, getKeyMethod));
            auto valueString = static_cast<jstring>(env->CallObjectMethod(entry, getValueMethod));
            const char *key = env->GetStringUTFChars(keyString, nullptr);
            const char *value = env->GetStringUTFChars(valueString, nullptr);

            next->byName.emplace(key, value);

            env->ReleaseStringUTFChars(keyString, key);
            env->ReleaseStringUTFChars(valueString, value);
            env->DeleteLocalRef(entry);
            env->DeleteLocalRef(keyString);
            env->DeleteLocalRef(valueString);
        }

        env->DeleteLocalRef(mapClass);
        env->DeleteLocalRef(entrySet);
        env->DeleteLocalRef(setClass);
        env->DeleteLocalRef(iterator);
        env->DeleteLocalRef(iteratorClass);
        env->DeleteLocalRef(entryClass);
    }

    // Resolve each name to its handle, so the prop_info* routes can be spoofed too.
    // __system_property_find is NOT hooked (only the reads are), so this calls straight
    // into libc — no re-entry. A name with no handle yet stays covered by the name route.
    for (const auto &kv : next->byName) {
        if (const prop_info *pi = __system_property_find(kv.first.c_str())) {
            next->byInfo.push_back({pi, kv.second});
        }
    }

    // Publish. Any hook mid-read still holds the previous pointer, which we never free.
    gSnap.store(next, std::memory_order_release);
    LOGD("setProps: %zu overrides published, %zu handles resolved",  // safe: no lock held
         next->byName.size(), next->byInfo.size());
}

// Our own package. The framework loads a module into its own app process unconditionally —
// that is not something the scope list can turn off — so this UI always gets native_init.
// Hooking libc there is pure downside: nothing in our own process should be lied to (the
// readings card exists to show the REAL device state), and an inline hook on a hot libc
// entry point is the one thing in this process that can kill it. That is issue #2 upstream:
// on a Nothing A065 / Android 16 build the very first property read off the EmojiCompatInit
// thread landed in the trampoline and took SIGILL (ILL_ILLOPC), so the UI died seconds
// after launch — in a process that never needed the hook in the first place.
static const char kOwnPackage[] = "com.example.duckusb";

// Defence-in-depth: never install the libc hooks in our own UI, nor in core OS processes
// (mirrors the Kotlin SKIP_SPOOF_PACKAGES). The lock-free hooks are already safe with an
// empty map, but not touching libc at all removes both the crash surface here and any
// footprint in system_server / SystemUI.
// Fails safe: if the process name can't be read (very early load), we DON'T skip.
static bool should_skip_hooks(const char **reason) {
    char cmd[128] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);  // raw syscall, no FILE lock
    if (fd < 0) return false;
    ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
    close(fd);
    if (n <= 0) return false;
    cmd[n] = '\0';  // first NUL-separated token is the process name

    // A private sub-process reports "<package>:<name>"; compare on the package half so
    // com.example.duckusb:anything is covered too. strchr stops at the token's NUL.
    char *colon = strchr(cmd, ':');
    if (colon) *colon = '\0';

    if (strcmp(cmd, kOwnPackage) == 0) {
        *reason = "own process";
        return true;
    }
    static const char *core[] = {
        "system_server", "android",
        "com.android.systemui", "com.android.settings",
        "com.android.shell", "com.android.phone",
    };
    for (const char *c : core) {
        if (strcmp(cmd, c) == 0) {
            *reason = "core OS process";
            return true;
        }
    }
    return false;
}

// Binds NativeProps.setProps to the Java declaration, which no linker will do for us here.
// Called from native_init, i.e. from a module-class context, so FindClass resolves this
// module's own class rather than the framework's.
static bool bind_native_methods(JNIEnv *env) {
    if (env == nullptr) {
        LOGE("bind_native_methods: no JNIEnv (JavaVM bootstrap failed)");
        return false;
    }
    jclass cls = env->FindClass("com/example/duckusb/NativeProps");
    if (cls == nullptr) {
        LOGE("bind_native_methods: NativeProps class not found");
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
        return false;
    }
    static const JNINativeMethod methods[] = {
        {"setProps", "(Ljava/util/Map;)V",
         reinterpret_cast<void *>(&Java_com_example_duckusb_NativeProps_setProps)},
    };
    jint rc = env->RegisterNatives(cls, methods, sizeof(methods) / sizeof(methods[0]));
    env->DeleteLocalRef(cls);
    if (rc != JNI_OK) {
        if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
        LOGE("bind_native_methods: RegisterNatives failed rc=%d", static_cast<int>(rc));
        return false;
    }
    LOGD("bind_native_methods: NativeProps.setProps registered");
    return true;
}

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) {
        LOGE("native_init: entries is null");
        return nullptr;
    }
    if (entries->version < 2) {
        LOGD("native_init: unexpected API version %u (expected >= 2)", entries->version);
    }

    // Always bind the Java method first, even in the processes whose libc we leave alone:
    // setProps is called from the UI process, and a missing binding there is an
    // UnsatisfiedLinkError on the user's first screen.
    JNIEnv *env = GetJNIEnv();
    bind_native_methods(env);

    const char *reason = "";
    if (should_skip_hooks(&reason)) {
        LOGD("native_init: %s, skipping libc hooks", reason);
        return [](const char *name, void *handle) {};
    }

    if (entries->hookFunc == nullptr) {
        LOGE("native_init: hookFunc is null, cannot install property hooks");
        return [](const char *name, void *handle) {};
    }

    LOGD("native_init");
    HookFunType hook = entries->hookFunc;
    // Three read routes, one hook each. __system_property_find is deliberately NOT hooked:
    // handles must resolve normally, and the reads are where the value gets rewritten.
    // Log the hooker's verdict — a failed inline hook is otherwise indistinguishable from a
    // working one until a property read misbehaves, which is a miserable thing to debug from
    // a crash report (see issue #2 upstream).
    int rc_get = hook((void *) __system_property_get, (void *) hooked_system_property_get,
                      (void **) &orig_system_property_get);
    int rc_cb = hook((void *) __system_property_read_callback,
                     (void *) hooked_system_property_read_callback,
                     (void **) &orig_system_property_read_callback);
    int rc_read = hook((void *) __system_property_read, (void *) hooked_system_property_read,
                       (void **) &orig_system_property_read);
    LOGD("native_init: hook rc get=%d read_callback=%d read=%d", rc_get, rc_cb, rc_read);

    // Notification-only, per the NativeOnModuleLoaded contract. Non-null so the framework
    // records this module as having a load callback; it installs nothing further.
    return [](const char *name, void *handle) {
        LOGD("[module-load] name='%s' handle=%p", name ? name : "(null)", handle);
    };
}
