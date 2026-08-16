package org.matrix.vector.nativebridge

object NativeAPI {
    /**
     * Compatibility no-op. Vector no longer loads native code from module APKs at runtime; native
     * inline hook capabilities are provided by Vector core.
     */
    @JvmStatic external fun recordNativeEntrypoint(library_name: String)

    /** Installs Vector core's built-in RegisterNatives probe through the wxshadow hook engine. */
    @JvmStatic external fun installRegisterNativesProbe(): Boolean
}
