# Vector Native 模块编写指南

> 适用基线：soLoader 路径（`USE_RAW_VECTOR_LINKER_NATIVE_LOADER = false`），API 102，目标平台
> Android 12 / SDK 31 / arm64-v8a。硬约束摘要在 `AGENTS.md` 的《编写 native 模块的硬约束》
> 一节；本文给出完整原理、可照抄的骨架和排障依据。

本指南中每一条结论都对应仓库内可核对的位置（框架源码 / demo 实测日志）。凭经验写的 hook 代码
在 soLoader 基线下会静默失效，先读本文再动手。

## 一、模块整体形态

```
my-module/
├── src/main/AndroidManifest.xml              # 普通 APK，无需额外声明（除非要用模块服务，见 §8）
├── src/main/cpp/CMakeLists.txt               # 产出 libmy_module.so
├── src/main/cpp/my_module.cpp                # native_init 入口 + hook 逻辑
├── src/main/java/.../MyModule.java           # Java 侧入口，必须 extends WbModule
└── src/main/resources/META-INF/xposed/
    ├── module.prop          # targetApiVersion=102 / minApiVersion=101 / exceptionMode / staticScope
    ├── java_init.list       # 模块入口类全名（必须；为空则 loadModule 判 Unusable）
    ├── native_init.list     # so 文件名（与 APK 内 lib/arm64-v8a/ 下一致）
    └── scope.list           # 仅当 module.prop 含 staticScope=true 才被读取（见 §6）
```

`module.prop` 支持的关键字（`FileSystem.loadModule` / `readStaticScope`）：

| 键 | 作用 |
| --- | --- |
| `minApiVersion` | 作者声明的最低 API（管理器只用于提示不兼容，不下沉到加载判定） |
| `targetApiVersion` | `>= 101` 走 MODERN 策略并使用 `META-INF/xposed/*.list`；`== 100` 直接判 UnsupportedApi；缺失且无 `assets/xposed_init` 则 Unusable |
| `exceptionMode` | `passthrough` 才直通异常，其它一律 protective 默认 |
| `staticScope` | `true` 才让 daemon 读 `scope.list` 并限制模块作用域 |
| `autoHotReload` | 模块热重载相关开关 |

加载策略优先级（`FileSystem.loadModule`）：

```
targetApiVersion >= 101                 -> MODERN : META-INF/xposed/java_init.list + native_init.list
targetApiVersion == 100                 -> 拒绝（API 100 已移除）
无 targetApiVersion 且有 assets/xposed_init -> LEGACY : assets/xposed_init + assets/native_init
其它                                     -> Unusable（无有效 init 文件）
```

## 二、soLoader 装载链（为什么不能 System.loadLibrary）

Vector 不把 APK 路径交给系统 linker。完整链路：

```
daemon 读取 APK 内 lib/arm64-v8a/<lib>.so
  -> 拷进 sealed read-only SharedMemory
  -> Java: NativeAPI.loadNativeLibraries(names, images)
  -> core: ElfLoader::LoadLibraryFd()   （external/soLoader，匿名装载，自带 relocator/TLS）
  -> native_init 被【延迟】：只入队 g_pending_native_inits，不在装载时调用
  -> 模块 ClassLoader 建好后，经载体 dex 的 native trampoline 触发
```

由此产生三条硬性结论：

1. **`System.loadLibrary()` / `System.load()` 必然失败**。`VectorModuleClassLoader.findLibrary`
   在 soLoader 模式下**故意**跳过 `apk!/lib/<abi>/<lib>.so` zip entry（避免 linker 把 base.apk
   映射进目标进程，留下外来代码容器映射痕迹）。所以模块 Java 侧不要写静态 `loadLibrary`：
   ```java
   // ❌ 错误：soLoader 下抛 UnsatisfiedLinkError
   static { System.loadLibrary("my_module"); }
   ```
