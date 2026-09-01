package com.example.lsphello;

import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import java.lang.reflect.Method;
import java.util.concurrent.atomic.AtomicBoolean;

import de.wwb.android.wbosed.api.WbModule;
import de.wwb.android.wbosed.api.WbModuleInterface;

/**
 * Minimal Java.ready equivalent:
 *
 * <pre>
 * Java.ready(function () {
 *     var ActivityThread = Java.use("android.app.ActivityThread");
 *     var app = ActivityThread.currentApplication();
 *
 *     if (app) {
 *         console.log("[+] 当前包名: " + String(app.getPackageName()));
 *     } else {
 *         console.log("[+] 当前包名: " + String(ActivityThread.currentPackageName()));
 *     }
 * });
 * </pre>
 *
 * No hooks and no native code are installed here. The callback only waits until the app main
 * thread has finished the current bind step, then resolves the package name through ActivityThread
 * and shows it in a Toast.
 */
public final class HelloModule extends WbModule {
    private static final String TAG = "LspHelloJavaReady";
    private static final String ACTIVITY_THREAD = "android.app.ActivityThread";
    private static final int MAX_APP_WAIT_ATTEMPTS = 20;
    private static final long APP_WAIT_DELAY_MS = 100L;

    private final AtomicBoolean reported = new AtomicBoolean(false);

    @Override
    public void onPackageReady(WbModuleInterface.PackageReadyParam param) {
        Log.i(TAG, "onPackageReady begin first=" + param.isFirstPackage()
                + " package=" + param.getPackageName());
        // LoadedApk is created for framework packages (for example "android") before the
        // host application's package. This example is intended to run once for the process's
        // main package; reflecting ActivityThread from the framework-package callback can trip
        // protected hosts' anti-tamper code before their Application is initialized.
        if (!param.isFirstPackage()) {
            Log.i(TAG, "onPackageReady skipped non-first package=" + param.getPackageName());
            return;
        }
        if (!reported.compareAndSet(false, true)) {
            Log.i(TAG, "onPackageReady skipped duplicate package=" + param.getPackageName());
            return;
        }

        String fallbackPackageName = param.getPackageName();
        Log.i(TAG, "onPackageReady before Handler.post package=" + fallbackPackageName);
        Handler mainHandler = new Handler(Looper.getMainLooper());
        mainHandler.post(() -> reportPackageNameWhenReady(mainHandler, fallbackPackageName, 0));
        Log.i(TAG, "onPackageReady after Handler.post package=" + fallbackPackageName);
    }

    private void reportPackageNameWhenReady(
            Handler mainHandler,
            String fallbackPackageName,
            int attempt
    ) {
        Log.i(TAG, "reportPackageNameWhenReady begin attempt=" + attempt);
        ResolvedPackage resolved = resolvePackageName(fallbackPackageName);
        if (resolved.application == null && attempt < MAX_APP_WAIT_ATTEMPTS) {
            mainHandler.postDelayed(
                    () -> reportPackageNameWhenReady(
                            mainHandler,
                            fallbackPackageName,
                            attempt + 1
                    ),
                    APP_WAIT_DELAY_MS
            );
            return;
        }

        String message = "[+] 当前包名: " + resolved.packageName;
        Log.i(TAG, message);

        Context toastContext = resolved.application != null
                ? resolved.application
                : resolveSystemContext();
        if (toastContext == null) {
            Log.w(TAG, "Toast skipped because no Context is available");
            return;
        }

        try {
            Toast.makeText(toastContext, message, Toast.LENGTH_SHORT).show();
        } catch (Throwable throwable) {
            Log.e(TAG, "Toast failed", throwable);
        }
    }

    private static ResolvedPackage resolvePackageName(String fallbackPackageName) {
        try {
            Log.i(TAG, "resolvePackageName before Class.forName");
            Class<?> activityThreadClass = Class.forName(ACTIVITY_THREAD);

            Log.i(TAG, "resolvePackageName before currentApplication");
            Application application = invokeStaticApplication(
                    activityThreadClass,
                    "currentApplication"
            );
            Log.i(TAG, "resolvePackageName after currentApplication");
            if (application != null) {
                return new ResolvedPackage(application, application.getPackageName());
            }

            String currentPackageName = invokeStaticString(
                    activityThreadClass,
                    "currentPackageName"
            );
            if (currentPackageName != null && !currentPackageName.isEmpty()) {
                return new ResolvedPackage(null, currentPackageName);
            }
        } catch (Throwable throwable) {
            Log.e(TAG, "Failed to resolve package name through ActivityThread", throwable);
        }

        String packageName = fallbackPackageName != null && !fallbackPackageName.isEmpty()
                ? fallbackPackageName
                : "<unknown>";
        return new ResolvedPackage(null, packageName);
    }

    private static Context resolveSystemContext() {
        try {
            Class<?> activityThreadClass = Class.forName(ACTIVITY_THREAD);
            Method currentActivityThread = activityThreadClass.getDeclaredMethod(
                    "currentActivityThread"
            );
            currentActivityThread.setAccessible(true);
            Object activityThread = currentActivityThread.invoke(null);
            if (activityThread == null) {
                return null;
            }

            Method getSystemContext = activityThreadClass.getDeclaredMethod("getSystemContext");
            getSystemContext.setAccessible(true);
            Object context = getSystemContext.invoke(activityThread);
            return context instanceof Context ? (Context) context : null;
        } catch (Throwable throwable) {
            Log.e(TAG, "Failed to resolve system Context", throwable);
            return null;
        }
    }

    private static Application invokeStaticApplication(Class<?> owner, String methodName)
            throws Exception {
        Method method = owner.getDeclaredMethod(methodName);
        method.setAccessible(true);
        Object value = method.invoke(null);
        return value instanceof Application ? (Application) value : null;
    }

    private static String invokeStaticString(Class<?> owner, String methodName) throws Exception {
        Method method = owner.getDeclaredMethod(methodName);
        method.setAccessible(true);
        Object value = method.invoke(null);
        return value != null ? String.valueOf(value) : null;
    }

    private static final class ResolvedPackage {
        final Application application;
        final String packageName;

        ResolvedPackage(Application application, String packageName) {
            this.application = application;
            this.packageName = packageName;
        }
    }
}
