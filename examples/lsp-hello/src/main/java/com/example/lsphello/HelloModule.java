package com.example.lsphello;

import android.app.Application;
import android.app.Activity;
import android.app.Dialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.view.Gravity;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.FileInputStream;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.Set;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.ConcurrentHashMap;

import de.wwb.android.wbosed.api.WbInterface;
import de.wwb.android.wbosed.api.WbModule;
import de.wwb.android.wbosed.api.WbModuleInterface;
import org.matrix.vector.nativebridge.NativeAPI;

/**
 * Launch-only LibXposed module port of the rustFrida Java.ready score hook:
 *
 * <pre>
 * Java.ready(function () {
 *     Java.setStealth(1);
 *     var EnvironmentFragment = Java.use("com.xff.launch.ui.environment.EnvironmentFragment");
 *     var updateUi = EnvironmentFragment.updateUI.overload(
 *         "com.xff.launch.model.DetectionResult"
 *     );
 *     updateUi.impl = function (result) {
 *         this.$orig(result);
 *         ...
 *     };
 * });
 * </pre>
 *
 * Vector currently selects ART stealth mode at native bootstrap time rather than exposing
 * rustFrida's Java.setStealth runtime API to modules. This module logs the same intent, then
 * installs the equivalent Java method hook through Vector's selected ART backend.
 */
public final class HelloModule extends WbModule {
    private static final String TAG = "LspHelloShadow";
    private static final String TARGET_PACKAGE = "com.xff.launch";
    private static final String ENVIRONMENT_FRAGMENT =
            "com.xff.launch.ui.environment.EnvironmentFragment";
    private static final String DETECTION_RESULT =
            "com.xff.launch.model.DetectionResult";
    private static final int RUST_FRIDA_STEALTH_WXSHADOW = 1;

    private final AtomicBoolean stealthLogged = new AtomicBoolean(false);
    private final AtomicBoolean hookInstalled = new AtomicBoolean(false);
    private final AtomicBoolean consoleHookInstalled = new AtomicBoolean(false);
    private final AtomicBoolean nativeProbeStarted = new AtomicBoolean(false);
    private final Set<String> toastPackages = ConcurrentHashMap.newKeySet();
    private final Deque<String> consoleLines = new ArrayDeque<>();
    private final Object consoleLock = new Object();
    private volatile Button consoleButton;
    private volatile WindowManager consoleWindowManager;
    private volatile String moduleProcessName = "<unloaded>";

    @Override
    public void onModuleLoaded(WbModuleInterface.ModuleLoadedParam param) {
        moduleProcessName = param.getProcessName();
        startNativeRegisterNativesProbe();
    }

    @Override
    public void onPackageLoaded(WbModuleInterface.PackageLoadedParam param) {
        String packageName = callbackPackageName(param.getPackageName());
        logPackageReady("onPackageLoaded", packageName);
        if (TARGET_PACKAGE.equals(packageName)) {
            installConsoleUiHook(param.getDefaultClassLoader());
            installLaunchScoreHook(param.getDefaultClassLoader(), "onPackageLoaded");
        }
    }

    @Override
    public void onPackageReady(WbModuleInterface.PackageReadyParam param) {
        String packageName = callbackPackageName(param.getPackageName());
        logPackageReady("onPackageReady", packageName);
        if (TARGET_PACKAGE.equals(packageName)) {
            installConsoleUiHook(param.getClassLoader());
            installLaunchScoreHook(param.getClassLoader(), "onPackageReady");
        }
    }

    private void logPackageReady(String reason, String packageName) {

        if (stealthLogged.compareAndSet(false, true)) {
            consoleLog(Log.INFO, TAG, "[score-1000] Java.setStealth("
                    + RUST_FRIDA_STEALTH_WXSHADOW
                    + ") requested; Vector uses bootstrap-selected ART stealth"
                    + " pid=" + Process.myPid()
                    + " process=" + resolvedProcessName());
        }

        showPackageToastOnce(packageName, reason);
        consoleLog(Log.INFO, TAG, "package ready package=" + packageName
                + " reason=" + reason
                + " pid=" + Process.myPid()
                + " process=" + resolvedProcessName());
    }

