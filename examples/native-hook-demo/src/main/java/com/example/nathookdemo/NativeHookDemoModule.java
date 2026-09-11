package com.example.nathookdemo;

import android.util.Log;

import de.wwb.android.wbosed.api.WbModule;
import de.wwb.android.wbosed.api.WbModuleInterface;

/**
 * Native RegisterNatives hook demo module entry class.
 *
 * <p>Load/initialization chain (per injected process, 2026-09-11 soLoader restored):
 * <ol>
 *   <li>The daemon reads {@code META-INF/xposed/native_init.list} and registers every
 *       native library name with Vector's native API before this class is instantiated.</li>
 *   <li>Vector reads the module APK's {@code lib/arm64-v8a/libvector_nathook_demo.so} into a
 *       sealed read-only SharedMemory and hands the fd to core: {@code NativeAPI.loadNativeLibraries()}
 *       → {@code ElfLoader::LoadLibraryFd()} (no APK path / zip entry ever reaches the linker).</li>
 *   <li>After linking, Vector calls the module's {@code native_init(NativeAPIEntries*)} export.
 *       The native entry resolves {@code RegisterNatives} (rustFrida
 *       {@code Jni.addr} equivalent) and hooks it.</li>
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

        // 2026-09-11 soLoader restored: the RegisterNatives hook is installed by native_init
        // during ElfLoader::LoadLibraryFd(), not by NativeBridge's System.loadLibrary trigger.
        // NativeBridge.startRecording() stays only as an optional manual UI entrypoint; its
        // native method needs explicit binding under soLoader and is not callable here.
        Log.i(TAG, "native hook handled by Vector soLoader native_init");
    }
}
