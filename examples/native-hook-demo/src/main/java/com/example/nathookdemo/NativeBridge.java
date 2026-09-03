package com.example.nathookdemo;

/**
 * Thin JNI bridge into {@code libvector_nathook_demo.so}.
 *
 * <p>The static block is the module's native-load trigger: {@code System.loadLibrary} funnels
 * through {@code VectorModuleClassLoader.findLibrary()} (which answers with the STORED
 * {@code apk!/lib/<abi>/…} zip entry) into the linker {@code do_dlopen} hook, where Vector
 * resolves and runs {@code native_init} with the {@code NativeAPIEntries} pointer.</p>
 *
 * <p>The native entry reproduces rustFrida's {@code Jni.addr("RegisterNatives")} resolution
 * (JNI_GetCreatedJavaVMs → GetEnv → *env table[215]) and hooks it through Vector's hookFunc.
 * The module's own UI can install/remove the hook in its process; scoped target processes install
 * it from native_init during injection.</p>
 */
public final class NativeBridge {

    private NativeBridge() {}

    static {
        System.loadLibrary("vector_nathook_demo");
    }

    /** Installs the RegisterNatives hook and performs the one-time resolution (idempotent). */
    public static native boolean startRecording();

    /** Removes the RegisterNatives hook in this process. */
    public static native boolean stopRecording();
}
