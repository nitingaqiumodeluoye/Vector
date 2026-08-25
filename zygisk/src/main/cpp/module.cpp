#include <common/config.h>
#include <common/logging.h>
#include <core/context.h>
#include <core/native_api.h>
#include <elf/elf_image.h>
#include <elf/symbol_cache.h>
#include <jni/jni_bridge.h>
#include <jni/jni_hooks.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <zygisk.hpp>

#include "ipc_bridge.h"

namespace vector::native::module {

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

static constexpr const char *kZygiskVmaName = "wwb_zygisk";
static constexpr useconds_t kLateVmaNameDelaysUs[] = {
    10 * 1000,
    50 * 1000,
    200 * 1000,
    1000 * 1000,
    3000 * 1000,
};

// rustFrida reference:
// - loader/helpers/bootstrapper.c frida_bootstrap names its temporary executable mapping
//   "wwb_loader" with PR_SET_VMA_ANON_NAME.
// - loader/helpers/rustfrida-loader.c rustfrida_set_vma_name applies the same naming to
//   agent mappings derived from the "wwb_so" memfd.
// Vector is loaded by the Zygisk runtime before our code runs, so we cannot turn this VMA into
// a memfd here. We can still name the already-loaded anonymous RX segment that contains this
// module's code, matching rustFrida's wwb_* VMA naming style.
static void NameOwnZygiskRxVma() {
    const auto own_pc = reinterpret_cast<uintptr_t>(&NameOwnZygiskRxVma);

    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGW("[vma-name] failed to open /proc/self/maps");
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), maps)) {
        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        unsigned long inode = 0;
        char perms[5] = {};
        char dev[32] = {};

        if (sscanf(line, "%lx-%lx %4s %lx %31s %lu", &start, &end, perms, &offset, dev,
                   &inode) != 6) {
            continue;
        }

        if (own_pc < static_cast<uintptr_t>(start) || own_pc >= static_cast<uintptr_t>(end)) {
            continue;
        }

        if (perms[2] != 'x') {
            LOGW("[vma-name] own pc is not in executable VMA: {}", line);
            break;
        }

        if (strcmp(dev, "00:00") != 0 || inode != 0) {
            LOGD("[vma-name] own zygisk RX VMA is not anonymous; skip naming: {}", line);
            break;
        }

        const unsigned long size = end - start;
        errno = 0;
        const int ret = prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, start, size,
                              reinterpret_cast<unsigned long>(kZygiskVmaName));
        if (ret == 0) {
            LOGI("[vma-name] named own zygisk RX VMA '{}' range={:x}-{:x}", kZygiskVmaName,
                 start, end);
        } else {
            LOGW("[vma-name] failed to name own zygisk RX VMA '{}' range={:x}-{:x} errno={}",
                 kZygiskVmaName, start, end, errno);
        }
        break;
    }

    fclose(maps);
}

static void *LateNameOwnZygiskRxVmaWorker(void *) {
    NameOwnZygiskRxVma();

    for (useconds_t delay_us : kLateVmaNameDelaysUs) {
        usleep(delay_us);
        NameOwnZygiskRxVma();
    }

    return nullptr;
}

static void ScheduleLateNameOwnZygiskRxVma() {
    pthread_t thread{};
    errno = 0;
    const int ret = pthread_create(&thread, nullptr, LateNameOwnZygiskRxVmaWorker, nullptr);
    if (ret == 0) {
        pthread_detach(thread);
        LOGD("[vma-name] scheduled late own zygisk RX VMA naming");
    } else {
        LOGW("[vma-name] failed to schedule late own zygisk RX VMA naming errno={}", ret);
    }
}

// Snapshot anonymous executable mappings at injection boundaries. These maps have no
// pathname/inode, so recording them around each native stage is the only reliable way
// to distinguish Vector allocation from a later app/library initializer.
static void TraceAnonymousExecMaps(const char *stage) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGW("[exec-map-native] stage={} failed to open /proc/self/maps", stage);
        return;
    }
    char line[512];
    size_t count = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, " r-xp 00000000 00:00 0")) {
            ++count;
            LOGI("[exec-map-native] stage={} map={}", stage, line);
        }
    }
    fclose(maps);
    LOGI("[exec-map-native] stage={} pid={} count={}", stage, getpid(), count);
}

