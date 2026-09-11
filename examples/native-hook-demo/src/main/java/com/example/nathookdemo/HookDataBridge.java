package com.example.nathookdemo;

import android.app.Application;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Bridge class to send hook data from native to Java layer and report to ContentProvider.
 */
public final class HookDataBridge {
    private static final String TAG = "HookDataBridge";
    private static final Uri PROVIDER_URI = Uri.parse("content://com.example.nathookdemo.provider");

    /**
     * 跨进程上报去重指纹（processName|className|method:signature...）。某些 SDK 会以极高频率
     * 重复 RegisterNatives 同一个类（实测 bugly NativeCrashHandler 每秒 10+ 次），不去重时
     * 每次拦截都发一条显式广播，曾造成 36 条/秒的广播风暴，反复拉起模块 UI 进程，最终把宿主
     * 的 GC/帧管线拖死（ANR）。本地 DataStore 仍保留每次拦截的原始记录，仅上报去重。
     */
    private static final Set<String> sReportedKeys = ConcurrentHashMap.newKeySet();

    private static final Handler sAsyncHandler;
    static {
        // 2026-09-11 soLoader baseline: the module ELF is loaded by Vector's soLoader and the
        // RegisterNatives hook is installed by native_init long before this class loads. The
        // linker never sees the module library, so System.loadLibrary here cannot resolve it and
        // would throw UnsatisfiedLinkError, failing <clinit> and every later callback. Guarded:
        // this block no longer needs to load anything.
        try {
            System.loadLibrary("vector_nathook_demo");
        } catch (Throwable t) {
            Log.w(TAG, "System.loadLibrary unavailable under soLoader (hook handled by native_init): " + t);
        }
        HandlerThread thread = new HandlerThread("HookReportThread");
        thread.start();
        sAsyncHandler = new Handler(thread.getLooper());
    }

    private static Context getSystemOrAppContext() {
        try {
            Class<?> atClass = Class.forName("android.app.ActivityThread");
            Method currentAppMethod = atClass.getDeclaredMethod("currentApplication");
            Application app = (Application) currentAppMethod.invoke(null);
            if (app != null) return app.getApplicationContext();

            Method systemCtxMethod = atClass.getDeclaredMethod("currentActivityThread");
            Object at = systemCtxMethod.invoke(null);
            if (at != null) {
                Method getCtx = atClass.getDeclaredMethod("getSystemContext");
                return (Context) getCtx.invoke(at);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to get Context via reflection: " + t.getMessage());
        }
        return null;
    }

    public static native void reportRegisterNatives(
            String processName,
            String className,
            String[] methodNames,
            String[] signatures,
            long[] functionPointers,
            int result);

    public static native void registerProcess(String processName, int pid);

    public static void onNativeInit() {
        Log.i(TAG, "Native bridge initialized");
    }

    public static void onRegisterNativesIntercepted(
            String processName,
            String className,
            String[] methodNames,
            String[] signatures,
            long[] functionPointers,
            int result) {

        if (processName == null || className == null) {
            Log.w(TAG, "Invalid parameters");
            return;
        }

        List<JniMethodInfo> methods = new ArrayList<>();
        if (methodNames != null && signatures != null && functionPointers != null) {
            int count = Math.min(methodNames.length,
                    Math.min(signatures.length, functionPointers.length));
            for (int i = 0; i < count; i++) {
                methods.add(new JniMethodInfo(
                        methodNames[i],
                        signatures[i],
                        functionPointers[i]));
            }
        }

        final RegisterNativesRecord record = new RegisterNativesRecord(
                System.currentTimeMillis(),
                className,
                methods,
                result);

        // 1. 本地存储一份
        DataStore.getInstance().addHookRecord(processName, record);

        Log.i(TAG, String.format("Recorded RegisterNatives in %s: %s (%d methods)",
                processName, className, methods.size()));

        // 2. 如果不是模块自己的主进程，异步通过 ContentProvider 跨进程上报给模块 UI 进程。
        //    同一 (process, class, methods) 指纹只上报一次，防广播风暴。
        if (!"com.example.nathookdemo".equals(processName)) {
            final String key = buildReportKey(processName, record);
            if (!sReportedKeys.add(key)) {
                Log.d(TAG, "Skip duplicate report for " + key);
                return;
            }
            sAsyncHandler.post(() -> reportToProvider(processName, record));
        }
    }

    private static String buildReportKey(String processName, RegisterNativesRecord record) {
        StringBuilder sb = new StringBuilder(processName)
                .append('|').append(record.className)
                .append('|').append(record.methodCount);
        if (record.methods != null) {
            for (JniMethodInfo method : record.methods) {
                sb.append('|').append(method.methodName).append(method.signature);
            }
        }
        return sb.toString();
    }

    private static void reportToProvider(String processName, RegisterNativesRecord record) {
        // 首选：通过显式广播（explicit intent）发给模块 UI 进程的 BroadcastReceiver。
        // 广播是 Android 中最稳定、最不依赖时序的 IPC 方式，可以跨 UID 跨进程带来额外 Bundle。
        try {
            Context ctx = getSystemOrAppContext();
            if (ctx == null) {
                Log.w(TAG, "Context unavailable, cannot report");
                return;
            }
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(
                    "com.example.nathookdemo",
                    "com.example.nathookdemo.HookReportReceiver"));
            intent.setAction("com.example.nathookdemo.REPORT_HOOK_RECORD");
            intent.putExtra("process_name", processName);
            intent.putExtra("record", record);
            ctx.sendBroadcast(intent);
            Log.i(TAG, "Sent broadcast for " + processName + " / " + record.className);
        } catch (Throwable t) {
            Log.e(TAG, "Broadcast failed: " + t.getMessage());
            reportToProviderWithContentResolver(processName, record, 0);
        }
    }

    private static void reportToProviderWithContentResolver(String processName, RegisterNativesRecord record, int attempt) {
        try {
            Context ctx = getSystemOrAppContext();
            if (ctx == null) return;
            Bundle extras = new Bundle();
            extras.putString("process_name", processName);
            extras.putParcelable("record", record);
            Bundle resp = ctx.getContentResolver().call(PROVIDER_URI, "reportSingleRecord", null, extras);
            if (resp != null && resp.getBoolean("success")) {
                Log.i(TAG, "ContentProvider fallback success for " + processName);
            } else if (attempt < 2) {
                sAsyncHandler.postDelayed(() -> reportToProviderWithContentResolver(processName, record, attempt + 1), 1000);
            }
        } catch (Throwable t) {
            Log.e(TAG, "ContentProvider fallback failed: " + t.getMessage());
        }
    }
}
