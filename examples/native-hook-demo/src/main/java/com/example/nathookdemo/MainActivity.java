package com.example.nathookdemo;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcelable;
import android.os.SharedMemory;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * Main activity showing list of injected processes.
 * 
 * <p>Features:
 * <ul>
 *   <li>Displays all processes that have been injected with RegisterNatives hook</li>
 *   <li>Uses SharedMemory for efficient bulk data transfer</li>
 *   <li>Click to view detailed RegisterNatives data</li>
 *   <li>Refresh button to update the list</li>
 *   <li>Clear all data button</li>
 * </ul>
 */
public final class MainActivity extends Activity {
    private static final String TAG = "MainActivity";
    
    private ListView processListView;
    private ProcessListAdapter adapter;
    private TextView statusText;
    private Button refreshButton;
    private Button clearButton;
    
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<ProcessInfo> processList = new ArrayList<>();
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        setupUI();
        refreshProcessList();
    }
    
    @Override
    protected void onResume() {
        super.onResume();
        refreshProcessList();
    }
    
    private void setupUI() {
        ViewGroup root = createLinearLayout(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));
        
        // Title
        TextView title = new TextView(this);
        title.setText("RegisterNatives Hook Demo");
        title.setTextSize(20);
        title.setPadding(0, 0, 0, dp(16));
        root.addView(title);
        
        // Status
        statusText = new TextView(this);
        statusText.setText("Loading...");
        statusText.setTextSize(14);
        statusText.setPadding(0, 0, 0, dp(8));
        root.addView(statusText);
        
        // Buttons
        ViewGroup buttonContainer = createLinearLayout(LinearLayout.HORIZONTAL);
        buttonContainer.setPadding(0, 0, 0, dp(16));
        
        refreshButton = new Button(this);
        refreshButton.setText("刷新");
        refreshButton.setOnClickListener(v -> refreshProcessList());
        buttonContainer.addView(refreshButton, createLayoutParams(0, 1));
        
        clearButton = new Button(this);
        clearButton.setText("清除所有数据");
        clearButton.setOnClickListener(v -> clearAllData());
        buttonContainer.addView(clearButton, createLayoutParams(dp(8), 1));
        
        root.addView(buttonContainer);
        
        // Process list
        processListView = new ListView(this);
        adapter = new ProcessListAdapter();
        processListView.setAdapter(adapter);
        processListView.setOnItemClickListener((parent, view, position, id) -> {
            ProcessInfo info = processList.get(position);
            if (info != null) {
                openProcessDetail(info.processName);
            }
        });
        root.addView(processListView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        
        setContentView(root);
    }
    
    private void refreshProcessList() {
        statusText.setText("正在加载...");
        
        new Thread(() -> {
            try {
                Bundle result = callContentProvider("getInjectedProcesses", null, null);
                if (result == null) {
                    handler.post(() -> {
                        statusText.setText("获取数据失败");
                        Toast.makeText(this, "刷新失败", Toast.LENGTH_SHORT).show();
                    });
                    return;
                }
                
                // Read Parcelable array directly (same-process shortcut)
                result.setClassLoader(getClassLoader());
                Parcelable[] items = result.getParcelableArray("processes");
                
                if (items == null || items.length == 0) {
                    handler.post(() -> {
                        processList.clear();
                        adapter.notifyDataSetChanged();
                        statusText.setText("没有注入的进程\n\n提示：请先打开其他应用触发 hook");
                    });
                    return;
                }
                
                List<ProcessInfo> newProcessList = new ArrayList<>();
                for (Parcelable item : items) {
                    if (item instanceof ProcessInfo) {
                        newProcessList.add((ProcessInfo) item);
                    }
                }
                
                // Update UI
                handler.post(() -> {
                    processList.clear();
                    processList.addAll(newProcessList);
                    adapter.notifyDataSetChanged();
                    
                    long totalHooks = 0;
                    for (ProcessInfo info : processList) {
                        totalHooks += info.hookCount;
                    }
                    
                    statusText.setText(String.format(
                            "已注入 %d 个进程，共捕获 %d 次 RegisterNatives 调用",
                            processList.size(), totalHooks));
                });
                
            } catch (Exception e) {
                Log.e(TAG, "Failed to refresh process list", e);
                handler.post(() -> {
                    statusText.setText("加载失败: " + e.getMessage());
                    Toast.makeText(this, "刷新失败", Toast.LENGTH_SHORT).show();
                });
            }
        }).start();
    }
    
    private void clearAllData() {
        new Thread(() -> {
            try {
                Bundle result = callContentProvider("clearAllData", null, null);
                handler.post(() -> {
                    if (result != null && result.getBoolean("success")) {
                        Toast.makeText(this, "数据已清除", Toast.LENGTH_SHORT).show();
                        refreshProcessList();
                    } else {
                        Toast.makeText(this, "清除失败", Toast.LENGTH_SHORT).show();
                    }
                });
            } catch (Exception e) {
                handler.post(() -> {
                    Toast.makeText(this, "清除失败: " + e.getMessage(), 
                            Toast.LENGTH_SHORT).show();
                });
            }
        }).start();
    }
    
    private void openProcessDetail(String processName) {
        Intent intent = new Intent(this, ProcessDetailActivity.class);
        intent.putExtra(ProcessDetailActivity.EXTRA_PROCESS_NAME, processName);
        startActivity(intent);
    }
    
    private Bundle callContentProvider(String method, String arg, Bundle extras) {
        android.net.Uri uri = android.net.Uri.parse("content://com.example.nathookdemo.provider");
        return getContentResolver().call(uri, method, arg, extras);
    }
    
    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density);
    }
    
    private android.widget.LinearLayout createLinearLayout(int orientation) {
        android.widget.LinearLayout layout = new android.widget.LinearLayout(this);
        layout.setOrientation(orientation);
        return layout;
    }
    
    private android.widget.LinearLayout.LayoutParams createLayoutParams(int leftMargin, int weight) {
        android.widget.LinearLayout.LayoutParams params = 
                new android.widget.LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, weight);
        params.leftMargin = leftMargin;
        return params;
    }
    
    private class ProcessListAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return processList.size();
        }
        
        @Override
        public ProcessInfo getItem(int position) {
            return processList.get(position);
        }
        
        @Override
        public long getItemId(int position) {
            return position;
        }
        
        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            ViewHolder holder;
            
            if (convertView == null) {
                convertView = LayoutInflater.from(MainActivity.this)
                        .inflate(android.R.layout.simple_list_item_2, parent, false);
                holder = new ViewHolder();
                holder.text1 = convertView.findViewById(android.R.id.text1);
                holder.text2 = convertView.findViewById(android.R.id.text2);
                convertView.setTag(holder);
            } else {
                holder = (ViewHolder) convertView.getTag();
            }
            
            ProcessInfo info = getItem(position);
            if (info != null) {
                holder.text1.setText(info.processName);
                holder.text2.setText(String.format(
                        "PID: %d | Hooks: %d | Last: %tT",
                        info.pid, info.hookCount, info.lastHookTime));
            }
            
            return convertView;
        }
        
        private class ViewHolder {
            TextView text1;
            TextView text2;
        }
    }
}