2. **`JNI_OnLoad` 不会被调用**。soLoader 的 `Relocator::CallConstructors` 只跑 `DT_INIT` /
   `init_array`；仓库内不存在任何调用 `JNI_OnLoad` 的代码。不要依赖它，也不要用它获取
   `JavaVM*`。
3. **模块 Java 声明的 `native` 方法没有绑定来源**。匿名映射不进系统 dl 链，JNI 符号不会被
   自动查找。要么在 `native_init` 里自己 `RegisterNatives` 把它们绑上，要么把这些方法当作
   可选入口并做好 `UnsatisfiedLinkError` 兜底（demo 的做法，见 §7）。

`native_init.list` 的校验（`FileSystem.loadModule`）：文件名必须以 `.so` 结尾、**不得含 `/` 或
`\`**、必须在 `lib/arm64-v8a/` 下存在。Vector soLoader 目前**只支持 arm64-v8a**。

## 三、native_init 是唯一入口

导出符号名固定为 `native_init`，签名与 `native/include/core/native_api.h` 一致：

```cpp
using NativeOnModuleLoaded = void (*)(const char *name, void *handle);
using NativeInit = NativeOnModuleLoaded (*)(const NativeAPIEntries *entries);
```

框架侧取符号的方式是 `loader->GetSymbol("native_init")`；取不到会直接判装载失败
（`Module '<name>' does not export native_init.`）。

`NativeAPIEntries` 当前 `version = 2`，只有两个函数指针：

```cpp
struct NativeAPIEntries {
    uint32_t version;          // 2
    HookFunType hookFunc;      // = HookInlineWxshadow
    UnhookFunType unhookFunc;  // = UninlineWxshadow
};
```

- `hookFunc(target, replacement, &backup)` 返回 0 且 `backup != nullptr` 才算成功；
  `backup` 是可调原函数的 trampoline。
- ABI 语义是**裸替换**（`hook_install`）：`replacement` 收到与目标函数完全相同的寄存器/参数，
  想调原函数就走 `backup`。因此 **replacement 的签名必须与被 hook 函数一致**。
- 必须使用 `entries->hookFunc` / `entries->unhookFunc`，**不得**引入 Dobby、mprotect fallback
  或第二套 inline hook（`AGENTS.md`《模块 native so 加载约束》第 3 条）。

### 标准骨架

```cpp
#include "native_api.h"   // 模块侧只需自己复制结构体定义或包含 Vector 公开头

static NativeAPIEntries g_entries{};
static JavaVM *g_vm = nullptr;

extern "C" NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) return nullptr;
    if (entries->version < 2) { /* 记录告警 */ }
    g_entries = *entries;

    JNIEnv *env = GetJNIEnvForCallback();      // 自举 VM + attach（见 §4）
    InitializeJavaReferences(env);             // FindClass + GetStaticMethodID + NewGlobalRef
    InstallHook();                             // g_entries.hookFunc(target, replacement, &backup)

    // 必须返回非 null，否则框架只记录 "completed without library-load callback"
    return [](const char *name, void *handle) {
        // 后续 dlopen 的 so 通知；只做日志/可选 dlsym
    };
}
```

返回值语义：`nullptr` 表示"这个模块不关心后续库加载"。非 null 的回调被存进
`g_module_loaded_callbacks`，由 `NotifyNativeModuleCallbacks` 在每个后续库加载时调用。它只是
**通知**，不是安装后续 hook 的地方。

## 四、JavaVM / JNIEnv 自举

首次 `native_init` 时 `env->GetJavaVM()` 不可靠（demo 曾因此拿不到 env，形如
`Failed to initialize Java callback references`）。对齐 rustFrida `get_or_init_vm` 的做法是从
libart 的 `.dynsym` 解析 `JNI_GetCreatedJavaVMs`：

```cpp
JavaVM *GetJavaVm() {
    static JavaVM *cached = nullptr;
    if (cached) return cached;
    uintptr_t addr = DlsymLibartDynamic("JNI_GetCreatedJavaVMs");  // 遍历 /proc/self/maps 找 libart
    if (addr == 0) return nullptr;
    auto get_vms = reinterpret_cast<jint (*)(JavaVM **, jsize, jsize *)>(addr);
    JavaVM *vm = nullptr; jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || count == 0 || vm == nullptr) return nullptr;
    return cached = vm;
}