    private static String callbackPackageName(String callbackPackageName) {
        if (callbackPackageName != null && !callbackPackageName.isEmpty()) {
            return callbackPackageName;
        }
        return actualPackageName(null);
    }

    private void startNativeRegisterNativesProbe() {
        if (!nativeProbeStarted.compareAndSet(false, true)) {
            return;
        }
        try {
            boolean installed = NativeAPI.installRegisterNativesProbe();
            consoleLog(Log.INFO, TAG, "[native-register-natives] core probe active=" + installed
                    + " pid=" + Process.myPid()
                    + " process=" + resolvedProcessName());
            if (!installed) {
                nativeProbeStarted.set(false);
            }
        } catch (Throwable throwable) {
            nativeProbeStarted.set(false);
            consoleLog(Log.ERROR, TAG, "[native-register-natives] probe unavailable", throwable);
        }
    }

    private void installConsoleUiHook(ClassLoader classLoader) {
        if (!consoleHookInstalled.compareAndSet(false, true)) {
            return;
        }
        try {
            Class<?> activityClass = Class.forName("android.app.Activity", false, classLoader);
            Method onResume = activityClass.getDeclaredMethod("onResume");
            onResume.setAccessible(true);
            hook(onResume)
                    .setId("lsphello-console-activity-resume")
                    .intercept(new WbInterface.Hooker() {
                        @Override
                        public Object intercept(WbInterface.Chain chain) throws Throwable {
                            Object result = chain.proceed();
                            if (chain.getThisObject() instanceof Activity) {
                                showConsoleButton((Activity) chain.getThisObject());
                            }
                            return result;
                        }
                    });
            consoleLog(Log.INFO, TAG, "console UI hook installed");
        } catch (Throwable throwable) {
            consoleHookInstalled.set(false);
            consoleLog(Log.ERROR, TAG, "failed to install console UI hook", throwable);
        }
    }

