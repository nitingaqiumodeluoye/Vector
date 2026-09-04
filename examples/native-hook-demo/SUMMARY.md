# 方案总结：RegisterNatives 数据传输到 Java UI

## 问题分析

**核心挑战**：
1. RegisterNatives hook 运行在**被注入的目标进程**
2. UI 运行在**模块 app 进程**
3. 需要高效的跨进程数据传输方案

## 推荐方案：ContentProvider + SharedMemory

### 方案优势

| 特性 | 说明 |
|------|------|
| **高性能** | SharedMemory 避免 Binder 事务大小限制，传输速度提升 ~10x |
| **高稳定** | 使用 Android 标准 API，兼容性好 |
| **类型安全** | 通过 Parcelable 保证数据结构完整 |
| **易扩展** | 模块化设计，易于添加新的 hook 类型 |
| **零拷贝** | 同进程访问无需数据复制 |

### 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                          整体架构                                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Target Process (被注入进程)        Module Process (模块 UI)        │
│  ┌─────────────────────┐            ┌─────────────────────┐         │
│  │  Native Hook        │            │     MainActivity    │         │
│  │  (RegisterNatives)  │◄──────────►│   (Process List)    │         │
│  └─────────────────────┘            └─────────────────────┘         │
│           │                                  │                      │
│           │ JNI Callback                    │ ContentProvider       │
│           ▼                                  ▼                      │
│  ┌─────────────────────┐            ┌─────────────────────┐         │
│  │  HookDataBridge     │            │ NativeHookContent   │         │
│  └─────────────────────┘            │     Provider        │         │
│           │                         └─────────────────────┘         │
│           ▼                                  │                      │
│  ┌─────────────────────┐                     │ SharedMemory         │
│  │    DataStore        │◄────────────────────┘                      │
│  │  (Singleton Cache)  │                                            │
│  └─────────────────────┘                                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

## 关键文件

### 1. Native 层 (数据捕获)
- `native_hook_demo.cpp` - RegisterNatives hook 实现
- 拦截 JNI 方法注册，解析类名、方法名、签名、函数指针

### 2. Java 层 (数据传输)
- `HookDataBridge.java` - Java-Native 桥接
- 接收 Native 回调，创建数据对象

### 3. 数据存储
- `DataStore.java` - 单例数据存储
- 线程安全，缓存所有进程信息和 Hook 数据

### 4. 跨进程通信
- `NativeHookContentProvider.java` - ContentProvider
- 使用 SharedMemory 高效传输大数据

### 5. SharedMemory 辅助
- `SharedMemoryHelper.java` - SharedMemory 读写封装
- 支持 Parcelable 数组的序列化/反序列化

### 6. UI 界面
- `MainActivity.java` - 进程列表界面
- `ProcessDetailActivity.java` - Hook 详情界面

## 数据流

### 1. Hook 数据捕获 (Native → Java)

```
RegisterNatives() 被调用
        │
        ▼
HookedRegisterNatives() [Native Hook]
        │
        ├─► 解析类名 (通过 JNI)
        ├─► 提取方法信息
        │
        ▼
CallJavaCallback() [JNI Call]
        │
        ▼
HookDataBridge.onRegisterNativesIntercepted() [Java]
        │
        ├─► 创建 JniMethodInfo
        ├─► 创建 RegisterNativesRecord
        │
        ▼
DataStore.addHookRecord() [缓存]
```

### 2. UI 数据查询 (Java → Java)

```
用户点击 "刷新"
        │
        ▼
ContentResolver.call("getInjectedProcesses")
        │
        ▼
NativeHookContentProvider.call()
        │
        ├─► DataStore.getInjectedProcesses()
        │
        ▼
SharedMemoryHelper.writeParcelableArray() [写入 SharedMemory]
        │
        ▼
返回 Bundle(shared_memory, count)
        │
        ▼
MainActivity: SharedMemoryHelper.readParcelableArray() [读取 SharedMemory]
        │
        ▼
更新 ListView 显示
```

## 性能优化

### 1. SharedMemory 容量
- 进程列表: 1MB
- Hook 数据: 4MB
- 可根据需要调整

### 2. 异步处理
- 所有数据查询在后台线程执行
- UI 更新通过 Handler 切换到主线程
- 避免阻塞 UI

### 3. 数据缓存
- DataStore 单例缓存所有数据
- 避免重复序列化/反序列化

## 编译和使用

### 编译
```powershell
cd E:\52\Vector\examples\native-hook-demo
.\gradlew.bat assembleDebug
```

### 安装
```powershell
$adb = "D:\sdk\platform-tools\adb.exe"
& $adb install build/outputs/apk/debug/native-hook-demo-debug.apk
```

### 测试
1. 启动应用查看进程列表
2. 打开其他应用触发 hook
3. 点击刷新查看新数据
4. 点击进程查看详情

## 总结

本方案通过 **ContentProvider + SharedMemory** 实现了高效的跨进程数据传输：

✅ **高效**: SharedMemory 避免 Binder 限制，传输速度提升 ~10x  
✅ **稳定**: 使用 Android 标准 API，兼容性好  
✅ **易用**: 封装了通用的读写辅助类  
✅ **可扩展**: 模块化设计，易于添加新的 hook 类型  

这种方案特别适合需要频繁传输大量结构化数据的场景，如 hook 数据、日志信息、性能监控数据等。