static long long StartupElapsedUs(
    const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

// --- Process UID Constants ---
// Values used to identify special Android processes to avoid injection.
// https://android.googlesource.com/platform/system/core/+/master/libcutils/include/private/android_filesystem_config.h

// The range of UIDs used for isolated processes (e.g., web renderers, WebView).
constexpr int FIRST_ISOLATED_UID = 99000;
constexpr int LAST_ISOLATED_UID = 99999;

// The range of UIDs used for application zygotes, which are also not targets.
constexpr int FIRST_APP_ZYGOTE_ISOLATED_UID = 90000;
constexpr int LAST_APP_ZYGOTE_ISOLATED_UID = 98999;

// UID for the process responsible for creating shared RELRO files.
constexpr int SHARED_RELRO_UID = 1037;

// Android uses this to separate users. UID = AppID + UserID * 10000.
constexpr int PER_USER_RANGE = 100000;

// Defined via CMake generated marcos
constexpr uid_t kHostPackageUid = INJECTED_PACKAGE_UID;
#define VECTOR_STRINGIFY_IMPL(value) #value
#define VECTOR_STRINGIFY(value) VECTOR_STRINGIFY_IMPL(value)
const char *const kHostPackageName = VECTOR_STRINGIFY(INJECTED_PACKAGE_NAME);
const char *const kManagerPackageName = VECTOR_STRINGIFY(MANAGER_PACKAGE_NAME);
#undef VECTOR_STRINGIFY
#undef VECTOR_STRINGIFY_IMPL
constexpr uid_t GID_INET = 3003;  // Android's Internet group ID.

enum RuntimeFlags : uint32_t {
    // Flags defined by NeoZygisk
    LATE_INJECT = 1 << 30,
};

// A simply ConfigBridge implemnetation holding obfuscation maps in memory
using obfuscation_map_t = std::map<std::string, std::string>;
class ConfigImpl : public ConfigBridge {
public:
    inline static void Init() { instance_ = std::make_unique<ConfigImpl>(); }

    virtual obfuscation_map_t &obfuscation_map() override { return obfuscation_map_; }

    virtual void obfuscation_map(obfuscation_map_t m) override { obfuscation_map_ = std::move(m); }

private:
    ConfigImpl() = default;

    friend std::unique_ptr<ConfigImpl> std::make_unique<ConfigImpl>();
    obfuscation_map_t obfuscation_map_;
};

/**
 * @class VectorModule
 * @brief The core implementation of the Zygisk module for the Vector framework.
 *
 * This class is the main entry point for Zygisk. It inherits from:
 * - zygisk::ModuleBase:      To receive lifecycle callbacks from the Zygisk loader.
 * - vector::native::Context: To gain the core injection capabilities (DEX loading, ART hooking)
 *                            from the 'native' library.
 *
 * It orchestrates the injection process by deciding which processes to target,
 * using the IPCBridge to fetch the framework from the manager service, and then
 * using the Context base to perform the actual injection.
 */
class VectorModule : public zygisk::ModuleBase, public vector::native::Context {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override;
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override;
    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override;
    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override;
    void postServerSpecialize(const zygisk::ServerSpecializeArgs *args) override;

protected:
    /**
     * @brief Provides the concrete implementation for loading the framework DEX.
     *
     * This method is a pure virtual in the native::core::Context base class and
     * must be implemented here.
     * It uses an InMemoryDexClassLoader to load our framework into the target process.
     */
    void LoadDex(JNIEnv *env, PreloadedDex &&dex) override;

    /**
     * @brief Provides the concrete implementation for finding the Java entry
     * class.
     *
     * This method is also a pure virtual in the base class.
     * It uses the obfuscation map to determine the real entry class name and
     * finds it in the ClassLoader we created in LoadDex.
     */
    void SetupEntryClass(JNIEnv *env) override;

private:
    /**
     * @brief Encapsulates the logic for telling Zygisk whether to unload our library.
     *
     * If we don't inject into a process, we allow Zygisk to dlclose our .so.
     * Otherwise, we MUST prevent this.
     * @param unload True to allow unloading, false to prevent it.
     */
    void SetAllowUnload(bool unload);

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;

    // --- ART Hooker Configuration ---
    const lsplant::InitInfo init_info_{
        .inline_hooker =
            [](auto target, auto replace) {
                void *backup = nullptr;
                return HookInline(target, replace, &backup) == 0 ? backup : nullptr;
            },
        .inline_unhooker = [](auto target) { return UnhookInline(target) == 0; },
        .art_symbol_resolver =
            [](auto symbol) { return ElfSymbolCache::GetArt()->getSymbAddress(symbol); },
        .art_symbol_prefix_resolver =
            [](auto symbol) { return ElfSymbolCache::GetArt()->getSymbPrefixFirstAddress(symbol); },
        .generated_class_name = "Vector_",
        .generated_source_name = "WxShadow",
    };

    // State managed within the class instance for each forked process.
    bool should_inject_ = false;
    bool is_manager_app_ = false;
};

// =========================================================================================
// Implementation of VectorModule
// =========================================================================================

void VectorModule::LoadDex(JNIEnv *env, PreloadedDex &&dex) {
    LOGV("Loading framework DEX into memory (size: {}).", dex.size());

    // Get the system ClassLoader. This will be the parent of our new loader.
    auto classloader_class = lsplant::JNI_FindClass(env, "java/lang/ClassLoader");
    if (!classloader_class) {
        LOGE("Failed to find java.lang.ClassLoader");
        return;
    }
    auto getsyscl_mid = lsplant::JNI_GetStaticMethodID(
        env, classloader_class.get(), "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    auto system_classloader =
        lsplant::JNI_CallStaticObjectMethod(env, classloader_class.get(), getsyscl_mid);
    if (!system_classloader) {
        LOGE("Failed to get SystemClassLoader");
        return;
    }

    // Create a Java ByteBuffer wrapping our in-memory DEX data.
    auto byte_buffer_class = lsplant::JNI_FindClass(env, "java/nio/ByteBuffer");
    if (!byte_buffer_class) {
        LOGE("Failed to find java.nio.ByteBuffer");
        return;
    }
    auto dex_buffer =
        lsplant::ScopedLocalRef(env, env->NewDirectByteBuffer(dex.data(), dex.size()));
    if (!dex_buffer) {
        LOGE("Failed to create DirectByteBuffer for DEX.");
        return;
    }

    // Create an InMemoryDexClassLoader instance.
    auto in_memory_cl_class = lsplant::JNI_FindClass(env, "dalvik/system/InMemoryDexClassLoader");
    if (!in_memory_cl_class) {
        LOGE("Failed to find InMemoryDexClassLoader.");
        return;
    }
    auto init_mid = lsplant::JNI_GetMethodID(env, in_memory_cl_class.get(), "<init>",
                                             "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (!init_mid) {
        LOGE("Failed to find InMemoryDexClassLoader constructor.");
        return;
    }

    auto new_cl =
        lsplant::ScopedLocalRef(env, env->NewObject(in_memory_cl_class.get(), init_mid,
                                                    dex_buffer.get(), system_classloader.get()));
    if (env->ExceptionCheck() || !new_cl) {
        LOGE("Failed to create InMemoryDexClassLoader instance.");
        env->ExceptionClear();
        return;
    }

    // Store a global reference to our new ClassLoader.
    inject_class_loader_ = env->NewGlobalRef(new_cl.get());
    LOGV("Framework ClassLoader created successfully.");
}

void VectorModule::SetupEntryClass(JNIEnv *env) {
    if (!inject_class_loader_) {
        LOGE("Cannot setup entry class: ClassLoader is null.");
        return;
    }

    // Use the obfuscation map from the config to get the real class name.
    const auto &obfs_map = ConfigBridge::GetInstance()->obfuscation_map();
    std::string entry_class_name;
    entry_class_name = obfs_map.at("org.matrix.vector.core.") + "Main";

    // We must find the class through our custom ClassLoader.
    auto entry_class = this->FindClassFromLoader(env, inject_class_loader_, entry_class_name);
    if (!entry_class) {
        LOGE("Failed to find entry class '{}' in the loaded DEX.", entry_class_name.c_str());
        return;
    }

    // Store a global reference to the entry class.
    entry_class_ = lsplant::JNI_NewGlobalRef(env, entry_class);
    LOGV("Framework entry class '{}' located.", entry_class_name.c_str());
}

void VectorModule::onLoad(zygisk::Api *api, JNIEnv *env) {
    this->api_ = api;
    this->env_ = env;

    NameOwnZygiskRxVma();

    // Create two singlton instances for classes Context and ConfigBridge
    instance_.reset(this);
    ConfigImpl::Init();
    LOGD("Vector Zygisk module loaded");
}

void VectorModule::preAppSpecialize(zygisk::AppSpecializeArgs *args) {
    NameOwnZygiskRxVma();

    // Reset state for this new process fork.
    should_inject_ = false;
    is_manager_app_ = false;
    jni::SetManagerProcess(false);
    jni::SetLsplantInitialized(false);

    // --- Manager App Special Handling ---
    // We identify our manager app by a special UID and
    // grant it internet permissions by adding it to the INET group.
    if (args->uid == kHostPackageUid) {
        lsplant::JUTFString nice_name_str(env_, args->nice_name);
        if (nice_name_str.get() == std::string(kManagerPackageName)) {
            LOGI("Manager app detected. Granting internet permissions.");
            is_manager_app_ = true;
            jni::SetManagerProcess(true);

            // Add GID_INET to the GID list.
            int original_gids_count = env_->GetArrayLength(args->gids);
            jintArray new_gids = env_->NewIntArray(original_gids_count + 1);
            if (env_->ExceptionCheck()) {
                LOGE("Failed to create new GID array for manager.");
                env_->ExceptionClear();  // Clear exception to prevent a crash.
                return;
            }

            jint *gids_array = env_->GetIntArrayElements(args->gids, nullptr);
            env_->SetIntArrayRegion(new_gids, 0, original_gids_count, gids_array);
            env_->ReleaseIntArrayElements(args->gids, gids_array, JNI_ABORT);

            jint inet_gid = GID_INET;
            env_->SetIntArrayRegion(new_gids, original_gids_count, 1, &inet_gid);

            // The new array is only seen by the native specialization, which calls setgroups().
            // Once it returns, Zygote#forkAndSpecialize keeps using the array it passed in:
            //
            //   NetworkUtils.setAllowNetworkingForProcess(containsInetGid(gids));
            //
            // That flag lives in libnetd_client and gates the whole process: without it,
            // socket() and dns_open_proxy() fail with EPERM no matter which groups we really
            // belong to. So overwrite the first entry of the original array as well, to make
            // containsInetGid() see the INET group. Dropping this slot is harmless: the array
            // has no further use once specialization is done.
            if (original_gids_count > 0) {
                env_->SetIntArrayRegion(args->gids, 0, 1, &inet_gid);
            }

            args->nice_name = env_->NewStringUTF(kHostPackageName);
            args->gids = new_gids;
        }
    }

    IPCBridge::GetInstance().Initialize(env_);

    // --- Injection Decision Logic ---
    // Determine if the current process is a valid target for injection.
    lsplant::JUTFString nice_name_str(env_, args->nice_name);
    const bool is_child_zygote = args->is_child_zygote && *args->is_child_zygote;
    const uid_t app_id = args->uid % PER_USER_RANGE;

    LOGI("preAppSpecialize candidate '{}' uid={} app_id={} child_zygote={} has_app_data_dir={}",
         nice_name_str.get(), args->uid, app_id, is_child_zygote,
         args->app_data_dir != nullptr);

    // An app without a data directory cannot be a target.
    if (!args->app_data_dir) {
        LOGI("Skipping injection for '{}': no app_data_dir.", nice_name_str.get());
        return;
    }

    // rustFrida-style package targeting should not require users to name implementation
    // processes. Some protected apps create app-zygote/child-zygote processes whose final
    // process names are still package-scoped. Do not reject them here; let the daemon's scope
    // decision in postAppSpecialize decide whether a binder is returned.
    if (is_child_zygote) {
        LOGI("Allowing child zygote candidate '{}' to continue to daemon scope check.",
             nice_name_str.get());
    }

    // Skip isolated processes, which are heavily sandboxed.
    if ((app_id >= FIRST_ISOLATED_UID && app_id <= LAST_ISOLATED_UID) ||
        (app_id >= FIRST_APP_ZYGOTE_ISOLATED_UID && app_id <= LAST_APP_ZYGOTE_ISOLATED_UID) ||
        app_id == SHARED_RELRO_UID) {
        LOGI("Skipping injection for '{}': is an isolated process (UID: {}).", nice_name_str.get(),
             app_id);
        return;
    }

    // If we passed all checks, mark this process for injection.
    should_inject_ = true;
    LOGI("Process '{}' (UID: {}) is marked for injection.", nice_name_str.get(), args->uid);
}

void VectorModule::postAppSpecialize(const zygisk::AppSpecializeArgs *args) {
    if (!should_inject_) {
        lsplant::JUTFString nice_name_str(env_, args->nice_name);
        LOGI("postAppSpecialize skipped for '{}': not marked for injection.",
             nice_name_str.get());
        SetAllowUnload(true);  // Not a target, allow module to be unloaded.
        return;
    }

    NameOwnZygiskRxVma();

    if (is_manager_app_) {
        args->nice_name = env_->NewStringUTF(kManagerPackageName);
    }

    // --- Framework Injection ---
    lsplant::JUTFString nice_name_str(env_, args->nice_name);
    const auto startup_start = std::chrono::steady_clock::now();
    LOGI("[startup-trace] native postAppSpecialize begin package='{}'", nice_name_str.get());
    LOGD("Asking the daemon about '{}'.", nice_name_str.get());
    TraceAnonymousExecMaps("postAppSpecialize:entry");

    auto &ipc_bridge = IPCBridge::GetInstance();
    auto stage_start = std::chrono::steady_clock::now();
    auto binder = ipc_bridge.RequestAppBinder(env_, args->nice_name);
    if (!binder) {
        // Usually because nothing has it in scope, but the daemon may also not be up yet or have
        // registered this process already. Only it knows which, and it logs the reason itself.
        LOGI("[startup-trace] native stage end=RequestAppBinder package='{}' "
             "elapsedUs={} result=none",
             nice_name_str.get(), StartupElapsedUs(stage_start));
        LOGD("Not injecting '{}': the daemon has no binder for it.", nice_name_str.get());
        NameOwnZygiskRxVma();
        SetAllowUnload(true);
        return;
    }
    LOGI("[startup-trace] native stage end=RequestAppBinder package='{}' "
         "elapsedUs={} result=ok",
         nice_name_str.get(), StartupElapsedUs(stage_start));
    NameOwnZygiskRxVma();
    TraceAnonymousExecMaps("postAppSpecialize:after-binder");

    // Fetch resources from the manager service.
    stage_start = std::chrono::steady_clock::now();
    auto [dex_fd, dex_size] = ipc_bridge.FetchFrameworkDex(env_, binder.get());
    if (dex_fd < 0) {
        LOGI("[startup-trace] native stage end=FetchFrameworkDex package='{}' "
             "elapsedUs={} result=failed",
             nice_name_str.get(), StartupElapsedUs(stage_start));
        LOGE("Failed to fetch framework DEX for '{}'.", nice_name_str.get());
        NameOwnZygiskRxVma();
        SetAllowUnload(true);
        return;
    }

    auto obfs_map = ipc_bridge.FetchObfuscationMap(env_, binder.get());
    ConfigBridge::GetInstance()->obfuscation_map(std::move(obfs_map));
    LOGI("[startup-trace] native stage end=FetchFrameworkDex package='{}' "
         "elapsedUs={} result=ok size={}",
         nice_name_str.get(), StartupElapsedUs(stage_start), dex_size);
    TraceAnonymousExecMaps("postAppSpecialize:after-fetch");

    stage_start = std::chrono::steady_clock::now();
    {
        PreloadedDex dex(dex_fd, dex_size);
        this->LoadDex(env_, std::move(dex));
    }
    close(dex_fd);  // The FD is duplicated by mmap, we can close it now.
    LOGI("[startup-trace] native stage end=LoadDex package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));
    TraceAnonymousExecMaps("postAppSpecialize:after-load-dex");

    // Flow reference: rawVector installs framework hooks synchronously before ActivityThread.attach.
    // Vector's hook backend is WxShadow rather than rawVector's LSPlant, so Java initializes the
    // lightweight ART bootstrap before installing only the attach hook. The complete rustFrida
    // controller is installed after attach returns.
    const bool use_art_bootstrap = true;
    LOGI("App process selected: using WxShadow pre-attach bootstrap");
    // Initialize JNI hooks via the native library.
    stage_start = std::chrono::steady_clock::now();
    this->InitHooks(env_);
    LOGI("[startup-trace] native stage end=InitHooks package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));
    TraceAnonymousExecMaps("postAppSpecialize:after-init-hooks");
    // Find the Java entrypoint.
    stage_start = std::chrono::steady_clock::now();
    this->SetupEntryClass(env_);
    LOGI("[startup-trace] native stage end=SetupEntryClass package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));
    TraceAnonymousExecMaps("postAppSpecialize:after-entry-class");

    // Hand off control to the Java side of the framework.
    stage_start = std::chrono::steady_clock::now();
    TraceAnonymousExecMaps("postAppSpecialize:before-fork-common");
    bool entered = this->FindAndCall(
        env_, "forkCommon", "(ZZLjava/lang/String;Ljava/lang/String;Landroid/os/IBinder;Z)V",
        JNI_FALSE, JNI_FALSE, args->nice_name, args->app_data_dir, binder.get(),
        use_art_bootstrap ? JNI_TRUE : JNI_FALSE);
    LOGI("[startup-trace] native stage end=Main.forkCommon package='{}' "
         "elapsedUs={} result={}",
         nice_name_str.get(), StartupElapsedUs(stage_start), entered);
    TraceAnonymousExecMaps("postAppSpecialize:after-fork-common");

    if (entered) {
        LOGV("Injected Vector framework into '{}'.", nice_name_str.get());
    } else {
        LOGE("Framework entry failed in '{}'; this process runs without Xposed.",
             nice_name_str.get());
    }
    // Unconditionally: the ART and JNI hooks were installed before the entry ran, and their
    // trampolines point into this library. Letting it be unloaded now would leave them dangling.
    SetAllowUnload(false);
    NameOwnZygiskRxVma();
    ScheduleLateNameOwnZygiskRxVma();
    LOGI("[startup-trace] native postAppSpecialize end package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(startup_start));
}

void VectorModule::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
    NameOwnZygiskRxVma();

    // Reset process-local backend state just like app specialization does.
    // system_server currently initializes LSPlant explicitly below, but keeping
    // inherited zygote state out avoids stale fallback decisions.
    should_inject_ = false;
    is_manager_app_ = false;
    jni::SetManagerProcess(false);
    jni::SetLsplantInitialized(false);

    // The system server is always a target for injection.
    should_inject_ = true;
    LOGI("System server process detected. Marking for injection.");

    // Initialize our IPC bridge singleton.
    IPCBridge::GetInstance().Initialize(env_);
}

void VectorModule::postServerSpecialize(const zygisk::ServerSpecializeArgs *args) {
    if (!should_inject_) {
        SetAllowUnload(true);
        return;
    }

    LOGD("Attempting injection into system_server.");

    // --- Device-Specific Workaround ---
    // Some ZTE devices require argv[0] to be explicitly set to "system_server"
    // for certain services to function correctly after modification.
    if (__system_property_find("ro.vendor.product.ztename")) {
        LOGV("Applying ZTE-specific workaround: setting argv[0] to system_server.");
        auto process_class = lsplant::ScopedLocalRef(env_, env_->FindClass("android/os/Process"));
        if (process_class) {
            auto set_argv0_mid =
                env_->GetStaticMethodID(process_class.get(), "setArgV0", "(Ljava/lang/String;)V");
            auto name_str = lsplant::ScopedLocalRef(env_, env_->NewStringUTF("system_server"));
            if (set_argv0_mid && name_str) {
                env_->CallStaticVoidMethod(process_class.get(), set_argv0_mid, name_str.get());
            }
        }
        if (env_->ExceptionCheck()) {
            LOGW("Exception occurred during ZTE workaround.");
            env_->ExceptionClear();
        }
    }

    // --- Framework Injection for System Server ---
    bool is_late_inject = (args->runtime_flags & RuntimeFlags::LATE_INJECT) != 0;
    const auto startup_start = std::chrono::steady_clock::now();
    LOGI("[startup-trace] native postServerSpecialize begin late={}", is_late_inject);
    auto &ipc_bridge = IPCBridge::GetInstance();
    std::string bridgeServiceName = "serial";
    if (is_late_inject) bridgeServiceName = "serial_vector";
    auto stage_start = std::chrono::steady_clock::now();
    auto system_binder = ipc_bridge.RequestSystemServerBinder(env_, bridgeServiceName);
    if (!system_binder) {
        LOGI("[startup-trace] native stage end=RequestSystemServerBinder elapsedUs={} "
             "result=failed",
             StartupElapsedUs(stage_start));
        LOGE("Failed to get system server IPC binder. Aborting injection.");
        SetAllowUnload(true);  // Allow unload on failure.
        return;
    }
    LOGI("[startup-trace] native stage end=RequestSystemServerBinder elapsedUs={} result=ok",
         StartupElapsedUs(stage_start));

    stage_start = std::chrono::steady_clock::now();
    auto manager_binder =
        ipc_bridge.RequestManagerBinderFromSystemServer(env_, system_binder.get());
    LOGI("[startup-trace] native stage end=RequestManagerBinder elapsedUs={} result={}",
         StartupElapsedUs(stage_start), manager_binder ? "ok" : "fallback-system");

    // Use either the direct manager binder if available,
    // otherwise proxy through the system binder.
    jobject effective_binder = manager_binder ? manager_binder.get() : system_binder.get();

    stage_start = std::chrono::steady_clock::now();
    auto [dex_fd, dex_size] = ipc_bridge.FetchFrameworkDex(env_, effective_binder);
    if (dex_fd < 0) {
        LOGI("[startup-trace] native stage end=FetchFrameworkDex(system) elapsedUs={} "
             "result=failed",
             StartupElapsedUs(stage_start));
        LOGE("Failed to fetch framework DEX for system_server.");
        SetAllowUnload(true);
        return;
    }

    auto obfs_map = ipc_bridge.FetchObfuscationMap(env_, effective_binder);
    ConfigBridge::GetInstance()->obfuscation_map(std::move(obfs_map));
    LOGI("[startup-trace] native stage end=FetchFrameworkDex(system) elapsedUs={} "
         "result=ok size={}",
         StartupElapsedUs(stage_start), dex_size);

    stage_start = std::chrono::steady_clock::now();
    {
        PreloadedDex dex(dex_fd, dex_size);
        this->LoadDex(env_, std::move(dex));
    }
    close(dex_fd);
    LOGI("[startup-trace] native stage end=LoadDex(system) elapsedUs={}",
         StartupElapsedUs(stage_start));

    stage_start = std::chrono::steady_clock::now();
    ipc_bridge.HookBridge(env_);
    LOGI("[startup-trace] native stage end=HookBridge(system) elapsedUs={}",
         StartupElapsedUs(stage_start));

    stage_start = std::chrono::steady_clock::now();
    this->InitArtHooker(env_, init_info_);
    this->InitHooks(env_);
    this->SetupEntryClass(env_);
    LOGI("[startup-trace] native stage end=InitHooksAndEntry(system) elapsedUs={}",
         StartupElapsedUs(stage_start));

    stage_start = std::chrono::steady_clock::now();
    auto system_name = lsplant::ScopedLocalRef(env_, env_->NewStringUTF("system"));
    bool entered = this->FindAndCall(
        env_, "forkCommon", "(ZZLjava/lang/String;Ljava/lang/String;Landroid/os/IBinder;Z)V",
        JNI_TRUE, is_late_inject, system_name.get(), nullptr, manager_binder.get(),
        JNI_TRUE);
    LOGI("[startup-trace] native stage end=Main.forkCommon(system) elapsedUs={} result={}",
         StartupElapsedUs(stage_start), entered);

    if (entered) {
        LOGI("Injected Vector framework into system_server.");
    } else {
        LOGE("Framework entry failed in system_server; it runs without Xposed.");
    }
    // See postAppSpecialize: the hooks outlive a failed entry, so the library must stay.
    SetAllowUnload(false);
    LOGI("[startup-trace] native postServerSpecialize end elapsedUs={}",
         StartupElapsedUs(startup_start));
}

void VectorModule::SetAllowUnload(bool unload) {
    if (api_ && unload) {
        LOGD("Allowing Zygisk to unload module library.");
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);

        // Release the pointer from the unique_ptr's control. This prevents the
        // static unique_ptr's destructor from calling delete on our object, which
        // would cause a double-free when the Zygisk framework cleans up.
        if (instance_.release() != nullptr) {
            LOGD("Module context singleton released.");
        }
    } else {
        LOGD("Preventing Zygisk from unloading module library.");
    }
}

}  // namespace vector::native::module

// =========================================================================================
// Zygisk Module Registration
// =========================================================================================
REGISTER_ZYGISK_MODULE(vector::native::module::VectorModule);
