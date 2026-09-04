package com.example.nathookdemo;

import android.os.SharedMemory;

/**
 * AIDL interface for communicating between the module process and injected target processes.
 * 
 * <p>This interface enables efficient transfer of RegisterNatives hook data:
 * <ul>
 *   <li>Module process: UI (app list, details view)</li>
 *   <li>Target processes: Native hook running in each injected app</li>
 * </ul>
 * 
 * <p>Data transfer uses SharedMemory for large payloads to avoid Binder transaction limits.
 */
interface INativeHookService {
    /**
     * Get list of all processes that have been injected and have active hooks.
     * Returns a SharedMemory containing serialized ProcessInfo[] data.
     */
    SharedMemory getInjectedProcesses();
    
    /**
     * Get RegisterNatives data for a specific process.
     * 
     * @param processName The target process name (e.g., "com.android.settings")
     * @return SharedMemory containing serialized RegisterNativesInfo[] data,
     *         or null if no data available for this process
     */
    SharedMemory getRegisterNativesData(String processName);
    
    /**
     * Check if a process has active hooks.
     * 
     * @param processName The target process name
     * @return true if hooks are active in this process
     */
    boolean isProcessHooked(String processName);
    
    /**
     * Clear all collected data for a specific process.
     * 
     * @param processName The target process name
     */
    void clearProcessData(String processName);
    
    /**
     * Clear all collected data for all processes.
     */
    void clearAllData();
}
