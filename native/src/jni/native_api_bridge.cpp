#include "core/native_api.h"
#include "jni/jni_bridge.h"
#include "jni/jni_hooks.h"

#include <android/sharedmem_jni.h>

#include <chrono>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace vector::native::jni {

VECTOR_DEF_NATIVE_METHOD(void, NativeAPI, recordNativeEntrypoint, jstring jstr) {
    lsplant::JUTFString str(env, jstr);
    vector::native::RegisterNativeLib(str.get());
}

VECTOR_DEF_NATIVE_METHOD(jbooleanArray, NativeAPI, loadNativeLibraries, jobjectArray names,
                         jobjectArray libraries) {
    const jsize name_count = names ? env->GetArrayLength(names) : 0;
    const jsize library_count = libraries ? env->GetArrayLength(libraries) : 0;
    auto result = env->NewBooleanArray(name_count);
    if (!result) return nullptr;
    if (name_count != library_count) return result;

    std::vector<jboolean> values(static_cast<size_t>(name_count), JNI_FALSE);
    for (jsize i = 0; i < name_count; ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        auto memory = env->GetObjectArrayElement(libraries, i);
        if (!name || !memory) {
            if (name) env->DeleteLocalRef(name);
            if (memory) env->DeleteLocalRef(memory);
            continue;
        }
        const char *name_chars = env->GetStringUTFChars(name, nullptr);
        const int fd = ASharedMemory_dupFromJava(env, memory);
        const size_t size = fd >= 0 ? ASharedMemory_getSize(fd) : 0;
        if (name_chars && fd >= 0) {
            const auto elem_start = std::chrono::steady_clock::now();
            unsigned char pread_magic[4] = {};
            const ssize_t pread_size = size >= sizeof(pread_magic)
                                           ? pread(fd, pread_magic, sizeof(pread_magic), 0)
                                           : -1;
            void* mapped = size > 0 ? mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
            if (mapped != MAP_FAILED) {
                const auto* bytes = static_cast<const unsigned char*>(mapped);
                LOGI("Native soLoader input '{}' fd={} size={} pread={} preadMagic={:02x} {:02x} {:02x} {:02x} mmap=shared mmapMagic={:02x} {:02x} {:02x} {:02x}",
                     name_chars, fd, size, pread_size,
                     pread_magic[0], pread_magic[1], pread_magic[2], pread_magic[3],
                     bytes[0], bytes[1], bytes[2], bytes[3]);
                munmap(mapped, size);
            } else {
                LOGE("Native soLoader input '{}' fd={} size={} shared mmap failed: {}", name_chars, fd,
                     size, strerror(errno));
            }
            values[static_cast<size_t>(i)] =
                vector::native::LoadNativeLibraryFd(fd, size, name_chars) ? JNI_TRUE : JNI_FALSE;
            LOGI("[perf] load_native_lib element={} name='{}' elapsedUs={}", i, name_chars,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - elem_start)
                     .count()));
        }
        if (name_chars) env->ReleaseStringUTFChars(name, name_chars);
        if (fd >= 0) close(fd);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(memory);
    }
    env->SetBooleanArrayRegion(result, 0, name_count, values.data());
    return result;
}

VECTOR_DEF_NATIVE_METHOD(jboolean, NativeAPI, firePendingNativeInits, jclass carrier_class) {
    // Fires the native_init entrypoints deferred by LoadNativeLibraryFd from the context of the
    // given module-defined carrier class, so FindClass inside them resolves the module
    // ClassLoader (rawVector semantics). See native_api.h for the full rationale.
    return vector::native::FirePendingNativeInits(env, carrier_class) ? JNI_TRUE : JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
    VECTOR_NATIVE_METHOD(NativeAPI, recordNativeEntrypoint, "(Ljava/lang/String;)V"),
    VECTOR_NATIVE_METHOD(NativeAPI, loadNativeLibraries,
                        "([Ljava/lang/String;[Landroid/os/SharedMemory;)[Z"),
    VECTOR_NATIVE_METHOD(NativeAPI, firePendingNativeInits, "(Ljava/lang/Class;)Z"),
};

void RegisterNativeApiBridge(JNIEnv *env) { REGISTER_VECTOR_NATIVE_METHODS(NativeAPI); }

}  // namespace vector::native::jni
