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

    public static void bootstrapXposed(boolean systemServerStarted) {
        try {
            VectorStartup.bootstrap(XposedInit.startsSystemServer, systemServerStarted);
            XposedInit.loadLegacyModules();
        } catch (Throwable t) {
            Utils.logE("Error during framework initialization", t);
        }
    }

    public static void bootstrapXposedNoArt() {
        try {
            VectorStartup.bootstrapNoArtDeferred();
        } catch (Throwable t) {
            Utils.logE("Error during no-ART deferred framework initialization", t);
        }
    }

    public static void initXposed(boolean isSystem, String processName, String appDir, IFrameworkService service) {
        // rustFrida installs its VMRuntime hidden-API exemption while initializing Java support.
        // Vector needs only BaseDexClassLoader's ByteBuffer constructors, so install that narrow
        // exemption in the same early window, before the target application's Java initialization.
        // The native side caches success for the later deferred module-loading thread.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                && !HookBridge.exemptModuleClassLoaderHiddenApi()) {
            Utils.logE("Unable to enable the in-memory module ClassLoader constructor");
        }

        // Establish the Dependency Injection contract
        VectorBootstrap.INSTANCE.init(new LegacyDelegateImpl());

        // Initialize legacy resources and state
        WbosedBridge.initXResources();
        XposedInit.startsSystemServer = isSystem;

        // Hand off execution to the modern framework initialization
        VectorStartup.init(isSystem, processName, appDir, service);
    }
}
