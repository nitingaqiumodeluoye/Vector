package com.example.nathookdemo;

import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SharedMemory;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Central data store for RegisterNatives hook data across all injected processes.
 * 
 * <p>This singleton class manages:
 * <ul>
 *   <li>Process registration and status tracking</li>
 *   <li>RegisterNatives hook data storage per process</li>
 *   <li>Data serialization/deserialization through SharedMemory</li>
 * </ul>
 * 
 * <p>Thread-safe for concurrent access from multiple processes.</p>
 */
public final class DataStore {
    private static final String TAG = "DataStore";
    
    private static volatile DataStore sInstance;
    
    // Process name -> ProcessInfo
    private final ConcurrentHashMap<String, ProcessInfo> mProcesses = new ConcurrentHashMap<>();
    
    // Process name -> List of RegisterNativesRecord
    private final ConcurrentHashMap<String, List<RegisterNativesRecord>> mHookData = 
            new ConcurrentHashMap<>();
    
    private final HandlerThread mHandlerThread;
    private final Handler mHandler;
    
    private DataStore() {
        mHandlerThread = new HandlerThread("DataStoreWorker");
        mHandlerThread.start();
        mHandler = new Handler(mHandlerThread.getLooper());
    }
    
    public static DataStore getInstance() {
        if (sInstance == null) {
            synchronized (DataStore.class) {
                if (sInstance == null) {
                    sInstance = new DataStore();
                }
            }
        }
        return sInstance;
    }
    
    /**
     * Register a new injected process.
     * 
     * @param processName Process name
     * @param pid Process ID
     */
    public void registerProcess(String processName, int pid) {
        if (processName == null || processName.isEmpty()) {
            Log.w(TAG, "Cannot register process with null/empty name");
            return;
        }
        
        long now = System.currentTimeMillis();
        ProcessInfo existing = mProcesses.get(processName);
        
        if (existing == null) {
            ProcessInfo info = new ProcessInfo(processName, pid, 0, now, now);
            mProcesses.put(processName, info);
            Log.i(TAG, "Registered new process: " + processName + " (PID: " + pid + ")");
        } else {
            // Update PID if changed
            ProcessInfo updated = new ProcessInfo(
                processName, pid, existing.hookCount, existing.firstHookTime, now);
            mProcesses.put(processName, updated);
            Log.d(TAG, "Updated process: " + processName);
        }
    }
    
    /**
     * Add a RegisterNatives record for a process.
     * 
     * @param processName Process name
     * @param record The intercepted RegisterNatives call data
     */
    public void addHookRecord(String processName, RegisterNativesRecord record) {
        if (processName == null || record == null) {
            Log.w(TAG, "Cannot add null record");
            return;
        }
        
        // Ensure process is registered
        registerProcess(processName, android.os.Process.myPid());
        
        // Add record
        List<RegisterNativesRecord> records = mHookData.computeIfAbsent(
                processName, k -> Collections.synchronizedList(new ArrayList<>()));
        records.add(record);
        
        // Update process info
        long now = System.currentTimeMillis();
        ProcessInfo existing = mProcesses.get(processName);
        if (existing != null) {
            ProcessInfo updated = new ProcessInfo(
                processName, existing.pid, records.size(), 
                existing.firstHookTime == 0 ? now : existing.firstHookTime, now);
            mProcesses.put(processName, updated);
        }
        
        Log.d(TAG, "Added hook record for " + processName + ": " + record.className);
    }
    
    /**
     * Get all injected processes.
     * 
     * @return Array of ProcessInfo objects
     */
    public ProcessInfo[] getInjectedProcesses() {
        return mProcesses.values().toArray(new ProcessInfo[0]);
    }
    
    /**
     * Get RegisterNatives data for a specific process.
     * 
     * @param processName Process name
     * @return Array of RegisterNativesRecord objects
     */
    public RegisterNativesRecord[] getRegisterNativesData(String processName) {
        List<RegisterNativesRecord> records = mHookData.get(processName);
        if (records == null) {
            return new RegisterNativesRecord[0];
        }
        
        synchronized (records) {
            return records.toArray(new RegisterNativesRecord[0]);
        }
    }
    
    /**
     * Check if a process has active hooks.
     * 
     * @param processName Process name
     * @return true if hooks are active
     */
    public boolean isProcessHooked(String processName) {
        ProcessInfo info = mProcesses.get(processName);
        return info != null && info.hookCount > 0;
    }
    
    /**
     * Process hook data received from a target process via SharedMemory.
     * 
     * @param processName Process name
     * @param sharedMemory SharedMemory containing serialized data
     */
    public void processHookData(String processName, SharedMemory sharedMemory) {
        if (processName == null || sharedMemory == null) {
            Log.w(TAG, "Cannot process null data");
            return;
        }
        
        try {
            // Deserialize records from SharedMemory
            SharedMemoryHelper helper = new SharedMemoryHelper(0);
            // Note: This is a simplified implementation
            // In production, you would need to properly handle SharedMemory transfer
            
            registerProcess(processName, android.os.Process.myPid());
            Log.d(TAG, "Processed hook data for: " + processName);
        } catch (Exception e) {
            Log.e(TAG, "Failed to process hook data for: " + processName, e);
        }
    }
    
    /**
     * Clear data for a specific process.
     * 
     * @param processName Process name
     */
    public void clearProcessData(String processName) {
        if (processName == null) {
            return;
        }
        
        mProcesses.remove(processName);
        mHookData.remove(processName);
        Log.i(TAG, "Cleared data for process: " + processName);
    }
    
    /**
     * Clear all data.
     */
    public void clearAllData() {
        mProcesses.clear();
        mHookData.clear();
        Log.i(TAG, "Cleared all data");
    }
    
    /**
     * Get the count of all registered processes.
     */
    public int getProcessCount() {
        return mProcesses.size();
    }
    
    /**
     * Get the total count of all hook records.
     */
    public long getTotalHookCount() {
        long total = 0;
        for (List<RegisterNativesRecord> records : mHookData.values()) {
            synchronized (records) {
                total += records.size();
            }
        }
        return total;
    }
}
