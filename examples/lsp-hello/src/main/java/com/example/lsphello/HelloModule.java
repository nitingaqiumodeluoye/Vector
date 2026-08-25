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
        if (!reported.compareAndSet(false, true)) {
            return;
        }

        String fallbackPackageName = param.getPackageName();
        Handler mainHandler = new Handler(Looper.getMainLooper());
        mainHandler.post(() -> reportPackageNameWhenReady(mainHandler, fallbackPackageName, 0));
    }

    private void reportPackageNameWhenReady(
            Handler mainHandler,
            String fallbackPackageName,
            int attempt
    ) {
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
            Class<?> activityThreadClass = Class.forName(ACTIVITY_THREAD);

            Application application = invokeStaticApplication(
                    activityThreadClass,
                    "currentApplication"
            );
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
