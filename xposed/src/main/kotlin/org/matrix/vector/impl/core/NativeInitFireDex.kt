package org.matrix.vector.impl.core

import android.os.SharedMemory
import android.system.OsConstants
import android.util.Base64
import java.nio.ByteBuffer

/**
 * Prebuilt DEX (generated once at development time with d8 from
 * org/matrix/vector/nativeload/NativeInitFire.java, committed as base64) defining Vector's
 * internal carrier class `org.matrix.vector.nativeload.NativeInitFire` with a declared native
 * method `fireNativeInit()V`.
 *
 * The dex is appended to every module ClassLoader's dex list so the carrier class is defined by
 * the MODULE ClassLoader itself. Vector then binds a trampoline onto the declared native method
 * (RegisterNatives) and invokes it (CallStaticVoidMethod): while ART executes that native method,
 * JNIEnv::FindClass inside deferred module native_init entrypoints resolves against the module
 * ClassLoader — the same context rawVector provided by running native_init from the module's own
 * System.loadLibrary stack (ART GetClassLoader: current native method's declaring class loader).
 *
 * Why a prebuilt dex: RegisterNatives requires an already-declared native method and ART's JNI
 * DefineClass is not supported, so the carrier cannot be created at runtime.
 */
internal object NativeInitFireDex {
    const val CLASS_NAME = "org.matrix.vector.nativeload.NativeInitFire"

    private const val DEX_BASE64 =
        "ZGV4CjAzOAA/eHSDLDAPHq9oOV3KpWaA8q0CPW562sukAgAAcAAAAHhWNBIAAAAAAAAAABwCAAAHAAAAcAAAAAMAAACMAAAAAQAAAJgAAAAAAAAAAAAAAAMAAACkAAAAAQAAALwAAADIAQAA3AAAAPgAAAAAAQAAFAEAAEMBAABYAQAAWwEAAGsBAAABAAAAAgAAAAQAAAAEAAAAAgAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAQAAAAUAAAABAAAAEQAAAAAAAAAAAAAAAwAAAAAAAAAMAgAAAAAAAAEAAQABAAAA9AAAAAQAAABwEAAAAAAOAAsADgAGPGluaXQ+ABJMamF2YS9sYW5nL09iamVjdDsALUxvcmcvbWF0cml4L3ZlY3Rvci9uYXRpdmVsb2FkL05hdGl2ZUluaXRGaXJlOwATTmF0aXZlSW5pdEZpcmUuamF2YQABVgAOZmlyZU5hdGl2ZUluaXQAngF+fkQ4eyJiYWNrZW5kIjoiZGV4IiwiY29tcGlsYXRpb24tbW9kZSI6InJlbGVhc2UiLCJoYXMtY2hlY2tzdW1zIjpmYWxzZSwibWluLWFwaSI6MjYsInNoYS0xIjoiYWJhYWI0NjliNWViZDRkZDJiYjkxYmEwZWQ2ZjQ1Mjc3ZmFhZTRjYSIsInZlcnNpb24iOiI4LjYuMi1kZXYifQAAAAIAAYKABNwBAYkCAAAACwAAAAAAAAABAAAAAAAAAAEAAAAHAAAAcAAAAAIAAAADAAAAjAAAAAMAAAABAAAAmAAAAAUAAAADAAAApAAAAAYAAAABAAAAvAAAAAEgAAABAAAA3AAAAAMgAAABAAAA9AAAAAIgAAAHAAAA+AAAAAAgAAABAAAADAIAAAAQAAABAAAAHAIAAA=="

    /**
     * Creates a read-only SharedMemory carrying the carrier dex. The caller hands it to
     * [VectorModuleClassLoader.loadApk] together with the module's own dexes; loadApk closes the
     * fd once the ClassLoader is up. Returns null if allocation fails, which simply degrades to
     * the framework-context native_init fallback.
     */
    fun newSharedMemory(): SharedMemory? =
        runCatching {
            val bytes = Base64.decode(DEX_BASE64, Base64.NO_WRAP)
            val memory = SharedMemory.create("wwb_nif_dex", bytes.size)
            val buffer = memory.mapReadWrite()
            try {
                buffer.put(ByteBuffer.wrap(bytes))
            } finally {
                SharedMemory.unmap(buffer)
            }
            check(memory.setProtect(OsConstants.PROT_READ)) { "Unable to seal carrier dex" }
            memory
        }.onFailure { e ->
            android.util.Log.e("VectorModuleManager", "Cannot create NativeInitFire carrier dex", e)
        }.getOrNull()
}
