package com.example.nathookdemo;

/**
 *记录桥接 stub，见 {@code NativeHookDemoModule}。
 *
 * <p>2026-09-11 SoLoader 恢复：模块 native ELF 由 Vector daemon sealed SharedMemory +
 * NativeAPI.loadNativeLibraries() 走 ElfLoader::LoadLibraryFd() 装载，native_init 在装载后
 * 就完成 RegisterNatives hook 安装，本类不再作为 System.loadLibrary 触发器。
 * start/stopRecording 两个 native 方法仅在未来 demo UI 手动开关时使用；由于 soLoader 的
 * 匿名映射不注册进系统 dl 链，这两个 native 方法依赖进程侧 RegisterNatives 手动绑定，
 * 直接调用将拋 UnsatisfiedLinkError（现有调用点均有兑顶）。</p>
 */
public final class NativeBridge {

    private NativeBridge() {}

    /** Installs the RegisterNatives hook and performs the one-time resolution (idempotent). */
    public static native boolean startRecording();

    /** Removes the RegisterNatives hook in this process. */
    public static native boolean stopRecording();
}
