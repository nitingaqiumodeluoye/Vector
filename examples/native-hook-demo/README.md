# Native Hook Demo - 构建和测试指南

## 构建项目

### 1. 编译模块

```powershell
# 设置 Android SDK 路径
$ErrorActionPreference = 'Stop'
$env:ANDROID_HOME = 'D:\sdk'
$env:ANDROID_SDK_ROOT = 'D:\sdk'

# 进入项目目录
cd E:\52\Vector\examples\native-hook-demo

# 编译 Debug 版本
.\gradlew.bat assembleDebug
```

### 2. 检查构建产物

构建成功后，APK 文件位于：
```
build/outputs/apk/debug/native-hook-demo-debug.apk
```

## 安装和测试

### 1. 安装 APK

```powershell
# 使用 ADB 安装
$adb = "$env:ANDROID_HOME\platform-tools\adb.exe"
& $adb install build/outputs/apk/debug/native-hook-demo-debug.apk
```

### 2. 测试步骤

1. **启动应用**
   ```powershell
   & $adb shell am start -n com.example.nathookdemo/.MainActivity
   ```

2. **查看进程列表**
   - 应用启动后，首页会显示所有被注入的进程
   - 每个进程显示：进程名、PID、Hook 数量、最后更新时间

3. **刷新数据**
   - 点击"刷新"按钮获取最新数据
   - 新注入的进程会自动显示

4. **查看 Hook 详情**
   - 点击任意进程，进入详情页面
   - 显示该进程的所有 RegisterNatives 调用记录
   - 点击展开查看每个方法的详细信息

5. **清除数据**
   - 点击"清除所有数据"清空所有记录
   - 或在详情页点击"清除此进程数据"

### 3. 查看日志

```powershell
# 查看模块日志
& $adb logcat -s VectorNativeHookDemo

# 查看 ContentProvider 日志
& $adb logcat -s NativeHookCP

# 查看所有相关日志
& $adb logcat | findstr "VectorNativeHookDemo\|NativeHookCP\|DataStore"
```

## 数据传输机制

### 核心组件

1. **Native Hook** (`native_hook_demo.cpp`)
   - 拦截 RegisterNatives 调用
   - 解析类名、方法名、签名、函数指针
   - 通过 JNI 回调 Java 层

2. **Java 桥接** (`HookDataBridge.java`)
   - 接收 Native 层的回调数据
   - 创建数据对象并存储到 DataStore

3. **数据存储** (`DataStore.java`)
   - 单例模式，线程安全
   - 缓存所有进程信息和 Hook 数据

4. **ContentProvider** (`NativeHookContentProvider.java`)
   - 提供跨进程数据查询接口
   - 使用 SharedMemory 高效传输大数据

5. **SharedMemory 辅助类** (`SharedMemoryHelper.java`)
   - 封装 SharedMemory 读写操作
   - 支持 Parcelable 数组的序列化/反序列化

### 数据流

```
RegisterNatives() 被调用
        │
        ▼
HookedRegisterNatives() [Native Hook]
        │
        ▼
CallJavaCallback() [JNI]
        │
        ▼
HookDataBridge.onRegisterNativesIntercepted() [Java]
        │
        ▼
DataStore.addHookRecord() [缓存]
        │
        ▼
UI 查询时通过 ContentProvider + SharedMemory 读取
```

## 性能优化

### 1. SharedMemory 优势

- **高带宽**: 避免 Binder 事务大小限制
- **零拷贝**: 同进程访问无需数据复制
- **低延迟**: 直接内存访问

### 2. 容量管理

- 进程列表: 1MB SharedMemory
- Hook 数据: 4MB SharedMemory
- 可根据需要调整 `SharedMemoryHelper.java` 中的常量

### 3. 异步处理

- 所有数据查询在后台线程执行
- UI 更新通过 Handler 切换到主线程
- 避免阻塞 UI 线程

## 故障排查

### 1. 没有数据显示

检查日志：
```powershell
& $adb logcat -s VectorNativeHookDemo | findstr "hooked\|intercepted"
```

确保：
- 模块已正确安装
- 目标进程已注入
- RegisterNatives hook 已安装

### 2. 数据不完整

检查 SharedMemory 读写：
```powershell
& $adb logcat -s SharedMemoryHelper
```

确保：
- 数据未超过 SharedMemory 容量
- Parcelable 序列化/反序列化正确

### 3. 应用崩溃

检查异常日志：
```powershell
& $adb logcat -s AndroidRuntime
```

常见原因：
- ContentProvider 权限问题
- SharedMemory 内存不足
- 线程同步问题

## 扩展开发

### 添加新的 Hook 类型

1. 在 `native_hook_demo.cpp` 中添加 hook 函数
2. 在 `HookDataBridge.java` 中添加 Java 回调
3. 创建新的数据结构（实现 Parcelable）
4. 更新 `DataStore` 和 UI

### 修改 UI

所有 UI 都在 Java 代码中创建（无 XML 布局），修改对应 Activity 文件即可。

### 调整性能参数

修改 `SharedMemoryHelper.java` 中的常量：
```java
private static final int SHARED_MEMORY_CAPACITY = 4 * 1024 * 1024; // 4MB
```

## 版本历史

- **v1.6.0** (当前版本)
  - 添加 ContentProvider + SharedMemory 数据传输
  - 新增进程列表界面
  - 新增 Hook 详情界面
  - 优化性能和内存使用

- **v1.5.5**
  - 初始版本
  - 基本的 RegisterNatives hook
  - 简单的 UI 控制
