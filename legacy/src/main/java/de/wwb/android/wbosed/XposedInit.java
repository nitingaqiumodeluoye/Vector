package de.wwb.android.wbosed;

import static de.wwb.android.wbosed.XposedBridge.hookAllMethods;
import static de.wwb.android.wbosed.XposedHelpers.callMethod;
import static de.wwb.android.wbosed.XposedHelpers.findAndHookMethod;
import static de.wwb.android.wbosed.XposedHelpers.getObjectField;
import static de.wwb.android.wbosed.XposedHelpers.getParameterIndexByType;
import static de.wwb.android.wbosed.XposedHelpers.setStaticObjectField;

import android.app.ActivityThread;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.content.res.ResourcesImpl;
import android.content.res.TypedArray;
import android.content.res.XResources;
import android.os.Build;
import android.os.IBinder;
import android.os.Process;
import android.util.ArrayMap;

import org.matrix.vector.impl.core.VectorDeopter;
import org.matrix.vector.impl.core.VectorModuleManager;
import org.matrix.vector.impl.core.VectorServiceClient;
import org.matrix.vector.impl.utils.VectorModuleClassLoader;
import org.matrix.vector.nativebridge.HookBridge;
import org.matrix.vector.nativebridge.NativeAPI;
import org.matrix.vector.nativebridge.ResourcesHook;
import org.matrix.vector.ipc.ModuleCode;
import org.matrix.vector.util.Log;

import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

import de.wwb.android.wbosed.callbacks.XC_InitPackageResources;
import de.wwb.android.wbosed.callbacks.XCallback;
import hidden.HiddenApiBridge;

public final class XposedInit {
    private static final String TAG = XposedBridge.TAG;
    public static boolean startsSystemServer = false;

    public static volatile boolean disableResources = false;
    public static AtomicBoolean resourceInit = new AtomicBoolean(false);

    public static void hookResources() throws Throwable {
        if (disableResources || !resourceInit.compareAndSet(false, true)) {
            return;
        }

        VectorDeopter.deoptResourceMethods();

        if (!ResourcesHook.initXResourcesNative()) {
            Log.e(TAG, "Cannot hook resources");
            disableResources = true;
            return;
        }

        findAndHookMethod("android.app.ApplicationPackageManager", null, "getResourcesForApplication",
                ApplicationInfo.class, new XC_MethodHook() {
                    @Override
                    protected void beforeHookedMethod(MethodHookParam<?> param) {
                        ApplicationInfo app = (ApplicationInfo) param.args[0];
                        XResources.setPackageNameForResDir(app.packageName,
                                app.uid == Process.myUid() ? app.sourceDir : app.publicSourceDir);
                    }
                });

        /*
         * getTopLevelResources(a)
         *   -> getTopLevelResources(b)
         *     -> key = new ResourcesKey()
         *     -> r = new Resources()
         *     -> mActiveResources.put(key, r)
         *     -> return r
         */

        final Class<?> classGTLR;
        final Class<?> classResKey;
        final ThreadLocal<Object> latestResKey = new ThreadLocal<>();
        final ArrayList<String> createResourceMethods = new ArrayList<>();

        classGTLR = android.app.ResourcesManager.class;
        classResKey = android.content.res.ResourcesKey.class;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            createResourceMethods.add("createResources");
            createResourceMethods.add("createResourcesForActivity");
        } else if (Build.VERSION.SDK_INT == Build.VERSION_CODES.R) {
            createResourceMethods.add("createResources");
        } else {
            createResourceMethods.add("getOrCreateResources");
        }

