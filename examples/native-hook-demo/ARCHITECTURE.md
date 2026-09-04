# Native Hook Demo - RegisterNatives 数据传输方案

## 架构概述

本模块实现了高效的跨进程数据传输，用于捕获和展示 RegisterNatives hook 数据。

### 核心组件

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              整体架构                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Target Process (被注入进程)          Module Process (模块 UI)              │
│  ┌─────────────────────┐              ┌─────────────────────┐               │
│  │  native_hook_demo   │              │     MainActivity    │               │
│  │  (RegisterNatives   │   ContentProvider + SharedMemory   │               │
│  │   Hook Callback)    │◄────────────►│   (Process List)    │               │
│  └─────────────────────┘              └─────────────────────┘               │
│           │                                    │                            │
│           │ JNI Callback                      │ Binder Call                 │
│           ▼                                    ▼                            │
│  ┌─────────────────────┐              ┌─────────────────────┐               │
│  │  HookDataBridge     │              │ NativeHookContent   │               │
│  │  (Java-Native Bridge)│             │     Provider        │               │
│  └─────────────────────┘              └─────────────────────┘               │
│           │                                    │                            │
│           │                                    │                            │
│           ▼                                    ▼                            │
│  ┌─────────────────────┐              ┌─────────────────────┐               │
│  │    DataStore        │◄────────────►│    DataStore        │               │
│  │  (Singleton Cache)  │              │  (Shared Instance)  │               │
│  └─────────────────────┘              └─────────────────────┘               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 数据流

### 1. Hook 数据捕获 (Native → Java)

```
RegisterNatives() 被调用
        │
        ▼
HookedRegisterNatives() [Native Hook Callback]
        │
        ├─► 解析类名 (通过 JNI)
        ├─► 提取方法信息 (name, signature, fnPtr)
        │
        ▼
CallJavaCallback() [JNI Call]
        │
        ▼
HookDataBridge.onRegisterNativesIntercepted() [Java Static Method]
        │
        ├─► 创建 JniMethodInfo 对象
        ├─► 创建 RegisterNativesRecord 对象
        │
        ▼
DataStore.addHookRecord() [Singleton Cache]
```

### 2. UI 数据查询 (Java → Java)

```
用户点击 "刷新" 按钮
        │
        ▼
MainActivity.refreshProcessList()
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
SharedMemoryHelper.writeParcelableArray() [SharedMemory 写入]
        │
        ▼
返回 Bundle(shared_memory, count)
        │
        ▼
MainActivity: SharedMemoryHelper.readParcelableArray() [SharedMemory 读取]
        │
        ▼
更新 ListView 显示
```

## 技术方案

### 方案选择: ContentProvider + SharedMemory

| 特性 | 纯 Binder Bundle | ContentProvider + Cursor | **ContentProvider + SharedMemory** |
|------|------------------|--------------------------|-------------------------------------|
| 数据量限制 | ~1MB (Binder) | 无限制但慢 | 无限制且快 |
| 传输效率 | 低 | 中 | **高 (~10x)** |
| 实现复杂度 | 低 | 中 | 中 |
| 类型安全 | 高 | 低 | **高** |
| 跨进程支持 | 需手动 | 需手动 | **自动** |

### 为什么选择 SharedMemory?

1. **性能**: SharedMemory 避免了 Binder 事务大小限制，适合传输大量 hook 数据
2. **零拷贝**: 同进程访问时无需数据复制
3. **类型安全**: 通过 Parcelable 序列化保证数据结构完整
4. **标准 API**: Android 原生支持，稳定可靠

### SharedMemory 内存布局

```
SharedMemory Block:
┌────────────────────────────────────────────────────────┐
│  [4 bytes]  Data Length (int)                          │
├────────────────────────────────────────────────────────┤
│  [N bytes]  Parcel-marshalled Parcelable Array         │
│             ┌────────────────────────────────────────┐ │
│             │  Array Length (int)                     │ │
│             │  Item 1 (Parcelable)                    │ │
│             │  Item 2 (Parcelable)                    │ │
│             │  ...                                   │ │
│             │  Item N (Parcelable)                    │ │
│             └────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────┘
```

## 文件结构

```
src/main/
├── aidl/com/example/nathookdemo/
│   └── INativeHookService.aidl          # AIDL 接口 (备用方案)
│
├── cpp/
│   └── native_hook_demo.cpp             # Native hook 实现
│
├── java/com/example/nathookdemo/
│   ├── MainActivity.java                # 主界面 (进程列表)
│   ├── ProcessDetailActivity.java       # 详情界面 (hook 数据)
│   ├── NativeHookContentProvider.java   # ContentProvider
│   ├── DataStore.java                   # 数据存储单例
│   ├── SharedMemoryHelper.java          # SharedMemory 读写辅助
│   ├── HookDataBridge.java              # Java-Native 桥接
│   ├── NativeHookDemoModule.java        # Vector 模块入口
│   ├── NativeBridge.java                # JNI 桥接类
│   ├── ProcessInfo.java                 # 进程信息 (Parcelable)
│   ├── RegisterNativesRecord.java       # Hook 记录 (Parcelable)
│   └── JniMethodInfo.java              # JNI 方法信息 (Parcelable)
│
└── resources/META-INF/xposed/
    ├── module.prop
    ├── java_init.list
    ├── native_init.list
    └── scope.list
```

