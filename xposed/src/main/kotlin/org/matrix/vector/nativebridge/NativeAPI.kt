package org.matrix.vector.nativebridge

import android.os.SharedMemory

object NativeAPI {
    /**
     * Loads module ELF images from sealed SharedMemory through Vector's soLoader only.
     */
    @JvmStatic external fun recordNativeEntrypoint(library_name: String)

    @JvmStatic
    external fun loadNativeLibraries(
        libraryNames: Array<String>,
        libraries: Array<SharedMemory>,
    ): BooleanArray

    /**
     * Fires the native_init entrypoints deferred by [loadNativeLibraries] from the context of the
     * given module-defined carrier class, so that JNIEnv::FindClass inside native_init resolves
     * the module ClassLoader (rawVector semantics). Pass null to fall back to the framework
     * context. Returns false when the fire attempt failed and may be retried.
     */
    @JvmStatic external fun firePendingNativeInits(carrierClass: Class<*>?): Boolean
}
