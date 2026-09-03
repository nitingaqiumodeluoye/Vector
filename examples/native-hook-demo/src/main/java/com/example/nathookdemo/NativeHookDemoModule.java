package com.example.nathookdemo;

import android.util.Log;

import de.wwb.android.wbosed.api.WbModule;
import de.wwb.android.wbosed.api.WbModuleInterface;

/**
 * Native RegisterNatives hook demo module entry class.
 *
 * <p>Load/initialization chain (per injected process):
 * <ol>
 *   <li>The daemon reads {@code META-INF/xposed/native_init.list} and registers every
 *       native library name with Vector's native API before this class is instantiated.</li>
 *   <li>{@link NativeBridge}'s static block calls {@code System.loadLibrary("vector_nathook_demo")}.
 *       Android's linker opens {@code apk!/lib/arm64-v8a/libvector_nathook_demo.so} (the STORED
 *       zip entry resolved by VectorModuleClassLoader.findLibrary()).</li>
 *   <li>Vector's {@code do_dlopen} monitor matches the name, resolves {@code native_init} from
 *       the loaded ELF and calls {@code native_init(NativeAPIEntries*)}. The native entry resolves
 *       {@code RegisterNatives} (rustFrida {@code Jni.addr} equivalent) and hooks it.</li>
 * </ol>
 *
 * <p>Expected logcat (tag {@code VectorNativeHookDemo}):
 * <pre>
 * [register-natives] resolved addr=... via=env-table (JNIEnv table[215])
 * [register-natives] hooked addr=... trampoline=...
 * [register-natives] intercepted clazz=... methods=1
 * [register-natives]   [0] name='foo' sig='(I)I' fn=0x...
 * </pre>
 */
public final class NativeHookDemoModule extends WbModule {

    private static final String TAG = "VectorNativeHookDemo";

    @Override
    public void onModuleLoaded(WbModuleInterface.ModuleLoadedParam param) {
        Log.i(TAG, "onModuleLoaded proc=" + param.getProcessName()
                + " systemServer=" + param.isSystemServer());

        // Loads the native lib (static block) and installs the RegisterNatives hook. native_init
        // performs the same setup as soon as the linker loads the ELF, so this call is an idempotent
        // re-install for the module's Java lifecycle.
        //
        // Some framework processes can reject a module APK native mapping. A failed static init
        // also poisons NativeBridge for the rest of the process, surfacing later as
        // NoClassDefFoundError; catch both and keep the Java entry alive in that process.
        try {
            NativeBridge.startRecording();
            Log.i(TAG, "RegisterNatives hook ready");
        } catch (UnsatisfiedLinkError | NoClassDefFoundError e) {
            Log.w(TAG, "native RegisterNatives hook unavailable in this process: "
                    + e.getMessage());
        }
    }
}
