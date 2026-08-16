package de.wwb.android.wbosed;

import android.app.ActivityThread;
import android.content.res.Resources;
import android.content.res.TypedArray;

import org.matrix.vector.nativebridge.ResourcesHook;

/** Internal resource bootstrap whose name does not expose the legacy Xposed label. */
public final class WbosedBridge {
    private WbosedBridge() {
    }

    public static void initXResources() {
        if (XposedBridge.dummyClassLoader != null) {
            return;
        }
        try {
            Resources res = Resources.getSystem();
            Class<?> resClass = res.getClass();
            Class<?> taClass = TypedArray.class;
            try {
                try {
                    TypedArray ta = res.obtainTypedArray(res.getIdentifier(
                            "preloaded_drawables", "array", "android"));
                    taClass = ta.getClass();
                    ta.recycle();
                } catch (NullPointerException npe) {
                    var fake = XposedHelpers.newInstance(ActivityThread.class);
                    XposedHelpers.setStaticObjectField(ActivityThread.class,
                            "sCurrentActivityThread", fake);
                    try {
                        TypedArray ta = res.obtainTypedArray(res.getIdentifier(
                                "preloaded_drawables", "array", "android"));
                        taClass = ta.getClass();
                        ta.recycle();
                    } finally {
                        XposedHelpers.setStaticObjectField(ActivityThread.class,
                                "sCurrentActivityThread", null);
                    }
                }
            } catch (Resources.NotFoundException nfe) {
                XposedBridge.log(nfe);
            }

            ResourcesHook.makeInheritable(resClass);
            ResourcesHook.makeInheritable(taClass);
            ClassLoader myClassLoader = XposedBridge.class.getClassLoader();
            if (myClassLoader == null) {
                throw new IllegalStateException("WbosedBridge class loader is null");
            }
            ClassLoader dummyClassLoader = ResourcesHook.buildDummyClassLoader(
                    myClassLoader.getParent(), resClass.getName(), taClass.getName());
            dummyClassLoader.loadClass("wbosed.dummy.XResourcesSuperClass");
            dummyClassLoader.loadClass("wbosed.dummy.XTypedArraySuperClass");
            XposedBridge.dummyClassLoader = dummyClassLoader;
            XposedHelpers.setObjectField(myClassLoader, "parent", dummyClassLoader);
        } catch (Throwable throwable) {
            XposedBridge.log(throwable);
            XposedInit.disableResources = true;
        }
    }
}