JNIEnv *GetJNIEnvForCallback() {
    if (g_vm == nullptr) g_vm = GetJavaVm();     // lazy bootstrap，必须
    if (g_vm == nullptr) return nullptr;
    JNIEnv *env = nullptr;
    jint r = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    } else if (r != JNI_OK) {
        return nullptr;
    }
    return env;
}
```

`libart.so` 是 APEX 模块，`dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs")` 不一定能拿到，需要按
demo 的方式自己解析 `.dynsym`（遍历 `/proc/self/maps` 定位 libart 段 + 解析 ELF 动态符号表）。

## 五、FindClass 上下文（载体 trampoline）

**问题**：soLoader 下 `native_init` 从框架栈上执行，ART 的 `JNIEnv::FindClass` 会按"当前方法的
声明类"的 ClassLoader 解析（`art/runtime/jni/jni_internal.cc` 的 `GetClassLoader`），结果解析到
**框架**而不是模块，模块 `FindClass` 自己类必失败。

**解法**（Vector 已内建，模块无需自己实现）：框架先建好模块 `ClassLoader`，再把一段预编译载体
dex（`org.matrix.vector.nativeload.NativeInitFire`，676 字节）附加到模块 ClassLoader 里，通过
`NativeAPI.firePendingNativeInits(carrierClass)` 调用载体类的 native 方法
`fireNativeInit()`：

```
FirePendingNativeInits(env, carrier_class)
  -> RegisterNatives 绑定载体类 fireNativeInit()V
  -> CallStaticVoidMethod(carrierClass, fireNativeInit)
     -> NativeInitTrampoline: pending.swap(g_pending_native_inits); RunPendingNativeInits(pending)
```

此刻 ART 的"当前方法"声明类是模块 ClassLoader 里的载体类，`FindClass` 于是解析到模块自己的
类。**对模块的影响**：

- 在 `native_init` 内（含它调用的同步代码路径）`FindClass("com/example/mymodule/MyBridge")` 可靠；
- 不要依赖 `Thread.contextClassLoader`（ART 的 JNI `FindClass` 完全不读它）；
- 载体 dex 只在内嵌库存在时才附加；若其分配失败会降级为框架上下文（日志
  `Carrier dex unavailable for ...`），此时 `FindClass` 会解析到框架，模块应能容忍这种降级
  （记录日志而不是崩溃）。

排障日志串：
```
Firing deferred native_init entrypoints for <pkg>
FirePendingNativeInits: pending_count=1 carrier_class=...
native_init for '...' registered library-load callback 0x...
native_init for '...' completed without library-load callback.     // 返回了 nullptr
native_init threw for module '...'
Failed to register the native_init trampoline on the module carrier class.
Module carrier class does not expose fireNativeInit()V.
```

## 六、scope.list 与"模块自身进程"的真相

`scope.list` **默认完全无效**。`FileSystem.readStaticScope` 的第一道门：

```kotlin
if (!props.getProperty("staticScope").toBoolean()) return@use null
```

`module.prop` 没有 `staticScope=true` 时，`scope.list` 只是 APK 里的一个文件，daemon 不读它；
模块作用域完全由用户在 Vector 管理器（或 `cli scope set`）里决定。

实测证据（本机 demo）：APK 里 `scope.list` 写了 `system / com.android.settings /
com.example.nathookdemo`，但 `module.prop` 无 `staticScope`，于是
`cli scope ls com.example.nathookdemo` 只显示用户手动勾选的
`com.chunqiunativecheck, com.demo.addapp, com.eltavine.duckdetector, com.kanman.allfree,
com.xff.launch, com.zhenxi.hunter, org.zywx.wbpalmstar.widgetone.uex11296876` —— **不含自身包名**。

