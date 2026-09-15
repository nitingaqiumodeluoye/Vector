package com.example.duckusb

/**
 * JNI bridge to libvector_duckusb.so. Pushes the prop overrides the native libc hooks apply.
 *
 * The upstream version called System.loadLibrary("duckusb") here. That does not work under
 * Vector and is not a style question:
 *
 *  - Vector reads lib/arm64-v8a/libvector_duckusb.so out of the APK into sealed SharedMemory and
 *    links it through its own ELF loader. The image is never registered with the Android linker,
 *    and VectorModuleClassLoader.findLibrary deliberately refuses `apk!/lib/<abi>/<name>.so`
 *    entries so that no linker mapping of the module APK can appear in the target process.
 *  - Consequently this `external fun` has no symbol to bind to by name. The native side binds it
 *    itself with RegisterNatives from native_init (see bind_native_methods in native_hooks.cpp),
 *    which runs before any module Java code does.
 *
 * So the only thing the Kotlin side owes the native side is the call. It is safe before the
 * binding exists only in the sense that it cannot be reached first: native_init runs during the
 * module's load, and this class is only touchable afterwards.
 */
object NativeProps {

    /** Hand the override map to the native side. Safe to call repeatedly; the last map wins. */
    fun install(overrides: Map<String, String>) {
        setProps(overrides)
    }

    private external fun setProps(props: Map<String, String>)
}