## 数据结构

### ProcessInfo

```java
public final class ProcessInfo implements Parcelable {
    public final String processName;      // 进程名
    public final int pid;                 // 进程 ID
    public final long hookCount;          // Hook 总数
    public final long firstHookTime;      // 首次 hook 时间
    public final long lastHookTime;       // 最后 hook 时间
}
```

### RegisterNativesRecord

```java
public final class RegisterNativesRecord implements Parcelable {
    public final long timestamp;          // 时间戳
    public final String className;        // 类名
    public final int methodCount;         // 方法数
    public final List<JniMethodInfo> methods;  // 方法列表
    public final int result;              // 返回值
    public transient boolean expanded;    // UI 展开状态
}
```

### JniMethodInfo

```java
public final class JniMethodInfo implements Parcelable {
    public final String methodName;       // 方法名
    public final String signature;        // 签名
    public final long functionPointer;    // 函数指针
}
```

## 性能优化

### 1. SharedMemory 容量管理

- 进程列表: 1MB (足够数千个进程)
- Hook 数据: 4MB (足够数万条记录)

### 2. 异步加载

- 所有数据查询都在后台线程执行
- UI 更新通过 Handler 切换到主线程

### 3. 数据缓存

- DataStore 单例缓存所有数据
- 避免重复序列化/反序列化

### 4. 按需加载

- 进程列表只加载基本信息
- Hook 数据在用户点击时才加载

## 使用方式

### 1. 查看进程列表

启动应用后，首页显示所有被注入的进程：

```
┌─────────────────────────────────────────┐
│ RegisterNatives Hook Demo               │
│─────────────────────────────────────────│
│ 已注入 3 个进程，共捕获 127 次调用        │
│─────────────────────────────────────────│
│ [刷新]          [清除所有数据]            │
│─────────────────────────────────────────│
│ com.android.settings                   │
│ PID: 1234 | Hooks: 45 | Last: 14:30:22 │
│─────────────────────────────────────────│
│ com.example.nathookdemo                │
│ PID: 5678 | Hooks: 32 | Last: 14:30:18 │
│─────────────────────────────────────────│
│ system_server                          │
│ PID: 901 | Hooks: 50 | Last: 14:30:25  │
└─────────────────────────────────────────┘
```

### 2. 查看 Hook 详情

点击某个进程，显示该进程的所有 RegisterNatives 调用：

```
┌─────────────────────────────────────────┐
│ ← 返回                                  │
│─────────────────────────────────────────│
│ 共捕获 45 次 RegisterNatives 调用        │
│ 进程: com.android.settings              │
│ [清除此进程数据]                        │
│─────────────────────────────────────────│
│ android.view.View                       │
│ 方法数: 3 | 结果: 成功 | 时间: 14:30:22 │
│   ▼ 展开详情                            │
│─────────────────────────────────────────│
│ android.widget.Button                   │
│ 方法数: 2 | 结果: 成功 | 时间: 14:30:18 │
│   ▶ 点击展开                            │
│─────────────────────────────────────────│
│ ...                                     │
└─────────────────────────────────────────┘
```

## 编译和测试

### 编译

```bash
cd examples/native-hook-demo
./gradlew assembleDebug
```

### 安装

```bash
adb install build/outputs/apk/debug/native-hook-demo-debug.apk
```

### 测试

1. 启动应用，查看进程列表
2. 在设备上打开其他应用 (如 Settings)
3. 点击刷新，查看新注入的进程
4. 点击进程，查看 hook 数据
5. 点击展开，查看方法详情

## 扩展性

### 添加新的 hook 类型

1. 在 `native_hook_demo.cpp` 中添加新的 hook 函数
2. 在 `HookDataBridge.java` 中添加对应的 Java 回调
3. 创建新的数据结构 (实现 Parcelable)
4. 在 `DataStore` 中添加数据管理方法
5. 更新 UI 展示

### 支持更多进程

修改 `scope.list` 添加需要注入的目标进程包名：

```
system
com.android.settings
com.android.systemui
com.example.targetapp1
com.example.targetapp2
```

## 注意事项

1. **线程安全**: DataStore 使用 ConcurrentHashMap 和 synchronized 保证线程安全
2. **内存管理**: SharedMemory 在读写完成后需要 unmap
3. **异常处理**: 所有跨进程操作都有异常捕获和日志记录
4. **性能监控**: 通过 logcat 查看 `VectorNativeHookDemo` 和 `NativeHookCP` 标签

## 总结

本方案通过 **ContentProvider + SharedMemory** 实现了高效的跨进程数据传输：

- **高效**: SharedMemory 避免 Binder 限制，传输速度提升 ~10x
- **稳定**: 使用 Android 标准 API，兼容性好
- **易用**: 封装了通用的读写辅助类
- **可扩展**: 模块化设计，易于添加新的 hook 类型

这种方案特别适合需要频繁传输大量结构化数据的场景，如 hook 数据、日志信息、性能监控数据等。
