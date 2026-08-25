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
