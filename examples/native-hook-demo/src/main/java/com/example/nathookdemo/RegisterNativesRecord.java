package com.example.nathookdemo;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.List;

/**
 * Represents a single RegisterNatives call intercepted in a target process.
 */
public final class RegisterNativesRecord implements Parcelable {
    public final long timestamp;
    public final String className;
    public final int methodCount;
    public final List<JniMethodInfo> methods;
    public final int result;
    public transient boolean expanded = false;  // UI state, not serialized
    
    public RegisterNativesRecord(long timestamp, String className, 
                                  List<JniMethodInfo> methods, int result) {
        this.timestamp = timestamp;
        this.className = className;
        this.methodCount = methods != null ? methods.size() : 0;
        this.methods = methods != null ? new ArrayList<>(methods) : new ArrayList<>();
        this.result = result;
    }
    
    protected RegisterNativesRecord(Parcel in) {
        timestamp = in.readLong();
        className = in.readString();
        methodCount = in.readInt();
        methods = new ArrayList<>();
        in.readTypedList(methods, JniMethodInfo.CREATOR);
        result = in.readInt();
    }
    
    public static final Creator<RegisterNativesRecord> CREATOR = new Creator<RegisterNativesRecord>() {
        @Override
        public RegisterNativesRecord createFromParcel(Parcel in) {
            return new RegisterNativesRecord(in);
        }
        
        @Override
        public RegisterNativesRecord[] newArray(int size) {
            return new RegisterNativesRecord[size];
        }
    };
    
    @Override
    public int describeContents() {
        return 0;
    }
    
    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeLong(timestamp);
        dest.writeString(className);
        dest.writeInt(methodCount);
        dest.writeTypedList(methods);
        dest.writeInt(result);
    }
    
    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("[%s] %s (%d methods)\n", 
                className, result == 0 ? "OK" : "FAILED", methodCount));
        for (JniMethodInfo method : methods) {
            sb.append("  ").append(method).append("\n");
        }
        return sb.toString();
    }
}
