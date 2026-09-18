#include <common/config.h>
#include <common/logging.h>
#include <core/context.h>
#include <core/native_api.h>
#include <elf/elf_image.h>
#include <elf/symbol_cache.h>
#include <jni/jni_bridge.h>
#include <jni/jni_hooks.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <zygisk.hpp>

#include "ipc_bridge.h"

namespace vector::native::module {

// 2026-09-18 移除：此处曾实现「自身 VMA 命名」——常量 kZygiskVmaName="wwb_zygisk"、
// NameOwnZygiskRxVma()、后台延时重试线程（LateNameOwnZygiskRxVmaWorker /
// ScheduleLateNameOwnZygiskRxVma）。原意是把本模块的匿名 RX 段命名成 wwb_*，躲开
// 「无名 anon r-x = LSPLant trampoline / InMemoryDex」的启发式。整段已删除，原因：
//
//   1. ReZygisk 的 CSOLoader 以 `MAP_PRIVATE + real fd` 映射模块，该段是 file-backed
//      （实测 r-xp 00000000 fc:0a 297600 /data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so），
//      函数里的匿名守卫 `dev=="00:00" && inode==0` 直接 break，一次机会都没有。
//   2. 放宽守卫也无收益：PR_SET_VMA_ANON_NAME 对 file-backed VMA **返回 0 但静默无效**，
//      两道关卡都在内核里（小米 4.19 / alioth 源码已核对）：
//        kernel/sys.c:2307-2309   success: if (!vma->vm_file) vma->anon_name = name_addr;
//                                 —— file-backed 段直接跳过赋值，函数仍返回 0；
//        fs/proc/task_mmu.c:378-382  if (file) { seq_file_path(m, file, "\n"); goto done; }
//                                 —— 打印文件路径后就跳走，永远走不到 408-411 行的
//                                 seq_print_vma_name()。
//      所以只放宽守卫会打出「命名成功」的假阳性日志（本函数 `if (ret == 0) LOGI(...)`）。
//
//      另注（若日后重做本功能必须知道）：该内核的 anon name 是 **用户态指针**，
//      不拷贝字符串（mm_types.h:308 `const char __user *anon_name`），打印时才用
//      get_user_pages_remote 现读那一页；页读不到就打印 "[anon:<fault>]"
//      （task_mmu.c:152-157）。故名字字符串必须常驻 —— 放进会被卸载的库里会退化成
//      [anon:<fault>]，那是比无名段更明显的指纹。
//   3. 代价不低：8 处同步调用 + 后台线程 6 次重试 = 单进程最多 14 次全量扫
//      /proc/self/maps（page-table walk + mmap_lock），外加一个常驻 4.26s 的短命线程。
//
// 隐藏职责改由 Treat Wheel 的 MH（maps hiding，src/lib/hiding.c::do_maps_hiding）承担：
// 它用「匿名副本 + mremap(MREMAP_MAYMOVE|MREMAP_FIXED) 覆盖原址」把 file-backed 段换成
// 匿名段，路径整段从 maps 消失。若日后确需恢复命名，正确顺序是**先做该换页转换再
// prctl 命名**（换页后的 VMA 才是匿名，prctl 才真正生效）。

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
     * It uses a BaseDexClassLoader to load our framework into the target process.
     *
     * Source reference: rustFrida ddda052 "use BaseDexClassLoader for dynamic managed help dex"
     * (quickjs-hook/src/jsapi/java/java_hook_api/managed_install.rs). InMemoryDexClassLoader is
     * a BaseDexClassLoader subclass with identical DexPathList/ByteBuffer[] semantics, but its
     * runtime class name is a hunter-type detector signature ("dex load size" enum). Instantiating
     * the public parent class directly keeps the rawVector load timing/flow while aligning the
     * loader implementation with rustFrida.
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
            [](auto symbol) {
                const auto *art = ElfSymbolCache::GetArt();
                auto address = art ? art->getSymbAddress(symbol) : nullptr;
                if (!symbol.empty() && symbol ==
                                  "_ZN3art7Runtime15DisableVerifierEv") {
                    LOGI("[lsplant-art-resolver] symbol={} address={} cache={}",
                         symbol, address, static_cast<const void *>(art));
                }
                return address;
            },
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

