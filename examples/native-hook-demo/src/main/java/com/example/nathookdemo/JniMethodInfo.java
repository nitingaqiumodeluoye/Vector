package com.example.nathookdemo;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * Represents a registered JNI method from RegisterNatives call.
 */
public final class JniMethodInfo implements Parcelable {
    public final String methodName;
    public final String signature;
    public final long functionPointer;
    
    public JniMethodInfo(String methodName, String signature, long functionPointer) {
        this.methodName = methodName;
        this.signature = signature;
        this.functionPointer = functionPointer;
    }
    
    protected JniMethodInfo(Parcel in) {
        methodName = in.readString();
        signature = in.readString();
        functionPointer = in.readLong();
    }
    
    public static final Creator<JniMethodInfo> CREATOR = new Creator<JniMethodInfo>() {
        @Override
        public JniMethodInfo createFromParcel(Parcel in) {
            return new JniMethodInfo(in);
        }
        
        @Override
        public JniMethodInfo[] newArray(int size) {
            return new JniMethodInfo[size];
        }
    };
    
    @Override
    public int describeContents() {
        return 0;
    }
    
    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(methodName);
        dest.writeString(signature);
        dest.writeLong(functionPointer);
    }
    
    @Override
    public String toString() {
        return String.format("%s %s @ 0x%x", methodName, signature, functionPointer);
    }
}