由此产生一个高频困惑：**模块自己的 App 进程不被注入**。

```
scope 不含自身包名
  -> ConfigCache.shouldSkipProcess = modulesForProcess(...).isEmpty() = true
  -> VectorService.attachProcess 返回 null
  -> native RequestAppBinder result=none
  -> 日志：Not injecting '<pkg>': the daemon has no binder for it.
```

这是设计行为（只注入作用域内进程），不是 bug。若确实需要注入模块自身进程（例如要在 UI 进程里
验证 hook），必须显式做到：

- 在 `module.prop` 写 `staticScope=true` 且 `scope.list` 里包含自身包名，或
- 在管理器/CLI 里把自身包名加入作用域。

`modulesForProcess` 对子进程会回退到包名匹配（`ProcessScope(packageName, uid)`），所以写包名即可
覆盖该包的所有进程；`system` 是 `system_server` 的约定写法（`android` 只匹配普通 app 进程）。

### 6.1 `scope.list` 不支持注释（实测陷阱）

`staticScope=true` 时，`readStaticScope` 对 `scope.list` 的处理是：**逐行 trim，非空行全部
视为声明的包名**，没有 `#` 注释语法：

```kotlin
lines.map { it.trim() }.filter { it.isNotEmpty() }.toSet()   // 注释行原样进 claimed 集合
```

后果（2026-09 实测，本仓 `examples/duckusb`）：在 `scope.list` 顶部写了大段说明性注释，
`cli scope add` 的报错把注释的每个词都当成声明包名列出来：

```
Error: <pkg> fixes its scope in module.prop, so com.demo.addapp cannot be added.
It claims: #, # "system" is ..., # (sys.usb.state, init.svc.adbd), ..., system.
```

这不会立刻报错，但会让 `pruneScopeToClaimed` 以一堆垃圾 token 作为白名单，真实声明
（`system` / `com.android.systemui`）混在其中仍能匹配，于是“看起来能用”；一旦改动注释，
声明集合就跟着变。

**规则：`scope.list` 只写包名，一行一个，不写注释、不写行尾说明。** 说明写到 README
或 `module.prop` 附近；`module.prop` 走的是 `java.util.Properties`，它**支持** `#` 注释，
两者行为不同。

## 七、Java 侧入口与骨架（照抄 demo 的最终形态）

```java
public final class MyModule extends WbModule {
    @Override
    public void onModuleLoaded(WbModuleInterface.ModuleLoadedParam param) {
        // native_init 已在此前由框架触发；这里只做 Java 侧初始化
        Log.i(TAG, "onModuleLoaded proc=" + param.getProcessName());
    }
}
```

`java_init.list` 里列出的类必须 `extends WbModule`，否则框架日志
`Class does not extend WbModule, skipping.`。

模块内部桥接类（被 `native_init` 用 `FindClass` 找到的那个）建议纯静态、避免依赖 Android
组件生命周期：

```java
public final class MyBridge {
    // native_init 通过 FindClass(".../MyBridge") + GetStaticMethodID 调用
    public static void onHookEvent(String processName, String className) { /* ... */ }
}
```

若模块自带 `native` 方法（如 UI 手动开关）：

```java
public final class MyNativeBridge {
    // ⚠️ 直接调用会 UnsatisfiedLinkError（soLoader 下无声明的 native 方法）
    public static native boolean start();
}
```

两种正确处理：① 在 `native_init` 里用 `RegisterNatives` 显式绑定；② 像 demo 一样把它降级为可选
入口，调用点全部 try/catch（demo 的 `HookDataBridge` 静态块即是范例）。

## 八、数据回传与模块服务（可选）

**hook 数据回传**：模块在目标进程内拿到的数据要送回模块 App，常用两条路：
- 模块自带 ContentProvider + SharedMemory（demo 的做法：`NativeHookContentProvider` +
  `SharedMemoryHelper`，适合大数组/高频记录）；
