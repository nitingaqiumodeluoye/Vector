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
}
