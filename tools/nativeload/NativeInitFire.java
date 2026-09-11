package org.matrix.vector.nativeload;

/**
 * Vector-internal carrier class, embedded as a prebuilt DEX into every module ClassLoader
 * (build-time generated, committed as bytes). Vector binds a trampoline onto its declared native
 * method {@code fireNativeInit} via RegisterNatives and invokes it, so that deferred module
 * native_init entrypoints execute with the module ClassLoader as ART's FindClass context
 * (rawVector semantics under the soLoader path).
 */
public final class NativeInitFire {
    private NativeInitFire() {
    }

    public static native void fireNativeInit();
}