    // Create the loader instance directly as the public parent class. Source: rustFrida
    // managed_install.rs: runtime class = dalvik.system.BaseDexClassLoader, so hunter-type
    // InMemoryDexClassLoader enumeration stays at 0. API>=29 uses the 3-arg ctor (null
    // librarySearchPath), API<29 uses the 2-arg ctor; parent stays the system loader
    // (rawVector load timing predates app classloaders, matching rustFrida's fallback).
    auto base_cl_class = lsplant::JNI_FindClass(env, "dalvik/system/BaseDexClassLoader");
    if (!base_cl_class) {
        LOGE("Failed to find BaseDexClassLoader.");
        return;
    }
    auto dex_buffers = lsplant::ScopedLocalRef(env,
        env->NewObjectArray(1, byte_buffer_class.get(), dex_buffer.get()));
    if (!dex_buffers) {
        LOGE("Failed to create ByteBuffer[] for DEX.");
        return;
    }
    jmethodID init_mid = nullptr;
    const int api_level = []() {
        static int cached = -1;
        if (cached < 0) {
            char value[PROP_VALUE_MAX] = {0};
            __system_property_get("ro.build.version.sdk", value);
            cached = atoi(value);
        }
        return cached;
    }();
    if (api_level < 1) {
        LOGE("Failed to read ro.build.version.sdk.");
        return;
    }
    if (api_level >= 29) {
        init_mid = lsplant::JNI_GetMethodID(env, base_cl_class.get(), "<init>",
            "([Ljava/nio/ByteBuffer;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    } else {
        init_mid = lsplant::JNI_GetMethodID(env, base_cl_class.get(), "<init>",
            "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    }
    if (!init_mid) {
        LOGE("Failed to find BaseDexClassLoader constructor.");
        return;
    }

    jvalue ctor_args[3] = {};
    if (api_level >= 29) {
        // args: (ByteBuffer[], null librarySearchPath, parent) — matches rustFrida's
        // api_level >= 29 branch. jvalue.l for both the Object refs and the null String.
        ctor_args[0].l = dex_buffers.get();
        ctor_args[1].l = nullptr;
        ctor_args[2].l = system_classloader.get();
    } else {
        ctor_args[0].l = dex_buffers.get();
        ctor_args[1].l = system_classloader.get();
    }
    auto new_cl = lsplant::ScopedLocalRef(env, env->NewObjectA(base_cl_class.get(), init_mid,
        ctor_args));
    if (env->ExceptionCheck() || !new_cl) {
        LOGE("Failed to create BaseDexClassLoader instance.");
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

    // Create two singlton instances for classes Context and ConfigBridge
    instance_.reset(this);
    ConfigImpl::Init();
    LOGD("Vector Zygisk module loaded");
}

void VectorModule::preAppSpecialize(zygisk::AppSpecializeArgs *args) {
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

    if (is_manager_app_) {
        args->nice_name = env_->NewStringUTF(kManagerPackageName);
    }

    // --- Framework Injection ---
    lsplant::JUTFString nice_name_str(env_, args->nice_name);
    const auto startup_start = std::chrono::steady_clock::now();
    LOGI("[startup-trace] native postAppSpecialize begin package='{}'", nice_name_str.get());
    LOGD("Asking the daemon about '{}'.", nice_name_str.get());

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
        SetAllowUnload(true);
        return;
    }
    LOGI("[startup-trace] native stage end=RequestAppBinder package='{}' "
         "elapsedUs={} result=ok",
         nice_name_str.get(), StartupElapsedUs(stage_start));

    // Fetch resources from the manager service.
    stage_start = std::chrono::steady_clock::now();
    auto [dex_fd, dex_size] = ipc_bridge.FetchFrameworkDex(env_, binder.get());
    if (dex_fd < 0) {
        LOGI("[startup-trace] native stage end=FetchFrameworkDex package='{}' "
             "elapsedUs={} result=failed",
             nice_name_str.get(), StartupElapsedUs(stage_start));
        LOGE("Failed to fetch framework DEX for '{}'.", nice_name_str.get());
        SetAllowUnload(true);
        return;
    }

    auto obfs_map = ipc_bridge.FetchObfuscationMap(env_, binder.get());
    ConfigBridge::GetInstance()->obfuscation_map(std::move(obfs_map));
    LOGI("[startup-trace] native stage end=FetchFrameworkDex package='{}' "
         "elapsedUs={} result=ok size={}",
         nice_name_str.get(), StartupElapsedUs(stage_start), dex_size);

    stage_start = std::chrono::steady_clock::now();
    {
        PreloadedDex dex(dex_fd, dex_size);
        this->LoadDex(env_, std::move(dex));
    }
    close(dex_fd);  // The FD is duplicated by mmap, we can close it now.
    LOGI("[startup-trace] native stage end=LoadDex package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));

    // Flow reference: rawVector installs framework hooks synchronously before ActivityThread.attach.
    // Vector's hook backend is WxShadow rather than rawVector's LSPlant, so Java initializes the
    // lightweight ART bootstrap before installing only the attach hook. The complete rustFrida
    // controller is installed after attach returns.
    const bool use_art_bootstrap = true;
    LOGI("App process selected: using WxShadow pre-attach bootstrap");
    // 该路径不经过任何 JNI 方法，HookBridge.initializeArtBootstrap/completeArtBootstrap
    // 都不会被执行，dispatch 热路径的类缓存会永远为空（管理器进程因此每个 dispatch 都
    // 跳过，LoadedApk.getClassLoader() 返回 null 导致宿主 makeApplication 抛 NPE）。
    // 这里在栈仍然干净的 pre-attach 阶段先把缓存与 JavaVM 补齐。
    jni::PrimeDispatchClassCache(env_);
    // Initialize JNI hooks via the native library.
    stage_start = std::chrono::steady_clock::now();
    this->InitHooks(env_);
    LOGI("[startup-trace] native stage end=InitHooks package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));
    // Find the Java entrypoint.
    stage_start = std::chrono::steady_clock::now();
    this->SetupEntryClass(env_);
    LOGI("[startup-trace] native stage end=SetupEntryClass package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(stage_start));

    // Hand off control to the Java side of the framework.
    stage_start = std::chrono::steady_clock::now();
    bool entered = this->FindAndCall(
        env_, "forkCommon", "(ZZLjava/lang/String;Ljava/lang/String;Landroid/os/IBinder;Z)V",
        JNI_FALSE, JNI_FALSE, args->nice_name, args->app_data_dir, binder.get(),
        use_art_bootstrap ? JNI_TRUE : JNI_FALSE);
    LOGI("[startup-trace] native stage end=Main.forkCommon package='{}' "
         "elapsedUs={} result={}",
         nice_name_str.get(), StartupElapsedUs(stage_start), entered);

    if (entered) {
        LOGV("Injected Vector framework into '{}'.", nice_name_str.get());
    } else {
        LOGE("Framework entry failed in '{}'; this process runs without Xposed.",
             nice_name_str.get());
    }
    // Unconditionally: the ART and JNI hooks were installed before the entry ran, and their
    // trampolines point into this library. Letting it be unloaded now would leave them dangling.
    SetAllowUnload(false);
    LOGI("[startup-trace] native postAppSpecialize end package='{}' elapsedUs={}",
         nice_name_str.get(), StartupElapsedUs(startup_start));
}

void VectorModule::preServerSpecialize(zygisk::ServerSpecializeArgs *args) {
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