    private void showConsoleButton(Activity activity) {
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            try {
                if (activity.isFinishing() || (Build.VERSION.SDK_INT >= 17 && activity.isDestroyed())) {
                    return;
                }
                Window window = activity.getWindow();
                if (window == null || window.getDecorView().getWindowToken() == null) {
                    return;
                }

                removeConsoleButton();
                Button button = new Button(activity);
                button.setText("LSP");
                button.setTextColor(Color.WHITE);
                button.setTextSize(11.0f);
                button.setAllCaps(false);
                button.setTypeface(Typeface.DEFAULT_BOLD);
                button.setPadding(dp(activity, 8), 0, dp(activity, 8), 0);
                button.setBackgroundColor(Color.rgb(35, 95, 165));
                button.setContentDescription("LSP Hello console");
                button.setOnClickListener(view -> showConsoleDialog(activity));

                WindowManager windowManager = (WindowManager) activity.getSystemService(Activity.WINDOW_SERVICE);
                WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.WRAP_CONTENT,
                        dp(activity, 38),
                        WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                                | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                        android.graphics.PixelFormat.TRANSLUCENT);
                params.gravity = Gravity.TOP | Gravity.START;
                params.x = dp(activity, 6);
                params.y = dp(activity, 28);
                params.token = window.getDecorView().getWindowToken();
                windowManager.addView(button, params);
                consoleButton = button;
                consoleWindowManager = windowManager;
                consoleLog(Log.INFO, TAG, "console button attached activity="
                        + activity.getClass().getName());
            } catch (Throwable throwable) {
                consoleLog(Log.ERROR, TAG, "failed to attach console button", throwable);
            }
        }, 250L);
    }

    private void removeConsoleButton() {
        Button button = consoleButton;
        WindowManager windowManager = consoleWindowManager;
        consoleButton = null;
        consoleWindowManager = null;
        if (button != null && windowManager != null) {
            try {
                windowManager.removeViewImmediate(button);
            } catch (Throwable ignored) {
            }
        }
    }

    private void showConsoleDialog(Activity activity) {
        try {
            Dialog dialog = new Dialog(activity);
            dialog.setTitle("LSP Hello console");

            LinearLayout root = new LinearLayout(activity);
            root.setOrientation(LinearLayout.VERTICAL);
            root.setPadding(dp(activity, 12), dp(activity, 8), dp(activity, 12), dp(activity, 8));

            TextView output = new TextView(activity);
            output.setText(getConsoleText());
            output.setTextColor(Color.WHITE);
            output.setTextSize(12.0f);
            output.setTypeface(Typeface.MONOSPACE);
            output.setTextIsSelectable(true);
            output.setPadding(dp(activity, 8), dp(activity, 8), dp(activity, 8), dp(activity, 8));

            ScrollView scroll = new ScrollView(activity);
            scroll.setFillViewport(true);
            scroll.addView(output, new ScrollView.LayoutParams(
                    ScrollView.LayoutParams.MATCH_PARENT, ScrollView.LayoutParams.WRAP_CONTENT));
            root.addView(scroll, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

            Button close = new Button(activity);
            close.setText("关闭");
            close.setOnClickListener(view -> dialog.dismiss());
            root.addView(close, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(activity, 44)));

            dialog.setContentView(root);
            dialog.setOnShowListener(ignored -> {
                Window dialogWindow = dialog.getWindow();
                if (dialogWindow != null) {
                    dialogWindow.setLayout(
                            (int) (activity.getResources().getDisplayMetrics().widthPixels * 0.92f),
                            (int) (activity.getResources().getDisplayMetrics().heightPixels * 0.72f));
                }
            });
            dialog.show();
        } catch (Throwable throwable) {
            consoleLog(Log.ERROR, TAG, "failed to show console dialog", throwable);
        }
    }

    private String getConsoleText() {
        synchronized (consoleLock) {
            if (consoleLines.isEmpty()) {
                return "(no console.log messages)";
            }
            StringBuilder builder = new StringBuilder();
            for (String line : consoleLines) {
                builder.append(line).append('\n');
            }
            return builder.toString();
        }
    }

    private void consoleLog(int priority, String tag, String message) {
        recordConsoleLine(priority, message);
        log(priority, tag, message);
    }

    private void consoleLog(int priority, String tag, String message, Throwable throwable) {
        recordConsoleLine(priority, message + ": " + throwable);
        log(priority, tag, message, throwable);
    }

    private void recordConsoleLine(int priority, String message) {
        String level;
        switch (priority) {
            case Log.ERROR: level = "E"; break;
            case Log.WARN: level = "W"; break;
            case Log.DEBUG: level = "D"; break;
            default: level = "I";
        }
        String line = String.format(Locale.ROOT, "%s/%s %s", level, TAG, message);
        synchronized (consoleLock) {
            if (consoleLines.size() >= 300) {
                consoleLines.removeFirst();
            }
            consoleLines.addLast(line);
        }
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private synchronized void installLaunchScoreHook(ClassLoader classLoader, String reason) {
        if (hookInstalled.get()) {
            return;
        }

        try {
            Class<?> fragmentClass = Class.forName(ENVIRONMENT_FRAGMENT, false, classLoader);
            Class<?> resultClass = Class.forName(DETECTION_RESULT, false, classLoader);
            Method updateUi = fragmentClass.getDeclaredMethod("updateUI", resultClass);
            updateUi.setAccessible(true);

            hook(updateUi)
                    .setId("launch-score-1000-update-ui")
                    .intercept(new WbInterface.Hooker() {
                        @Override
                        public Object intercept(WbInterface.Chain chain) throws Throwable {
                            Object result = chain.getArg(0);
                            Object originalReturn = chain.proceed();

                            try {
                                updateLaunchScoreUi(chain.getThisObject(), result);
                            } catch (Throwable throwable) {
                                consoleLog(Log.ERROR, TAG, "[score-1000] display update failed", throwable);
                            }
                            return originalReturn;
                        }
                    });

            hookInstalled.set(true);
            consoleLog(Log.INFO, TAG, "[score-1000] EnvironmentFragment.updateUI hook installed"
                    + " reason=" + reason
                    + " method=" + updateUi
                    + " pid=" + Process.myPid()
                    + " process=" + resolvedProcessName());
        } catch (ClassNotFoundException | NoSuchMethodException expectedEarlyFailure) {
            consoleLog(Log.WARN, TAG, "[score-1000] launch hook not ready reason=" + reason
                    + " error=" + expectedEarlyFailure);
        } catch (Throwable throwable) {
            consoleLog(Log.ERROR, TAG, "[score-1000] failed to install launch updateUI hook"
                    + " reason=" + reason, throwable);
        }
    }

    private void updateLaunchScoreUi(Object fragment, Object detectionResult) throws Throwable {
        if (fragment == null || detectionResult == null) {
            return;
        }

        double rawScore = toDouble(invokeNoArg(detectionResult, "getOverallScore"));
        double displayScore = Math.max(0.0d, Math.min(1000.0d, rawScore * 10.0d));
        String displayScoreText = formatScore(displayScore);

        Object scoreView = resolveViewField(fragment, "tvScore");
        if (scoreView instanceof TextView) {
            ((TextView) scoreView).setText(displayScoreText + "/1000");
        } else if (scoreView != null) {
            invokeSetText(scoreView, displayScoreText + "/1000");
        }

        Object progressView = resolveViewField(fragment, "progressScore");
        int progress = (int) Math.max(0, Math.min(1000, Math.round(displayScore)));
        if (progressView instanceof ProgressBar) {
            ProgressBar bar = (ProgressBar) progressView;
            bar.setMax(1000);
            bar.setProgress(progress);
        } else if (progressView != null) {
            invokeInt(progressView, "setMax", 1000);
            invokeInt(progressView, "setProgress", progress);
        }

        showPackageToastOnce(callbackPackageName(TARGET_PACKAGE), "updateUI");
        consoleLog(Log.INFO, TAG, "[score-1000] " + formatScore(rawScore)
                + "/100 -> " + displayScoreText + "/1000"
                + " pid=" + Process.myPid()
                + " process=" + resolvedProcessName());
    }

    private static Object resolveViewField(Object owner, String fieldName) throws Throwable {
        Object value = readField(owner, fieldName);
        return unwrapValue(value, 0);
    }

    private static Object unwrapValue(Object value, int depth) throws Throwable {
        if (value == null || depth >= 3 || value instanceof TextView || value instanceof ProgressBar) {
            return value;
        }

        try {
            Object unwrapped = readField(value, "value");
            if (unwrapped != value && unwrapped != null) {
                return unwrapValue(unwrapped, depth + 1);
            }
        } catch (NoSuchFieldException ignored) {
        }

        for (String methodName : new String[] {"getValue", "get"}) {
            try {
                Method method = findNoArgMethod(value.getClass(), methodName);
                method.setAccessible(true);
                Object unwrapped = method.invoke(value);
                if (unwrapped != value && unwrapped != null) {
                    return unwrapValue(unwrapped, depth + 1);
                }
            } catch (NoSuchMethodException ignored) {
            }
        }

        return value;
    }

    private static Object readField(Object owner, String fieldName) throws Throwable {
        Field field = findField(owner.getClass(), fieldName);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static Field findField(Class<?> type, String name) throws NoSuchFieldException {
        Class<?> cursor = type;
        while (cursor != null) {
            try {
                return cursor.getDeclaredField(name);
            } catch (NoSuchFieldException ignored) {
                cursor = cursor.getSuperclass();
            }
        }
        throw new NoSuchFieldException(type.getName() + "#" + name);
    }

    private static Object invokeNoArg(Object receiver, String methodName) throws Throwable {
        Method method = findNoArgMethod(receiver.getClass(), methodName);
        method.setAccessible(true);
        return method.invoke(receiver);
    }

    private static Method findNoArgMethod(Class<?> type, String name) throws NoSuchMethodException {
        Class<?> cursor = type;
        while (cursor != null) {
            try {
                return cursor.getDeclaredMethod(name);
            } catch (NoSuchMethodException ignored) {
                cursor = cursor.getSuperclass();
            }
        }
        throw new NoSuchMethodException(type.getName() + "#" + name + "()");
    }

    private static double toDouble(Object value) {
        if (value instanceof Number) {
            return ((Number) value).doubleValue();
        }
        if (value == null) {
            return 0.0d;
        }
        return Double.parseDouble(String.valueOf(value));
    }

    private static String formatScore(double value) {
        double rounded = Math.rint(value);
        if (Math.abs(value - rounded) < 0.000001d) {
            return String.valueOf((long) rounded);
        }
        return String.valueOf(value);
    }

    private static void invokeSetText(Object receiver, CharSequence text) throws Throwable {
        Method method;
        try {
            method = receiver.getClass().getMethod("setText", CharSequence.class);
        } catch (NoSuchMethodException ignored) {
            method = receiver.getClass().getMethod("setText", String.class);
        }
        method.invoke(receiver, text.toString());
    }

    private static void invokeInt(Object receiver, String methodName, int value) throws Throwable {
        Method method = receiver.getClass().getMethod(methodName, int.class);
        method.invoke(receiver, value);
    }

    private void showPackageToastOnce(String packageName, String reason) {
        String resolvedPackageName = callbackPackageName(packageName);
        if (!toastPackages.add(resolvedPackageName)) {
            return;
        }
        Application app = currentApplication();
        if (app == null) {
            consoleLog(Log.DEBUG, TAG, "toast skipped: ActivityThread.currentApplication is null"
                    + " reason=" + reason
                    + " package=" + resolvedPackageName
                    + " pid=" + Process.myPid()
                    + " process=" + resolvedProcessName());
            toastPackages.remove(resolvedPackageName);
            return;
        }

        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                String message = "当前包名为：" + resolvedPackageName;
                Toast.makeText(app, message, Toast.LENGTH_LONG).show();
                consoleLog(Log.INFO, TAG, "toast shown: " + message
                        + " reason=" + reason
                        + " pid=" + Process.myPid()
                        + " process=" + resolvedProcessName());
            } catch (Throwable throwable) {
                toastPackages.remove(resolvedPackageName);
                consoleLog(Log.ERROR, TAG, "failed to show package toast"
                        + " reason=" + reason
                        + " package=" + resolvedPackageName, throwable);
            }
        });
    }

    private static String actualPackageName(String fallback) {
        Application app = currentApplication();
        if (app != null) {
            String packageName = app.getPackageName();
            if (packageName != null && !packageName.isEmpty()) {
                return packageName;
            }
        }

        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentPackageName = activityThread.getDeclaredMethod("currentPackageName");
            currentPackageName.setAccessible(true);
            Object result = currentPackageName.invoke(null);
            if (result instanceof String && !((String) result).isEmpty()) {
                return (String) result;
            }
        } catch (Throwable ignored) {
        }
        return fallback == null || fallback.isEmpty() ? "<unknown>" : fallback;
    }

    private static Application currentApplication() {
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentApplication = activityThread.getDeclaredMethod("currentApplication");
            currentApplication.setAccessible(true);
            Object app = currentApplication.invoke(null);
            if (app instanceof Application) {
                return (Application) app;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private String resolvedProcessName() {
        String fromModule = moduleProcessName;
        if (fromModule != null && !fromModule.isEmpty() && !"<unloaded>".equals(fromModule)) {
            return fromModule;
        }
        return currentProcessName();
    }

    private static String currentProcessName() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            String processName = Application.getProcessName();
            if (processName != null && !processName.isEmpty()) {
                return processName;
            }
        }
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentProcessName = activityThread.getDeclaredMethod("currentProcessName");
            currentProcessName.setAccessible(true);
            Object result = currentProcessName.invoke(null);
            if (result instanceof String && !((String) result).isEmpty()) {
                return (String) result;
            }
        } catch (Throwable ignored) {
        }
        try (FileInputStream input = new FileInputStream("/proc/self/cmdline")) {
            byte[] buffer = new byte[256];
            int count = input.read(buffer);
            if (count > 0) {
                int end = 0;
                while (end < count && buffer[end] != 0) {
                    end++;
                }
                if (end > 0) {
                    return new String(buffer, 0, end);
                }
            }
        } catch (IOException ignored) {
        }
        return "<unknown>";
    }
}
