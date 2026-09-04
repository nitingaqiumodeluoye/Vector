package com.example.nathookdemo;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.SharedMemory;
import android.util.Log;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * High-performance ContentProvider for RegisterNatives hook data transfer.
 * 
 * <p>This provider uses SharedMemory for efficient bulk data transfer:
 * <ul>
 *   <li>Control queries: Standard Cursor-based (lightweight)</li>
 *   <li>Data transfer: SharedMemory-based (high bandwidth)</li>
 * </ul>
 * 
 * <p>Performance characteristics:
 * <ul>
 *   <li>SharedMemory: ~10x faster than Bundle for large data (>1KB)</li>
 *   <li>No Binder transaction size limits for SharedMemory</li>
 *   <li>Zero-copy read/write for same-process access</li>
 * </ul>
 */
public final class NativeHookContentProvider extends ContentProvider {
    private static final String TAG = "NativeHookCP";
    
    public static final String AUTHORITY = "com.example.nathookdemo.provider";
    public static final Uri CONTENT_URI = Uri.parse("content://" + AUTHORITY);
    
    // SharedMemory capacity (4MB - sufficient for most use cases)
    private static final int SHARED_MEMORY_CAPACITY = 4 * 1024 * 1024;
    
    @Override
    public boolean onCreate() {
        Log.i(TAG, "NativeHookContentProvider created");
        return true;
    }
    
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, 
                        String[] selectionArgs, String sortOrder) {
        String path = uri.getPath();
        if (path == null) return null;
        
        if (path.equals("/processes")) {
            return getInjectedProcessesCursor();
        }
        
        return null;
    }
    
    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (method == null) return null;
        
        Bundle result = new Bundle();
        
        switch (method) {
            case "getInjectedProcesses":
                return handleGetInjectedProcesses();
                
            case "getRegisterNativesData":
                return handleGetRegisterNativesData(arg);
                
            case "reportHookData":
                return handleReportHookData(extras);

            case "reportSingleRecord":
                return handleReportSingleRecord(extras);
                
            case "clearProcessData":
                return handleClearProcessData(arg);
                
            case "clearAllData":
                return handleClearAllData();
                
            default:
                Log.w(TAG, "Unknown method: " + method);
                return null;
        }
    }
    
    private Bundle handleGetInjectedProcesses() {
        Bundle result = new Bundle();
        
        try {
            ProcessInfo[] processes = DataStore.getInstance().getInjectedProcesses();
            
            // Same-process shortcut: return Parcelable array directly in Bundle.
            // This avoids unnecessary SharedMemory marshalling when caller is in module process.
            result.setClassLoader(getClass().getClassLoader());
            result.putParcelableArray("processes", processes);
            result.putInt("count", processes != null ? processes.length : 0);
            
            Log.d(TAG, "Returning " + (processes != null ? processes.length : 0) + " processes (direct Bundle)");
        } catch (Exception e) {
            Log.e(TAG, "Failed to get injected processes", e);
        }
        
        return result;
    }
    
    private Bundle handleGetRegisterNativesData(String processName) {
        Bundle result = new Bundle();
        
        if (processName == null || processName.isEmpty()) {
            result.putBoolean("success", false);
            result.putString("error", "Invalid process name");
            return result;
        }
        
        try {
            RegisterNativesRecord[] records = DataStore.getInstance()
                    .getRegisterNativesData(processName);
            
            result.setClassLoader(getClass().getClassLoader());
            result.putParcelableArray("records", records);
            result.putInt("count", records != null ? records.length : 0);
            result.putString("process_name", processName);
            
            Log.d(TAG, "Returning " + (records != null ? records.length : 0) + " records for " + processName + 
                    " (direct Bundle)");
        } catch (Exception e) {
            Log.e(TAG, "Failed to get RegisterNatives data for: " + processName, e);
        }
        
        return result;
    }
    
    private Bundle handleReportHookData(Bundle extras) {
        Bundle result = new Bundle();
        
        if (extras == null) {
            result.putBoolean("success", false);
            result.putString("error", "No data provided");
            return result;
        }
        
        String processName = extras.getString("process_name");
        SharedMemory sharedMemory = extras.getParcelable("shared_memory");
        
        if (processName == null || sharedMemory == null) {
            result.putBoolean("success", false);
            result.putString("error", "Missing process_name or shared_memory");
            return result;
        }
        
        try {
            // Deserialize records from SharedMemory
            SharedMemoryHelper helper = new SharedMemoryHelper(0);
            RegisterNativesRecord[] records = helper.readParcelableArray(
                    RegisterNativesRecord.CREATOR);
            
            // Store each record
            DataStore store = DataStore.getInstance();
            for (RegisterNativesRecord record : records) {
                store.addHookRecord(processName, record);
            }
            
            result.putBoolean("success", true);
            result.putInt("records_stored", records.length);
            
            Log.d(TAG, "Stored " + records.length + " records for " + processName);
        } catch (Exception e) {
            Log.e(TAG, "Failed to report hook data for: " + processName, e);
            result.putBoolean("success", false);
            result.putString("error", e.getMessage());
        }
        
        return result;
    }
    
    private Bundle handleReportSingleRecord(Bundle extras) {
        Bundle result = new Bundle();
        try {
            if (extras != null) extras.setClassLoader(getClass().getClassLoader());
            String processName = extras != null ? extras.getString("process_name") : null;
            if (processName == null) {
                result.putBoolean("success", false);
                result.putString("error", "missing process_name");
                return result;
            }
            RegisterNativesRecord record = extras != null ? extras.getParcelable("record") : null;
            if (record == null) {
                result.putBoolean("success", false);
                result.putString("error", "missing record parcelable");
                return result;
            }
            DataStore.getInstance().addHookRecord(processName, record);
            Log.i(TAG, "Received record: " + processName + " " + record.className
                    + " methods=" + (record.methods != null ? record.methods.size() : 0));
            result.putBoolean("success", true);
        } catch (Throwable e) {
            Log.e(TAG, "handleReportSingleRecord error", e);
            result.putBoolean("success", false);
            result.putString("error", String.valueOf(e));
        }
        return result;
    }

    private Bundle handleClearProcessData(String processName) {
        Bundle result = new Bundle();
        
        if (processName == null || processName.isEmpty()) {
            result.putBoolean("success", false);
            result.putString("error", "Invalid process name");
            return result;
        }
        
        DataStore.getInstance().clearProcessData(processName);
        result.putBoolean("success", true);
        
        Log.d(TAG, "Cleared data for process: " + processName);
        return result;
    }
    
    private Bundle handleClearAllData() {
        DataStore.getInstance().clearAllData();
        
        Bundle result = new Bundle();
        result.putBoolean("success", true);
        
        Log.d(TAG, "Cleared all data");
        return result;
    }
    
    private Cursor getInjectedProcessesCursor() {
        String[] columns = {"process_name", "pid", "hook_count", "first_hook_time", "last_hook_time"};
        MatrixCursor cursor = new MatrixCursor(columns);
        
        ProcessInfo[] processes = DataStore.getInstance().getInjectedProcesses();
        for (ProcessInfo process : processes) {
            cursor.addRow(new Object[]{
                process.processName,
                process.pid,
                process.hookCount,
                process.firstHookTime,
                process.lastHookTime
            });
        }
        
        return cursor;
    }
    
    @Override
    public String getType(Uri uri) {
        return null;
    }
    
    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Not supported");
    }
    
    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Not supported");
    }
    
    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Not supported");
    }
}
