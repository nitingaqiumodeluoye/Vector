package org.matrix.vector;

import android.os.Build;

import org.matrix.vector.ipc.IFrameworkService;
import org.matrix.vector.util.Utils;
import org.matrix.vector.impl.core.VectorStartup;
import org.matrix.vector.impl.di.VectorBootstrap;
import org.matrix.vector.legacy.LegacyDelegateImpl;
import org.matrix.vector.nativebridge.HookBridge;

import de.wwb.android.wbosed.XposedBridge;
import de.wwb.android.wbosed.XposedInit;
import de.wwb.android.wbosed.WbosedBridge;

public class Startup {

    private static long elapsedUs(long startNs) {
        return (System.nanoTime() - startNs) / 1_000L;
    }

    private static void traceStartupStep(String label, Runnable action) {
        long startNs = System.nanoTime();
        Utils.logI("[startup-trace] Startup step begin: " + label);
        try {
            action.run();
            Utils.logI("[startup-trace] Startup step end: " + label
                    + " elapsedUs=" + elapsedUs(startNs));
        } catch (Throwable t) {
            Utils.logE("[startup-trace] Startup step failed: " + label
                    + " elapsedUs=" + elapsedUs(startNs), t);
            sneakyThrow(t);
        }
    }

    public static void bootstrapXposed(boolean systemServerStarted) {
        long totalStartNs = System.nanoTime();
        Utils.logI("[startup-trace] Startup.bootstrapXposed begin systemServerStarted="
                + systemServerStarted);
        try {
            traceStartupStep("VectorStartup.bootstrap", () ->
                    VectorStartup.bootstrap(XposedInit.startsSystemServer, systemServerStarted));
            if (!XposedInit.startsSystemServer) {
                // 设备适配（非 rawVector 偏差）：clone+replace 的 wxshadow ART hook 依赖 full
                // controller 的 shared_resolution trampoline 在 ActivityThread.main 首次被
                // reflection 调用时就绪。MIUI 后台 dexopt 把 boot 框架方法 (ActivityThread.main
                // 等) 编译进 boot-framework.oat，编译后的 main 对 attach 是烘焙直连、绕过
                // ArtMethod entrypoint router；若 resolution trampoline 只在 attach 之后
                // (fallback 线程) 才装，main 走正常解析直接进编译码，attach hook 不触发
                // (引擎 trace 实测 hit=0)。
                //
                // 落点选择：必须在 pre-attach LoadedApk hook 全部装完 (VectorStartup.bootstrap
                // 结束) 之后、attach 被调用之前完成 full controller。若提前到 Main.forkCommon
                // (装 LoadedApk hook 之前) 完成，后续安装 createOrUpdateClassLoaderLocked 等
                // hook 时会在 pre-attach 阶段卡死 (实测 3234 连续两个 start_timeout 被杀)。
                // 模块加载仍由 AppAttachHooker 在 attach 边界触发，不改其语义。
                traceStartupStep("completeArtBootstrapEarly", () -> {
                    if (!HookBridge.completeArtBootstrap()) {
                        Utils.logE("Unable to complete WxShadow ART controller early");
                    }
                });
            }
            traceStartupStep("XposedInit.loadLegacyModules", XposedInit::loadLegacyModules);
            traceStartupStep("VectorStartup.bootstrapArtDeferredFallback",
                    VectorStartup::bootstrapArtDeferredFallback);
        } catch (Throwable t) {
            Utils.logE("Error during framework initialization", t);
        } finally {
            Utils.logI("[startup-trace] Startup.bootstrapXposed end elapsedUs="
                    + elapsedUs(totalStartNs));
        }
    }

    public static void bootstrapXposedNoArt() {
        long totalStartNs = System.nanoTime();
        Utils.logI("[startup-trace] Startup.bootstrapXposedNoArt begin");
        try {
            traceStartupStep("VectorStartup.bootstrapNoArtDeferred",
                    VectorStartup::bootstrapNoArtDeferred);
        } catch (Throwable t) {
            Utils.logE("Error during no-ART deferred framework initialization", t);
        } finally {
            Utils.logI("[startup-trace] Startup.bootstrapXposedNoArt end elapsedUs="
                    + elapsedUs(totalStartNs));
        }
    }

    /**
     * Registers a pre-built {@code LoadedApk} (the LSPatch rootless target) with the modern hooks so
     * its package lifecycle is dispatched when its class loader is realized. See
     * {@link VectorStartup#trackLoadedApk(Object)}.
     */
    public static void trackLoadedApk(Object loadedApk) {
        VectorStartup.trackLoadedApk(loadedApk);
    }

    public static void initXposed(boolean isSystem, String processName, String appDir, IFrameworkService service) {
        long totalStartNs = System.nanoTime();
        Utils.logI("[startup-trace] Startup.initXposed begin system=" + isSystem
                + " process=" + processName);
        // rustFrida installs its VMRuntime hidden-API exemption while initializing Java support.
        // Vector needs only BaseDexClassLoader's ByteBuffer constructors, so install that narrow
        // exemption in the same early window, before the target application's Java initialization.
        // The native side caches success for the later deferred module-loading thread.
        try {
            traceStartupStep("HookBridge.exemptModuleClassLoaderHiddenApi", () -> {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                        && !HookBridge.exemptModuleClassLoaderHiddenApi()) {
                    Utils.logE("Unable to enable the in-memory module ClassLoader constructor");
                }
            });

            // Establish the Dependency Injection contract
            traceStartupStep("VectorBootstrap.init",
                    () -> VectorBootstrap.INSTANCE.init(new LegacyDelegateImpl()));

            // Initialize legacy resources and state
            traceStartupStep("WbosedBridge.initXResources", WbosedBridge::initXResources);
            XposedInit.startsSystemServer = isSystem;

            // Hand off execution to the modern framework initialization
            traceStartupStep("VectorStartup.init",
                    () -> VectorStartup.init(isSystem, processName, appDir, service));
        } finally {
            Utils.logI("[startup-trace] Startup.initXposed end elapsedUs="
                    + elapsedUs(totalStartNs));
        }
    }

    @SuppressWarnings("unchecked")
    private static <T extends Throwable> void sneakyThrow(Throwable t) throws T {
        throw (T) t;
    }
}