- 框架的远程偏好（`WbInterfaceWrapper.getRemotePreferences` / `listRemoteFiles` /
  `openRemoteFile`，适合配置类小数据）。

**IWbService（模块 App ↔ daemon 服务通道）**：框架用来把 IWbService binder 推给模块 App，
要求模块 APK 中存在 authority 为 `<applicationId>.WbService` 的 ContentProvider——即引入
`io.github.libxposed:service:102.0.0`（Maven Central 已发布，manifest 自动合并 `WbProvider`）。
它提供框架信息、作用域管理、运行目标、热重载、远程偏好/文件。缺这个 Provider 时 daemon 会
三连投递失败并进入 60s 节流（`ModuleAppService`），日志：

```
No service provider for <pkg>
<modulePkg>/<uid> failed to take its binder 3 times in a row; retrying at most once every 60s
```

不提供该 Provider 不影响 hook 功能，只影响模块 App 使用上述服务 API。

## 九、构建配置要点

```kotlin
android {
    // soLoader 的 elf 装载要求 |max-page-size=16384| 之外无特殊要求；
    // 该 flag 由 root build.gradle.kts 统一注入，模块无需重复声明。
}
packaging {
    // META-INF/xposed/* 必须原样进包，否则 daemon 发现不到入口
    resources { merges += "META-INF/xposed/*" }
}
```

模块 APK 必须是 **release（非 debuggable）+ debug 签名**：debuggable APK 永远 run-from-apk，
系统拒绝 dex2oat，模块 dex 每次进程 fork 都会解释执行 + 验证 + JIT，实测把宿主启动拖慢 5~10 秒
（见 `AGENTS.md`《模块 APK 必须使用非 debuggable 构建》）。参考
`examples/native-hook-demo/build.gradle.kts` 末尾的 `buildTypes.release`。

## 十、验证清单

安装模块 → 重启 → 启动一个作用域内 App（**不要**由脚本主动启动，等用户手动启动）→ 期望日志：

```
Loaded native libraries for <pkg>: <name>!/libmy_module.so=true
Firing deferred native_init entrypoints for <pkg>
FirePendingNativeInits: pending_count=1 carrier_class=class org.matrix.vector.nativeload.NativeInitFire
native_init for '<pkg>!/libmy_module.so' registered library-load callback 0x...
[register-natives] hooked addr=... trampoline=...          // 模块自己的 hook 日志
[module-load] name='...' handle=...                        // 后续 dlopen 通知
```

失败排查顺序：`native_init.list` 名称与 APK 内路径 → 是否导出 `native_init` → `FindClass` 是否
用了框架上下文 → hook 是否用了 `entries->hookFunc` → scope 是否包含目标进程。

## 十一、常见坑速查

| 症状 | 原因 |
| --- | --- |
| `System.loadLibrary` 抛 `UnsatisfiedLinkError` | soLoader 基线行为，改用 native_init |
| `FindClass` 返回 null / ClassNotFound | 在框架上下文调用；需经载体 trampoline 触发（§5） |
| `native_init` 未被调用 | 未导出 `native_init`，或 `native_init.list` 与 `lib/arm64-v8a/*.so` 不匹配 |
| `completed without library-load callback` | `native_init` 返回了 `nullptr`（不是失败，但无回调） |
| `failed to resolve JNI_GetCreatedJavaVMs from libart .dynsym` | 未按 §4 解析 libart 动态符号，直接 dlsym 拿不到 |
| 模块自身 App 进程不注入 | scope 未含自身包名（§6） |
| `Class does not extend WbModule, skipping` | `java_init.list` 里的类没继承 `WbModule` |
| 宿主启动被拖慢数秒 | 模块 APK 是 debuggable（run-from-apk），改 release |
| `has bad ELF magic: 00000000` | APK 内 so 未正确打包/被截断；见 `AGENTS.md` 同名章节 |
| daemon 报 `No service provider for <pkg>` | 模块 APK 没有 `.WbService` Provider（§8，非必需） |
