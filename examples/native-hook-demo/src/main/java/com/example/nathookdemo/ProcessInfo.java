package com.example.nathookdemo;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/**
 * Represents an injected process with its hook status and data.
 */
public final class ProcessInfo implements Parcelable {
    public final String processName;
    public final int pid;
    public final long hookCount;
    public final long firstHookTime;
    public final long lastHookTime;
    
    public ProcessInfo(String processName, int pid, long hookCount, 
                       long firstHookTime, long lastHookTime) {
        this.processName = processName;
        this.pid = pid;
        this.hookCount = hookCount;
        this.firstHookTime = firstHookTime;
        this.lastHookTime = lastHookTime;
    }
    
    protected ProcessInfo(Parcel in) {
        processName = in.readString();
        pid = in.readInt();
        hookCount = in.readLong();
        firstHookTime = in.readLong();
        lastHookTime = in.readLong();
    }
    
    public static final Creator<ProcessInfo> CREATOR = new Creator<ProcessInfo>() {
        @Override
        public ProcessInfo createFromParcel(Parcel in) {
            return new ProcessInfo(in);
        }
        
        @Override
        public ProcessInfo[] newArray(int size) {
            return new ProcessInfo[size];
        }
    };
    
    @Override
    public int describeContents() {
        return 0;
    }
    
    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(processName);
        dest.writeInt(pid);
        dest.writeLong(hookCount);
        dest.writeLong(firstHookTime);
        dest.writeLong(lastHookTime);
    }
    
    @Override
    public String toString() {
        return String.format("%s (PID: %d) - %d hooks", 
                processName, pid, hookCount);
    }
}
