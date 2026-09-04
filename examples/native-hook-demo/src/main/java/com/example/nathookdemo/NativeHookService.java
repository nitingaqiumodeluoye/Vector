package com.example.nathookdemo;

import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.Log;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Service for efficient cross-process communication of RegisterNatives hook data.
 * 
 * <p>This service uses Messenger + SharedMemory for high-performance IPC:
 * <ul>
 *   <li>Messenger: Lightweight message passing for control commands</li>
 *   <li>SharedMemory: High-bandwidth data transfer for large payloads</li>
 * </ul>
 * 
 * <p>Architecture:
 * <pre>
 *   Target Process (Hook)          Service (DataStore)           Module UI
 *   ┌─────────────────┐           ┌─────────────────┐          ┌─────────────┐
 *   │ HookDataBridge  │──SharedMem──>│ NativeHookService │──Binder──>│ MainActivity │
 *   │   (Native)      │  Messenger  │   (Central Hub)  │          │   (UI)      │
 *   └─────────────────┘           └─────────────────┘          └─────────────┘
 * </pre>
 */
public final class NativeHookService {
    private static final String TAG = "NativeHookService";
    
    // Message types
    public static final int MSG_REGISTER_PROCESS = 1;
    public static final int MSG_REPORT_HOOK_DATA = 2;
    public static final int MSG_GET_PROCESSES = 3;
    public static final int MSG_GET_HOOK_DATA = 4;
    public static final int MSG_CLEAR_PROCESS = 5;
    public static final int MSG_CLEAR_ALL = 6;
    
    // Bundle keys
    public static final String KEY_PROCESS_NAME = "process_name";
    public static final String KEY_PID = "pid";
    public static final String KEY_RECORDS_COUNT = "records_count";
    public static final String KEY_TIMESTAMP = "timestamp";
    public static final String KEY_CLASS_NAME = "class_name";
    public static final String KEY_METHOD_NAMES = "method_names";
    public static final String KEY_SIGNATURES = "signatures";
    public static final String KEY_FUNCTION_POINTERS = "function_pointers";
    public static final String KEY_RESULT = "result";
    
    // SharedMemory names
    public static final String SHM_NAME_PROCESSES = "native_hook_processes";
    public static final String SHM_NAME_HOOK_DATA_PREFIX = "native_hook_data_";
    
    private static volatile NativeHookService sInstance;
    
    private final DataStore mDataStore;
    private final Handler mHandler;
    private final Map<Messenger, String> mClientProcesses = new ConcurrentHashMap<>();
    
    private NativeHookService() {
        mDataStore = DataStore.getInstance();
        
        HandlerThread thread = new HandlerThread("NativeHookService");
        thread.start();
        mHandler = new ServiceHandler(thread.getLooper());
    }
    
    public static NativeHookService getInstance() {
        if (sInstance == null) {
            synchronized (NativeHookService.class) {
                if (sInstance == null) {
                    sInstance = new NativeHookService();
                }
            }
        }
        return sInstance;
    }
    
    /**
     * Handle incoming messages from target processes and UI.
     */
    private class ServiceHandler extends Handler {
        ServiceHandler(Looper looper) {
            super(looper);
        }
        
        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
                case MSG_REGISTER_PROCESS:
                    handleRegisterProcess(msg);
                    break;
                    
                case MSG_REPORT_HOOK_DATA:
                    handleReportHookData(msg);
                    break;
                    
                case MSG_GET_PROCESSES:
                    handleGetProcesses(msg);
                    break;
                    
                case MSG_GET_HOOK_DATA:
                    handleGetHookData(msg);
                    break;
                    
                case MSG_CLEAR_PROCESS:
                    handleClearProcess(msg);
                    break;
                    
                case MSG_CLEAR_ALL:
                    handleClearAll(msg);
                    break;
                    
                default:
                    Log.w(TAG, "Unknown message type: " + msg.what);
            }
        }
    }
    
    private void handleRegisterProcess(Message msg) {
        Bundle data = msg.getData();
        String processName = data.getString(KEY_PROCESS_NAME);
        int pid = data.getInt(KEY_PID);
        
        if (processName != null) {
            mDataStore.registerProcess(processName, pid);
            Log.d(TAG, "Registered process: " + processName);
        }
    }
    
    private void handleReportHookData(Message msg) {
        Bundle data = msg.getData();
        String processName = data.getString(KEY_PROCESS_NAME);
        String className = data.getString(KEY_CLASS_NAME);
        String[] methodNames = data.getStringArray(KEY_METHOD_NAMES);
        String[] signatures = data.getStringArray(KEY_SIGNATURES);
        long[] functionPointers = data.getLongArray(KEY_FUNCTION_POINTERS);
        int result = data.getInt(KEY_RESULT);
        
        if (processName != null && className != null) {
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
            
            RegisterNativesRecord record = new RegisterNativesRecord(
                    System.currentTimeMillis(),
                    className,
                    methods,
                    result);
            
            mDataStore.addHookRecord(processName, record);
            Log.d(TAG, "Reported hook data for: " + processName + "." + className);
        }
    }
    
    private void handleGetProcesses(Message msg) {
        ProcessInfo[] processes = mDataStore.getInjectedProcesses();
        
        Bundle reply = new Bundle();
        reply.putParcelableArray("processes", processes);
        
        try {
            msg.replyTo.send(Message.obtain(null, MSG_GET_PROCESSES, reply));
        } catch (Exception e) {
            Log.e(TAG, "Failed to reply with processes", e);
        }
    }
    
    private void handleGetHookData(Message msg) {
        Bundle data = msg.getData();
        String processName = data.getString(KEY_PROCESS_NAME);
        
        if (processName != null) {
            RegisterNativesRecord[] records = mDataStore.getRegisterNativesData(processName);
            
            Bundle reply = new Bundle();
            reply.putParcelableArray("records", records);
            
            try {
                msg.replyTo.send(Message.obtain(null, MSG_GET_HOOK_DATA, reply));
            } catch (Exception e) {
                Log.e(TAG, "Failed to reply with hook data", e);
            }
        }
    }
    
    private void handleClearProcess(Message msg) {
        Bundle data = msg.getData();
        String processName = data.getString(KEY_PROCESS_NAME);
        
        if (processName != null) {
            mDataStore.clearProcessData(processName);
            Log.d(TAG, "Cleared data for process: " + processName);
        }
    }
    
    private void handleClearAll(Message msg) {
        mDataStore.clearAllData();
        Log.d(TAG, "Cleared all data");
    }
    
    /**
     * Send a message to the service.
     */
    public static void sendMessage(Messenger service, int what, Bundle data, Messenger replyTo) {
        if (service == null) {
            Log.e(TAG, "Service messenger is null");
            return;
        }
        
        try {
            Message msg = Message.obtain(null, what);
            msg.setData(data);
            msg.replyTo = replyTo;
            service.send(msg);
        } catch (Exception e) {
            Log.e(TAG, "Failed to send message", e);
        }
    }
    
    /**
     * Create a messenger for the service.
     */
    public Messenger getMessenger() {
        return new Messenger(mHandler);
    }
}
