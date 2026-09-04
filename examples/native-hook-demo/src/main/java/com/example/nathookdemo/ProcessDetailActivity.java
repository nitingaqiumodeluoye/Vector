package com.example.nathookdemo;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcelable;
import android.os.SharedMemory;
import android.util.Log;
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
 * Activity to display RegisterNatives hook data for a specific process.
 * 
 * <p>Features:
 * <ul>
 *   <li>Shows all RegisterNatives calls intercepted in the target process</li>
 *   <li>Uses SharedMemory for efficient bulk data transfer</li>
 *   <li>Click on an item to expand and see method details</li>
 *   <li>Clear button to remove data for this process</li>
 * </ul>
 */
public final class ProcessDetailActivity extends Activity {
    public static final String EXTRA_PROCESS_NAME = "process_name";
    
    private static final String TAG = "ProcessDetailActivity";
    
    private String processName;
    private ListView recordsListView;
    private RecordsListAdapter adapter;
    private TextView headerText;
    private Button clearButton;
    
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<RegisterNativesRecord> recordsList = new ArrayList<>();
    
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        
        processName = getIntent().getStringExtra(EXTRA_PROCESS_NAME);
        if (processName == null) {
            Toast.makeText(this, "无效的进程名", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        
        setupUI();
        refreshRecords();
    }
    
    private void setupUI() {
        ViewGroup root = createLinearLayout(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));
        
        // Back button
        Button backButton = new Button(this);
        backButton.setText("← 返回");
        backButton.setOnClickListener(v -> finish());
        root.addView(backButton);
        
        // Header
        headerText = new TextView(this);
        headerText.setText("加载中...");
        headerText.setTextSize(16);
        headerText.setPadding(0, dp(16), 0, dp(8));
        root.addView(headerText);
        
        // Process name
        TextView processNameText = new TextView(this);
        processNameText.setText("进程: " + processName);
        processNameText.setTextSize(14);
        processNameText.setPadding(0, 0, 0, dp(16));
        root.addView(processNameText);
        
        // Clear button
        clearButton = new Button(this);
        clearButton.setText("清除此进程数据");
        clearButton.setOnClickListener(v -> clearProcessData());
        root.addView(clearButton);
        
        // Records list
        recordsListView = new ListView(this);
        adapter = new RecordsListAdapter();
        recordsListView.setAdapter(adapter);
        recordsListView.setOnItemClickListener((parent, view, position, id) -> {
            RegisterNativesRecord record = recordsList.get(position);
            if (record != null) {
                record.expanded = !record.expanded;
                adapter.notifyDataSetChanged();
            }
        });
        root.addView(recordsListView, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        
        setContentView(root);
    }
    
    private void refreshRecords() {
        headerText.setText("正在加载...");
        
        new Thread(() -> {
            try {
                Bundle result = callContentProvider("getRegisterNativesData", processName, null);
                if (result == null) {
                    handler.post(() -> {
                        headerText.setText("获取数据失败");
                        Toast.makeText(this, "刷新失败", Toast.LENGTH_SHORT).show();
                    });
                    return;
                }
                
                // Read Parcelable array directly (same-process shortcut)
                result.setClassLoader(getClassLoader());
                Parcelable[] items = result.getParcelableArray("records");
                
                List<RegisterNativesRecord> newRecords = new ArrayList<>();
                if (items != null) {
                    for (Parcelable item : items) {
                        if (item instanceof RegisterNativesRecord) {
                            newRecords.add((RegisterNativesRecord) item);
                        }
                    }
                }
                
                // Update UI
                handler.post(() -> {
                    recordsList.clear();
                    recordsList.addAll(newRecords);
                    adapter.notifyDataSetChanged();
                    
                    headerText.setText(String.format(
                            "共捕获 %d 次 RegisterNatives 调用", recordsList.size()));
                });
                
            } catch (Exception e) {
                Log.e(TAG, "Failed to refresh records", e);
                handler.post(() -> {
                    headerText.setText("加载失败: " + e.getMessage());
                    Toast.makeText(this, "刷新失败", Toast.LENGTH_SHORT).show();
                });
            }
        }).start();
    }
    
    private void clearProcessData() {
        new Thread(() -> {
            try {
                Bundle result = callContentProvider("clearProcessData", processName, null);
                handler.post(() -> {
                    if (result != null && result.getBoolean("success")) {
                        Toast.makeText(this, "数据已清除", Toast.LENGTH_SHORT).show();
                        recordsList.clear();
                        adapter.notifyDataSetChanged();
                        headerText.setText("共捕获 0 次 RegisterNatives 调用");
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
    
    private class RecordsListAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return recordsList.size();
        }
        
        @Override
        public RegisterNativesRecord getItem(int position) {
            return recordsList.get(position);
        }
        
        @Override
        public long getItemId(int position) {
            return position;
        }
        
        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            ViewHolder holder;
            
            if (convertView == null) {
                convertView = createRecordView();
                holder = new ViewHolder();
                holder.classNameText = convertView.findViewById(1001);
                holder.infoText = convertView.findViewById(1002);
                holder.detailsContainer = convertView.findViewById(1003);
                holder.detailsText = convertView.findViewById(1004);
                convertView.setTag(holder);
            } else {
                holder = (ViewHolder) convertView.getTag();
            }
            
            RegisterNativesRecord record = getItem(position);
            if (record != null) {
                holder.classNameText.setText(record.className);
                holder.infoText.setText(String.format(
                        "方法数: %d | 结果: %s | 时间: %tT",
                        record.methodCount,
                        record.result == 0 ? "成功" : "失败(" + record.result + ")",
                        record.timestamp));
                
                if (record.expanded) {
                    holder.detailsContainer.setVisibility(View.VISIBLE);
                    StringBuilder sb = new StringBuilder();
                    for (JniMethodInfo method : record.methods) {
                        sb.append(String.format("  • %s %s @ 0x%x\n",
                                method.methodName, method.signature, method.functionPointer));
                    }
                    holder.detailsText.setText(sb.toString().trim());
                } else {
                    holder.detailsContainer.setVisibility(View.GONE);
                }
            }
            
            return convertView;
        }
        
        private View createRecordView() {
            ViewGroup container = createLinearLayout(LinearLayout.VERTICAL);
            container.setPadding(dp(8), dp(8), dp(8), dp(8));
            
            TextView classNameText = new TextView(ProcessDetailActivity.this);
            classNameText.setId(1001);
            classNameText.setTextSize(16);
            classNameText.setPadding(0, 0, 0, dp(4));
            container.addView(classNameText);
            
            TextView infoText = new TextView(ProcessDetailActivity.this);
            infoText.setId(1002);
            infoText.setTextSize(12);
            infoText.setPadding(0, 0, 0, dp(4));
            container.addView(infoText);
            
            ViewGroup detailsContainer = createLinearLayout(LinearLayout.VERTICAL);
            detailsContainer.setId(1003);
            detailsContainer.setVisibility(View.GONE);
            detailsContainer.setPadding(dp(16), 0, 0, 0);
            
            TextView detailsText = new TextView(ProcessDetailActivity.this);
            detailsText.setId(1004);
            detailsText.setTextSize(12);
            detailsContainer.addView(detailsText);
            
            container.addView(detailsContainer);
            
            return container;
        }
        
        private class ViewHolder {
            TextView classNameText;
            TextView infoText;
            View detailsContainer;
            TextView detailsText;
        }
    }
}
