package com.example.nathookdemo;

import android.os.Parcel;
import android.os.Parcelable;
import android.os.SharedMemory;
import android.util.Log;

import java.io.Closeable;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * Helper class for efficient SharedMemory-based data transfer.
 * 
 * <p>This class provides type-safe methods to read/write Parcelable arrays
 * through SharedMemory, avoiding Binder transaction size limits.</p>
 * 
 * <p>Memory layout in SharedMemory:
 * <pre>
 *   [4 bytes] Total data length (int)
 *   [N bytes] Parcel-marshalled data
 * </pre>
 */
public final class SharedMemoryHelper implements Closeable {
    private static final String TAG = "SharedMemoryHelper";
    
    private final SharedMemory mSharedMemory;
    private final int mCapacity;
    private boolean mIsOwner;
    
    /**
     * Create a new SharedMemoryHelper with specified capacity.
     * 
     * @param capacity Capacity in bytes
     * @throws IOException If SharedMemory creation fails
     */
    public SharedMemoryHelper(int capacity) throws IOException {
        mCapacity = capacity;
        try {
            mSharedMemory = SharedMemory.create(null, capacity);
        } catch (Exception e) {
            throw new IOException("Failed to create SharedMemory", e);
        }
        mIsOwner = true;
    }
    
    /**
     * Wrap an existing SharedMemory for reading.
     * 
     * @param sharedMemory The SharedMemory to wrap
     */
    public SharedMemoryHelper(SharedMemory sharedMemory) {
        mSharedMemory = sharedMemory;
        mCapacity = 0;  // Unknown capacity; size validated by SharedMemory itself
        mIsOwner = false;
    }
    
    /**
     * Write a Parcelable array to SharedMemory.
     * 
     * @param items Array of Parcelable items
     * @return Number of bytes written
     * @throws IOException If write fails
     */
    public int writeParcelableArray(Parcelable[] items) throws IOException {
        if (!mIsOwner) {
            throw new IOException("Cannot write to non-owned SharedMemory");
        }
        
        Parcel parcel = Parcel.obtain();
        try {
            // Write array length
            parcel.writeInt(items.length);
            
            // Write each item
            for (Parcelable item : items) {
                parcel.writeParcelable(item, 0);
            }
            
            // Get marshalled bytes
            byte[] data = parcel.marshall();
            
            if (data.length + 4 > mCapacity) {
                throw new IOException("Data too large: " + data.length + " bytes (capacity: " + mCapacity + ")");
            }
            
            // Write to SharedMemory
            ByteBuffer buffer;
            try {
                buffer = mSharedMemory.mapReadWrite();
            } catch (Exception e) {
                throw new IOException("Failed to map SharedMemory for writing", e);
            }
            try {
                buffer.clear();
                buffer.putInt(data.length);
                buffer.put(data);
                buffer.flip();
            } finally {
                SharedMemory.unmap(buffer);
            }
            
            Log.d(TAG, "Wrote " + data.length + " bytes for " + items.length + " items");
            return data.length;
        } finally {
            parcel.recycle();
        }
    }
    
    /**
     * Read a Parcelable array from SharedMemory.
     * 
     * @param creator Parcelable.Creator for the target type
     * @return Array of deserialized items
     * @throws IOException If read fails
     */
    @SuppressWarnings("unchecked")
    public <T extends Parcelable> T[] readParcelableArray(Parcelable.Creator<T> creator) throws IOException {
        ByteBuffer buffer;
        try {
            buffer = mSharedMemory.mapReadOnly();
        } catch (Exception e) {
            throw new IOException("Failed to map SharedMemory for reading", e);
        }
        try {
            // Read data length
            int length = buffer.getInt();
            if (length <= 0) {
                throw new IOException("Invalid data length: " + length);
            }
            // Validate against buffer remaining when we don't know the original capacity
            if (mCapacity > 0 && length + 4 > mCapacity) {
                throw new IOException("Data too large: " + length + " (capacity: " + mCapacity + ")");
            }
            
            // Read marshalled bytes
            byte[] data = new byte[length];
            buffer.get(data);
            
            // Unmarshall
            Parcel parcel = Parcel.obtain();
            try {
                parcel.unmarshall(data, 0, data.length);
                parcel.setDataPosition(0);
                
                // Read array length
                int count = parcel.readInt();
                if (count < 0 || count > 10000) {
                    throw new IOException("Invalid array count: " + count);
                }
                
                // Create array
                @SuppressWarnings("rawtypes")
                Class<? extends Parcelable> clazz = (Class<? extends Parcelable>) 
                        creator.getClass().getDeclaringClass();
                @SuppressWarnings("unchecked")
                T[] result = (T[]) java.lang.reflect.Array.newInstance(clazz, count);
                
                // Read items
                for (int i = 0; i < count; i++) {
                    result[i] = parcel.readParcelable(getClass().getClassLoader());
                }
                
                Log.d(TAG, "Read " + count + " items (" + length + " bytes)");
                return result;
            } finally {
                parcel.recycle();
            }
        } finally {
            SharedMemory.unmap(buffer);
        }
    }
    
    /**
     * Write a single Parcelable to SharedMemory.
     * 
     * @param parcelable The item to write
     * @return Number of bytes written
     * @throws IOException If write fails
     */
    public int writeParcelable(Parcelable parcelable) throws IOException {
        return writeParcelableArray(new Parcelable[]{parcelable});
    }
    
    /**
     * Read a single Parcelable from SharedMemory.
     * 
     * @param creator Parcelable.Creator for the target type
     * @return The deserialized item
     * @throws IOException If read fails
     */
    public <T extends Parcelable> T readParcelable(Parcelable.Creator<T> creator) throws IOException {
        T[] items = readParcelableArray(creator);
        if (items == null || items.length == 0) {
            return null;
        }
        return items[0];
    }
    
    /**
     * Get the underlying SharedMemory.
     */
    public SharedMemory getSharedMemory() {
        return mSharedMemory;
    }
    
    /**
     * Get the capacity in bytes.
     */
    public int getCapacity() {
        return mCapacity;
    }
    
    @Override
    public void close() {
        // SharedMemory is managed by the caller
    }
}
