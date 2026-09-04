package com.example.nathookdemo;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * BroadcastReceiver that receives hook records from injected target processes.
 *
 * <p>Target processes (e.g. com.demo.addapp) run the same module code via the Vector
 * injection. When they intercept a RegisterNatives call, they send an explicit
 * broadcast to this receiver in the module's main process, which stores the record
 * into the shared DataStore consumed by the UI.</p>
 */
public final class HookReportReceiver extends BroadcastReceiver {
    private static final String TAG = "HookReportReceiver";

    /** 防御性去重：发送端已按指纹去重，这里拦截广播重试 / 广播+Provider 回退双路重复。 */
    private static final Set<String> sStoredKeys = ConcurrentHashMap.newKeySet();

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!"com.example.nathookdemo.REPORT_HOOK_RECORD".equals(intent.getAction())) {
            return;
        }
        intent.setExtrasClassLoader(getClass().getClassLoader());
        try {
            String processName = intent.getStringExtra("process_name");
            RegisterNativesRecord record = intent.getParcelableExtra("record");
            if (processName == null || record == null) {
                Log.w(TAG, "Broadcast received but missing extras");
                return;
            }
            StringBuilder sb = new StringBuilder(processName)
                    .append('|').append(record.className)
                    .append('|').append(record.methodCount);
            if (record.methods != null) {
                for (JniMethodInfo method : record.methods) {
                    sb.append('|').append(method.methodName).append(method.signature);
                }
            }
            if (!sStoredKeys.add(sb.toString())) {
                Log.d(TAG, "Skip duplicate stored record: " + sb);
                return;
            }
            DataStore.getInstance().addHookRecord(processName, record);
            Log.i(TAG, "Stored hook record from " + processName
                    + " class=" + record.className
                    + " methods=" + (record.methods != null ? record.methods.size() : 0));
        } catch (Throwable t) {
            Log.e(TAG, "Failed to handle broadcast: " + t.getMessage(), t);
        }
    }
}