        final Class<?> classActivityRes = XposedHelpers.findClassIfExists("android.app.ResourcesManager$ActivityResource", classGTLR.getClassLoader());
        var hooker = new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam<?> param) {
                // At least on OnePlus 5, the method has an additional parameter compared to AOSP.
                Object activityToken = null;
                try {
                    final int activityTokenIdx = getParameterIndexByType(param.method, IBinder.class);
                    activityToken = param.args[activityTokenIdx];
                } catch (NoSuchFieldError ignored) {
                }
                final int resKeyIdx = getParameterIndexByType(param.method, classResKey);
                String resDir = (String) getObjectField(param.args[resKeyIdx], "mResDir");
                XResources newRes = cloneToXResources(param, resDir);
                if (newRes == null) {
                    return;
                }

                //noinspection SynchronizeOnNonFinalField
                synchronized (param.thisObject) {
                    ArrayList<Object> resourceReferences;
                    if (activityToken != null) {
                        Object activityResources = callMethod(param.thisObject, "getOrCreateActivityResourcesStructLocked", activityToken);
                        //noinspection unchecked
                        resourceReferences = (ArrayList<Object>) getObjectField(activityResources, "activityResources");
                    } else {
                        //noinspection unchecked
                        resourceReferences = (ArrayList<Object>) getObjectField(param.thisObject, "mResourceReferences");
                    }
                    if (activityToken == null || classActivityRes == null) {
                        resourceReferences.add(new WeakReference<>(newRes));
                    } else {
                        // Android S createResourcesForActivity()
                        var activityRes = XposedHelpers.newInstance(classActivityRes);
                        XposedHelpers.setObjectField(activityRes, "resources", new WeakReference<>(newRes));
                        resourceReferences.add(activityRes);
                    }
                }
            }
        };

        for (var createResourceMethod : createResourceMethods) {
            hookAllMethods(classGTLR, createResourceMethod, hooker);
        }

        findAndHookMethod(TypedArray.class, "obtain", Resources.class, int.class,
                new XC_MethodHook() {
                    @Override
                    protected void afterHookedMethod(MethodHookParam<?> param) throws Throwable {
                        if (param.getResult() instanceof XResources.XTypedArray) {
                            return;
                        }
                        if (!(param.args[0] instanceof XResources)) {
                            return;
                        }
                        XResources.XTypedArray newResult =
                                new XResources.XTypedArray((Resources) param.args[0]);
                        int len = (int) param.args[1];
                        Method resizeMethod = XposedHelpers.findMethodBestMatch(
                                TypedArray.class, "resize", int.class);
                        resizeMethod.setAccessible(true);
                        resizeMethod.invoke(newResult, len);
                        param.setResult(newResult);
                    }
                });

        // Replace system resources
        XResources systemRes = new XResources(
                (ClassLoader) XposedHelpers.getObjectField(Resources.getSystem(), "mClassLoader"), null);
        HiddenApiBridge.Resources_setImpl(systemRes, (ResourcesImpl) XposedHelpers.getObjectField(Resources.getSystem(), "mResourcesImpl"));
        setStaticObjectField(Resources.class, "mSystem", systemRes);

        XResources.init(latestResKey);
    }

    private static XResources cloneToXResources(XC_MethodHook.MethodHookParam<?> param, String resDir) {
        Object result = param.getResult();
        if (result == null || result instanceof XResources) {
            return null;
        }

        // Replace the returned resources with our subclass.
        var newRes = new XResources(
                (ClassLoader) XposedHelpers.getObjectField(param.getResult(), "mClassLoader"), resDir);
        HiddenApiBridge.Resources_setImpl(newRes, (ResourcesImpl) XposedHelpers.getObjectField(param.getResult(), "mResourcesImpl"));

        // Invoke handleInitPackageResources().
        if (newRes.isFirstLoad()) {
            String packageName = newRes.getPackageName();
            XC_InitPackageResources.InitPackageResourcesParam resparam = new XC_InitPackageResources.InitPackageResourcesParam(XposedBridge.sInitPackageResourcesCallbacks);
            resparam.packageName = packageName;
            resparam.res = newRes;
            XCallback.callAll(resparam);
        }

        param.setResult(newRes);
        return newRes;
    }

    // only legacy modules have non-empty value
    private static final Map<String, Optional<String>> loadedModules = new ConcurrentHashMap<>();

    public static Map<String, Optional<String>> getLoadedModules() {
        return loadedModules;
    }

    public static void loadLegacyModules() {
        long totalStartNs = System.nanoTime();
        var moduleList = VectorServiceClient.INSTANCE.getLegacyModules();
        Log.i(TAG, "[startup-trace] loadLegacyModules begin count=" + moduleList.size());
        moduleList.forEach(module -> {
            long moduleStartNs = System.nanoTime();
            var apk = module.apkPath;
            var name = module.packageName;
            var file = module.code;
            loadedModules.put(name, Optional.of(apk)); // temporarily add it for XSharedPreference
            boolean loaded = loadModule(name, apk, file);
            if (!loaded) {
                loadedModules.remove(name);
            }
            Log.i(TAG, "[startup-trace] legacy module end package=" + name
                    + " loaded=" + loaded + " elapsedUs="
                    + ((System.nanoTime() - moduleStartNs) / 1_000L));
        });
        Log.i(TAG, "[startup-trace] loadLegacyModules end elapsedUs="
                + ((System.nanoTime() - totalStartNs) / 1_000L));
    }

    private static final AtomicBoolean modulesLoaded = new AtomicBoolean(false);

    public static void loadModules(ActivityThread at) {
        // A late-injected system server calls this directly because its ActivityThread.attach
        // already ran; guard so the normal path cannot load a second generation on top.
        if (!modulesLoaded.compareAndSet(false, true)) {
            Log.i(TAG, "[startup-trace] loadModules skipped: modules already loaded");
            return;
        }
        long totalStartNs = System.nanoTime();
        long stepStartNs = System.nanoTime();
        Log.i(TAG, "[startup-trace] loadModules step begin: getModules");
        var moduleList = VectorServiceClient.INSTANCE.getModules();
        Log.i(TAG, "[startup-trace] loadModules step end: getModules count=" + moduleList.size()
                + " elapsedUs=" + ((System.nanoTime() - stepStartNs) / 1_000L));
        Log.i(TAG, "[startup-trace] loadModules begin count=" + moduleList.size()
                + " system=" + startsSystemServer);

        stepStartNs = System.nanoTime();
        Log.i(TAG, "[startup-trace] loadModules step begin: reflect.ActivityThread.mPackages");
        var packages = (ArrayMap<?, ?>) XposedHelpers.getObjectField(at, "mPackages");
        Log.i(TAG, "[startup-trace] loadModules step end: reflect.ActivityThread.mPackages"
                + " size=" + packages.size()
                + " elapsedUs=" + ((System.nanoTime() - stepStartNs) / 1_000L));

        moduleList.forEach(module -> {
            long moduleStartNs = System.nanoTime();
            long moduleStepStartNs = System.nanoTime();
            Log.i(TAG, "[startup-trace] module step begin label=loadedModules.put module="
                    + module.packageName);
            loadedModules.put(module.packageName, Optional.empty());
            Log.i(TAG, "[startup-trace] module step end label=loadedModules.put module="
                    + module.packageName
                    + " elapsedUs=" + ((System.nanoTime() - moduleStepStartNs) / 1_000L));
            moduleStepStartNs = System.nanoTime();
            Log.i(TAG, "[startup-trace] module step begin label=VectorModuleManager.loadModule.call module="
                    + module.packageName);
            boolean loaded = VectorModuleManager.INSTANCE.loadModule(
                    module, startsSystemServer, VectorServiceClient.INSTANCE.getProcessName());
            Log.i(TAG, "[startup-trace] module step end label=VectorModuleManager.loadModule.call module="
                    + module.packageName
                    + " loaded=" + loaded
                    + " elapsedUs=" + ((System.nanoTime() - moduleStepStartNs) / 1_000L));
            if (!loaded) {
                loadedModules.remove(module.packageName);
            } else {
                moduleStepStartNs = System.nanoTime();
                Log.i(TAG, "[startup-trace] module step begin label=mPackages.remove module="
                        + module.packageName);
                packages.remove(module.packageName);
                Log.i(TAG, "[startup-trace] module step end label=mPackages.remove module="
                        + module.packageName
                        + " elapsedUs=" + ((System.nanoTime() - moduleStepStartNs) / 1_000L));
            }
            Log.i(TAG, "[startup-trace] module end package=" + module.packageName
                    + " loaded=" + loaded + " elapsedUs="
                    + ((System.nanoTime() - moduleStartNs) / 1_000L));
        });
        Log.i(TAG, "[startup-trace] loadModules end elapsedUs="
                + ((System.nanoTime() - totalStartNs) / 1_000L));
    }

    /** Registers module native entrypoint names before module code can invoke System.loadLibrary. */
    private static void initNativeModule(List<String> moduleLibraryNames) {
        moduleLibraryNames.forEach(NativeAPI::recordNativeEntrypoint);
    }

    private static boolean initModule(ClassLoader mcl, String apk, List<String> moduleClassNames) {
        var count = 0;
        for (var moduleClassName : moduleClassNames) {
            try {
                Log.v(TAG, "  Render class " + moduleClassName);

                Class<?> moduleClass = mcl.loadClass(moduleClassName);

                if (!IXposedMod.class.isAssignableFrom(moduleClass)) {
                    Log.e(TAG, "    This class doesn't implement any sub-interface of IXposedMod, skipping it");
                    continue;
                }

                final Object moduleInstance = moduleClass.newInstance();

                if (moduleInstance instanceof IXposedHookZygoteInit) {
                    IXposedHookZygoteInit.StartupParam param = new IXposedHookZygoteInit.StartupParam();
                    param.modulePath = apk;
                    param.startsSystemServer = startsSystemServer;
                    ((IXposedHookZygoteInit) moduleInstance).initZygote(param);
                    count++;
                }

                if (moduleInstance instanceof WbLoadPackage) {
                    XposedBridge.hookLoadPackage(new WbLoadPackage.Wrapper((WbLoadPackage) moduleInstance));
                    count++;
                }

                if (moduleInstance instanceof IXposedHookInitPackageResources) {
                    hookResources();
                    XposedBridge.hookInitPackageResources(new IXposedHookInitPackageResources.Wrapper((IXposedHookInitPackageResources) moduleInstance));
                    count++;
                }
            } catch (Throwable t) {
                Log.e(TAG, "    Failed to render class " + moduleClassName, t);
            }
        }
        return count > 0;
    }

    /**
     * Load a module from an APK by calling the init(String) method for all classes defined
     * in <code>assets/xposed_init</code>.
     */
    private static boolean loadModule(String name, String apk, ModuleCode file) {
        Log.v(TAG, "Render legacy module " + name + " from " + apk);

        // rawVector reference: legacy/src/main/java/de/robv/android/xposed/XposedInit.java
        // exposes the staged system_server directory first, then the APK's stored lib entries.
        // initNativeModule() below registers these names before module code can load them.
        var sb = new StringBuilder();
        if (startsSystemServer && file.nativeLibraryDir != null) {
            sb.append(file.nativeLibraryDir).append(File.pathSeparator);
        }
        var abis = Process.is64Bit() ? Build.SUPPORTED_64_BIT_ABIS : Build.SUPPORTED_32_BIT_ABIS;
        for (String abi : abis) {
            sb.append(apk).append("!/lib/").append(abi).append(File.pathSeparator);
        }
        var librarySearchPath = sb.toString();

        // This must run before the first active reference to VectorModuleClassLoader: ART links its
        // hidden BaseDexClassLoader(ByteBuffer...) super call before loadApk() can execute. This is
        // the narrow BaseDexClassLoader-only form of rustFrida's VMRuntime exemption path.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                && !HookBridge.exemptModuleClassLoaderHiddenApi()) {
            Log.e(TAG, "  Cannot render module: failed to enable the in-memory ClassLoader constructor");
            return false;
        }

        var initLoader = XposedInit.class.getClassLoader();
        var mcl = VectorModuleClassLoader.loadApk(apk, file.preLoadedDexes, librarySearchPath, initLoader);

        try {
            if (mcl.loadClass(XposedBridge.class.getName()).getClassLoader() != initLoader) {
                Log.e(TAG, "  Cannot render module: " + name);
                Log.e(TAG, "  The Xposed API classes are compiled into the module's APK.");
                Log.e(TAG, "  This may cause strange issues and must be fixed by the module developer.");
                Log.e(TAG, "  For details, see: https://api.xposed.info/using.html");
                return false;
            }
        } catch (ClassNotFoundException ignored) {
            return false;
        }
        initNativeModule(file.moduleLibraryNames);
        return initModule(mcl, apk, file.moduleClassNames);
    }

    public final static Set<String> loadedPackagesInProcess = ConcurrentHashMap.newKeySet(1);
}
