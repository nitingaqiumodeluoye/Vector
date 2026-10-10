# AGENTS.md

## wxshadow KPM 源码位置与构建

wxshadow KPM（内核模块，`PR_WXSHADOW_PATCH` 的内核实现）的**最新源码就在本机**：
`E:/52/mykpm/kpms/wxshadow/`，仓库 `nitingaqiumodeluoye/mykpm`。涉及内核 patch 路径
（stop_machine/全局锁/IPI/TLB 击落等）的排查与修改，**直接改这份源码**，
不再需要 SSH 到远端服务器拉取。

> 早期文档写的 `E:/52/mkpms/kpms/wxshadow/`（“源码在 SSH 服务器上”）已作废：
> `mkpms` 是 `mykpm` 的笔误，该目录从来不存在；服务器副本也已过期。

同目录下还有 `kpms/karinahide/`（`karinahide.kpm`，供 rustFrida 的 `madvise`
清 COW 路径调用）与其它 KPM。

### 构建走 CI，不走本地也不走 SSH

**wxshadow / karinahide 的 `.kpm` 一律通过 GitHub Actions 构建**，不要在本地跑
`cmake`，也不要 SSH 到远端构建机。原因：

- 本机 Windows 缺 `aarch64-linux-gnu-gcc` 与 `cmake`，本地构建不具可行性；
- CI 的产物才是唯一权威产物，本地/远端手工编译会引入工具链漂移，
  “本地能编”与“设备能跑”是两回事。

```bash
cd E:/52/mykpm
# 改完源码推送即触发（`.github/workflows/build.yml`，push / PR / workflow_dispatch）
git push origin master

# 事件驱动等待，不要盲等固定时长
gh run list  --repo nitingaqiumodeluoye/mykpm --limit 5
gh run watch <run-id> --repo nitingaqiumodeluoye/mykpm --exit-status --interval 30
echo "退出码: $?"

# 取产物（-n 指定 artifact，避免 gh 多套一层目录）
gh run download <run-id> --repo nitingaqiumodeluoye/mykpm -n wxshadow-kpm    -D out/
gh run download <run-id> --repo nitingaqiumodeluoye/mykpm -n wxshadow-client -D out/
```

CI 的 `wxshadow` job（`ubuntu-latest` + `gcc-aarch64-linux-gnu`，抓 `.kp` 子模块头文件
后 `cmake --build --target wxshadow.kpm wxshadow_client`）产出两个 artifact：

| artifact | 文件 | 说明 |
| --- | --- | --- |
| `wxshadow-kpm` | `wxshadow.kpm` | ELF aarch64 relocatable 内核模块 |
| `wxshadow-client` | `wxshadow_client` | 静态链接的 aarch64 用户态客户端 |

另有 `kpm` job 产出 `karinahide.kpm`。发正式版打 tag 即可，release 流程自动出包。
CI 失败时用 `gh run view <run-id> --log` 取日志按上面的失败行排查，**不要退回本地编译**。

## rawVector 与 rustFrida 双基准

Vector 的整体运行流程与 hook 实现分别使用两套已验证源码作为基准，后续改造必须同时对照：

1. **整体流程以 rawVector 为准。**
   - Zygisk 注入、`preAppSpecialize` / `postAppSpecialize`、`forkCommon`、
     `bootstrapXposed()`、`ActivityThread.attach` / `LoadedApk` 框架 hook、模块发现与加载、
     `native_init` 注册、目标库加载回调和管理器连接时序，必须优先对照
     `D:\crack\rsfrida\rawVector`。
   - rawVector 决定“在哪个进程、哪个生命周期阶段、按什么顺序执行”。不得为了迁就当前
     Vector 的临时实现而擅自延后、提前或合并关键阶段。

2. **hook 处理以 rustFrida 为准。**
   - ART layout、ArtMethod clone、router/controller、native replacement、WxShadow patch、
     trampoline、stack walk、deopt、原方法调用、callback 参数封装、ARM64 relocator、inline
     hook 和缓存刷新等具体实现，必须优先对照 `E:\52\rustFrida`。
   - rustFrida 决定“hook 如何安装、路由、执行、恢复和清理”。不得用 rawVector 中的 LSPlant、
     Dobby 或其它旧 hook 后端替代 rustFrida 的 WxShadow 语义。

3. **组合原则。**
   - 默认判断规则是：**rawVector 决定时机和流程，rustFrida 决定 hook 的实现和语义**。
   - Vector 适配层只负责把 rawVector 的调用时序接到 rustFrida 对齐的 hook 后端；适配代码不得
     改变任一侧的关键语义。
   - 若两侧无法直接组合，必须先列出 rawVector 调用点、rustFrida 对应实现和 Vector 当前差异，
     再做最小适配，并在代码注释或工作汇报中说明原因。

4. **对照与排障顺序。**
   - 先沿 rawVector 确认从 Zygisk 到 framework bootstrap、模块 Java/native 入口和目标库回调的
     完整时序，再沿 rustFrida 确认该阶段所需 ART/inline hook 的安装与调用语义，最后检查 Vector
     适配层是否遗漏、重复或错序。
   - 启动过晚、模块加载过晚、回调注册错过目标库等问题优先对照 rawVector；router、trampoline、
     原调用、寄存器/栈、WxShadow patch、崩溃或 hook 不生效等问题优先对照 rustFrida。
   - 不得只对照其中一个项目后就宣称整体链路已对齐。

5. **源码可用性。**
   - 如果 `D:\crack\rsfrida\rawVector` 或 `E:\52\rustFrida` 不存在、关键文件缺失或
     对应逻辑无法定位，不得凭经验继续实现；应先报告缺失的基准和具体阻塞点。

6. **构建一律走 CI，不走本地也不走 SSH。**
   - **rustFrida**（`E:\52\rustFrida`，仓库 `nitingaqiumodeluoye/rustFrida`）：产物通过
     GitHub Actions 构建，workflow `Build (Android aarch64)`，artifact
     `rustfrida-aarch64-<sha>`（含 `rustfrida` / `libagent.so` / `zymbiote.elf` /
     `rfclient` / `SHA256SUMS.txt`）。权威说明见该仓库的 `doc/build.md`，
     `scripts/wait-ci.sh` 与 `scripts/check-module.sh` 是其配套脚本。
     NDK 铉在 `/opt/android-ndk-r29`（与仓库内 `.cargo/config.toml` 一致）。
   - **KPM**（wxshadow / karinahide）：见本文开头《wxshadow KPM 源码位置与构建》。
   - 不要本地跑 `cargo` / NDK 构建，也不要 SSH 到远端构建机：本机工具链版本与 CI
     不一致，本地产物与 CI 产物不等价，而且手工编译绕过 CI 会掩盖“本地能编、CI 编不过”
     的真实分歧（如宏作用域、`unused import` 等 lint 升级为 error）。
   - 需要改 rustFrida 源码时，直接改本机 `E:\52\rustFrida` 仓库并推送触发 CI，
     不要在服务器副本上改。

## Vector ART Hook 改造约束

当前项目正在进行 Vector 改造：将原本的 ART hook 实现改为 `E:\52\rustFrida` 中的 wxshadow 模式 ART hook 实现。

后续涉及 ART hook、WxShadow、ArtMethod、router、native trampoline、inline hook、deopt、原方法调用、callback 参数封装、ARM64 relocator 或相关构建接入的修改时，必须遵守以下规则：

1. **不得凭经验或猜测实现。**
   - 修改前必须先对照 `E:\52\rustFrida` 中对应的 wxshadow 模式 ART hook 实现。
   - 不确定语义时，先查源实现；源实现仍不明确时，停止并说明不确定点。

2. **以 rustFrida 的 wxshadow ART hook 行为为准。**
   - Vector 侧改造应尽量保持 rustFrida 对 ART layout、ArtMethod clone、router table、native replacement、wxshadow patch、trampoline、stack walk 兼容处理等关键行为的一致性。
   - 需要适配 Vector 现有 API 时，应明确区分“保持源实现语义”和“Vector 接口适配代码”。

3. **修改时要标明参考依据。**
   - 对关键逻辑变更，应在提交说明、代码注释或工作汇报中指出参考的 rustFrida 文件/函数。
   - 如果 Vector 侧实现与 rustFrida 不一致，必须说明原因，例如现有 API 兼容、构建限制、ABI 限制或安全边界不同。

4. **优先收敛到最小可验证改动。**
   - 不要同时重写无关模块。
   - 不要为了“看起来完整”添加未对照过的推测性逻辑。
   - 新增 hook 路径时应保留可回退路径，除非已确认替代路径在目标场景中完全覆盖。

5. **源实现不可访问时不得继续猜测。**
   - 如果 `E:\52\rustFrida` 不存在、文件缺失、实现路径无法定位或关键函数无法确认，应先报告阻塞点，再请求明确指示。

6. **排障时优先对比原版 rustFrida。**
   - 原版 `E:\52\rustFrida` 已确认能在目标设备上正常使用。
   - 如果 Vector 移植逻辑在某个环节出现异常、崩溃、hook 不生效、参数/返回值错误、ART layout 推断失败、wxshadow patch 失败或行为不一致，应优先检查 Vector 当前实现与原版 rustFrida 对应逻辑的差异。
   - 排查结论应明确指出问题是来自 Vector 适配层、移植遗漏、构建差异、运行环境差异，还是与原版逻辑不一致。

## 当前 inline hook 基线

下一阶段工作是对齐 `E:\52\rustFrida` 的 inline hook 行为。开始改动前，先以以下 Vector 当前状态作为基线，随后逐项对照 rustFrida 源实现：

1. **Vector 当前 inline hook 统一入口。**
   - `HookInline()` / `UnhookInline()` 是兼容包装，但不得再包装 Dobby；普通 replacement 必须走 Vector 自带 ARM64 wxshadow hook engine：`hook_install(target, replacement, stealth=1)`，unhook 走 `hook_remove(target)`。
   - `NativeAPIEntries.hookFunc` / `unhookFunc` 必须指向同一套 wxshadow inline hook 实现，不允许为模块单独恢复 Dobby 或 mprotect fallback 路径。
   - ART wxshadow 改造路径同样使用 Vector 自带 ARM64 hook engine，主要位于 `native/src/art_hook/hook_engine_inline.c`、`hook_engine_mem.c`、`arm64_writer.c`、`arm64_relocator.c`。
   - Dobby 只能作为历史依赖或外部子模块存在；涉及模块/native inline hook 时不得新增对 Dobby 的运行时依赖。

2. **自带 hook engine 的核心流程。**
   - `hook_install()` 是简单 replacement：创建 `HookEntry`，构建 trampoline，再把目标入口 patch 到 replacement。
   - `hook_attach()` 生成 Frida-style enter/leave thunk：保存 `HookContext`，调用 enter callback，可选择 wrap 原函数并执行 leave callback，或在无 leave 时 tail-jump 到 trampoline。
   - `hook_replace()` 生成 replace thunk：保存寄存器上下文和 trampoline 地址，调用 callback，由 callback 决定是否通过 `hook_invoke_trampoline()` 调原方法。
   - `setup_hook_entry()` 会保存目标入口原始字节，分配 trampoline，并拒绝重复 hook。
   - `build_trampoline()` 使用 ARM64 relocator 重定位被覆盖指令，然后追加 jump-back 到原函数剩余代码。
   - `patch_target()` 根据目标距离生成 B、ADRP+ADD+BR 或 MOVZ/MOVK+BR 跳转，并按实际 patch 长度回写 `original_size`、必要时重建 trampoline。

3. **当前 patch 策略。**
   - `stealth == 1` 时走 `wxshadow_patch()`，通过 `prctl(PR_WXSHADOW_PATCH, ...)` 写 shadow page；失败后尝试 PMD split + COW，再失败则直接报错，不降级到 mprotect。
   - wxshadow 跨页 patch 会先写第二页，再写第一页，减少半跳转窗口。
   - 同页 LDR literal relocation 代码仍存在，但当前 `patch_target()` 中 diagnostic 开关固定跳过该扫描。
   - `stealth == 0` 时优先找同 inode 的 `rw-s` sibling 映射直写，失败后才走 `mprotect(RWX) + memcpy + restore prot`。

4. **当前 trampoline / thunk 内存策略。**
   - trampoline 不强制 near target，因为 jump-back 使用绝对跳转。
   - thunk 需要 near target，`hook_alloc_near()` 优先尝试 ±128MB 以便 4 字节 `B` patch，不行再尝试 ±4GB 的 ADRP patch。
   - exec pool cleanup 会保留 pool 范围给外层 safepoint / munmap 逻辑使用，不能在未确认无执行线程时直接释放。

5. **对齐 rustFrida inline hook 时的检查顺序。**
   - 先定位 rustFrida 中 inline hook、wxshadow patch、trampoline、relocator、原函数调用相关文件和函数。
   - 再逐项对比 Vector 的 `hook_install()`、`hook_attach()`、`hook_replace()`、`setup_hook_entry()`、`build_trampoline()`、`patch_target()`、`hook_relocate_instructions()`、`wxshadow_patch()`。
   - 对比点必须覆盖：覆盖字节长度、跳转编码选择、原始字节读取、XOM 处理、wxshadow 失败策略、跨页写入顺序、同页 literal 处理、trampoline 语义、寄存器保存范围、缓存刷新、remove/cleanup 语义。
   - 未确认 rustFrida 行为前，不要新增推测性 inline hook 逻辑。

## 模块 native so 加载约束

**2026-09-11 用户通知恢复 soLoader，本节基线切换回 soLoader 路径并立即生效。**
当前生效链路：模块 native ELF 由 daemon 读取到只读 SharedMemory，Java 侧
`NativeAPI.loadNativeLibraries(names, images)` 把 SharedMemory fd 交给 Vector core，由
`ElfLoader::LoadLibraryFd()` 自装链接；模块 Java 层不得再走 `System.load()` /
`System.loadLibrary()` / APK zip entry 直载 linker。

1. **生效基线（soLoader）。**
   - `VectorModuleManager` 的 `USE_RAW_VECTOR_LINKER_NATIVE_LOADER = false`：
     `librarySearchPath` 保持空串，模块库由 daemon sealed SharedMemory +
     `NativeAPI.loadNativeLibraries()` 走 soLoader 装载。
   - `META-INF/xposed/native_init.list` 与 `assets/native_init` 仍是元数据来源；
     `native_init` 由 soLoader 装载后由 core 调用并取回库加载回调。
   - `native_init` 返回的库加载回调仍可对后续目标库 `handle` 调用 `dlsym(handle,...)`
     定位导出符号；这不等同于加载目标库。
2. **linker 直载路径仅作诊断保留。**
   - `native/src/core/native_api.cpp` 的 `kDiagUseAndroidLinkerForModuleNative` 诊断开关
     保持 `false`； linker 直载分支与 `VectorModuleManager` 的
     `USE_RAW_VECTOR_LINKER_NATIVE_LOADER=true` 分支代码保留用于诊断，默认不得启用。
   - 暂停期遗留的经验已验证：linker 直载会在目标进程 maps 留下 `base.apk`
     外来代码容器映射（xff T2 `MAPS_FOREIGN` 口径），恢复 soLoader 后应不复存在。
3. **native inline hook 能力。**
   - 模块 native so 仍只能使用 Vector 提供的 `NativeAPIEntries` 和 Vector core/agent 的
     inline hook 能力，不得自行引入第二套 inline hook、mprotect fallback 或 Dobby runtime。
   - 装载后端切换不改变 ART hook、inline hook、router、trampoline 和原函数调用的
     soLoader/rustFrida 对齐要求。
4. **CSOLoader 对比决策与 TLS 吸收（2026-09-11）。**
   - 对比 `E:\52\CSOLoader`（PerformanC，AGPL-3.0，ReZygisk 采用）与
     `external/soLoader`（MIT，Vector 已集成）后，用户选定：**继续用 soLoader 作为模块 so
     装载后端；CSOLoader 仅作为功能参照，不作为运行时后端引入**（AGPL 传染顾虑、
     接口/管理局线成本、风险隔离；其 file-backed mmap 反而比 soLoader 的匿名拷贝映射
     多留一个文件路径痕迹面）。
   - **吸收项：TLS 重定位支持。** soLoader 原先不处理 `TLS_DTPMOD/TLS_DTPREL/TLS_TPREL/
     TLSDESC` 与 `__tls_get_addr`，带 `thread_local` 的模块 so 会拒载。参照 CSOLoader
     `linker.c` 的 TLS 模型（全局 module 表 + pthread key per-thread DTV +
     `__tls_get_addr` 自有实现 + TLSDESC dynamic resolver + TPREL 的 addr-tpidr emulation）
     新增 `TlsManager`，符号级 `__tls_get_addr` 替换仅作用于 soLoader 装载的 image。
   - 已知局限（与 CSOLoader 同款语义，需在代码注释保留）：TPREL emulation 以发起线程的
     块地址计算 tpoff，后续线程共用同一块；跨 image TLS 引用 v1 不支持（明确报错提示，
     避免静默错误符号值）。模块 so 常驻场景下 unload 语义仅在 register/unregister
     generation 翻转时生效。

## 编写 native 模块的硬约束

**每次新建或修改模块（`examples/`、用户模块、任何带 `native_init.list` 的 APK）都必须遵守。**
完整原理、可照抄骨架与排障依据见 `docs/native-module-guide.md`，本节只列无条件生效的铁律。

1. **禁止 `System.loadLibrary()` / `System.load()` / APK zip entry 直载 linker。** 模块 so 只能
   由 daemon sealed SharedMemory + `NativeAPI.loadNativeLibraries()` →
   `ElfLoader::LoadLibraryFd()` 装载；`VectorModuleClassLoader.findLibrary` 在 soLoader 模式下
   故意跳过 `apk!/lib/<abi>/*.so`。模块 Java 声明的 `native` 方法没有自动绑定来源，要么在
   `native_init` 里 `RegisterNatives`，要么当可选入口并兜底 `UnsatisfiedLinkError`。
2. **模块 so 必须导出 `native_init(NativeAPIEntries*)`，且返回非 null 的库加载回调。**
   符号名固定为 `native_init`；`JNI_OnLoad` **不会被调用**（soLoader 只跑 `DT_INIT`/
   `init_array`），不得依赖它，也不得用它获取 `JavaVM*`。
3. **`JavaVM*` 只能从 libart `.dynsym` 解析 `JNI_GetCreatedJavaVMs` 自举**（对齐 rustFrida
   `get_or_init_vm`），并在首次使用时 lazy 初始化；首次 `native_init` 里
   `env->GetJavaVM()` 不可靠。
4. **`FindClass` 只在 `native_init` 内经载体 trampoline 可靠解析模块自身类**；不得依赖
   `Thread.contextClassLoader`（ART 的 JNI `FindClass` 不读它）。载体
   （`org.matrix.vector.nativeload.NativeInitFire`）由框架内建，模块无需实现，但要容忍
   `Carrier dex unavailable` 的框架上下文降级。
5. **inline hook 只能使用 `entries->hookFunc` / `entries->unhookFunc`**（Vector wxshadow
   engine）。禁止 Dobby、mprotect fallback 或任何第二套 inline hook。
6. **replacement 的签名/ABI 必须与被 hook 函数一致**，属裸替换语义；原调用走 `backup`
   trampoline（`hookFunc` 返回 0 且 `backup != nullptr` 才算成功）。
7. **`scope.list` 仅在 `module.prop` 含 `staticScope=true` 时生效**；要让模块自身 App 进程
   被注入，必须在 scope 中显式包含自身包名（`staticScope=true` + `scope.list`，或管理器/
   `cli scope` 手动添加）。scope 不含自身包名时 `RequestAppBinder` 返回 none、
   `Not injecting ...: the daemon has no binder for it.` 是设计行为，不是 bug。
8. **`native_init.list` / `assets/native_init` 只接受 `lib/arm64-v8a/<纯文件名>.so`**：文件名
   必须 `.so` 结尾、不得含 `/` 或 `\`，且 soLoader 目前只支持 arm64-v8a。
9. **模块 APK 必须 release（非 debuggable）+ debug 签名**，且 release 前先 bump
   `versionCode`/`versionName`（见《版本号更新规则》与《模块 APK 必须使用非 debuggable 构建》）。
10. **改完必须按验收日志串自检**：`Loaded native libraries for ...`、
    `Firing deferred native_init entrypoints for ...`、`FirePendingNativeInits: pending_count=`、
    `registered library-load callback`。缺 `registered library-load callback` 优先查
    `native_init` 是否返回 nullptr 或 `native_init.list` 名称不匹配。禁止用脚本主动启动目标
    App 采集这些日志（见《安装重启后的启动约束》）。

## 目标设备：以当前 adb 连接为准

**Vector 不绑定任何固定机型或串号。**所有设备操作的唯一基准是当前 `adb` 连接的设备：

```bash
adb devices -l                            # 以实测输出为准，不预设型号
adb shell getprop ro.product.model
adb shell getprop ro.build.version.sdk
```

历史文档里出现过的“alioth / M2012K11AC”、“rubens / 22041211AC”等只是**当时那一次**测试
所用的机器，不是本项目的目标机型。换机器后旧的设备相关结论（型号专属路径、
OEM 行为、性能基线）必须重新实测，不得直接套用。

设备特征（内核版本、Android 版本、页表粒度、KPM/root 方案）**必须在动手前实测**，
例如：

```bash
adb shell 'su -c "uname -a"'                    # 内核版本
adb shell getprop ro.build.version.release       # Android 版本
adb shell getprop ro.product.cpu.abi             # ABI（Vector 目前只支持 arm64-v8a）
```

在多设备环境（同时插了多台机器）下，必须用 `adb -s <serial>` 显式指定目标，
不得依赖 `adb` 的默认选择。

## 本机 Android SDK 环境

本机存在可用 Android SDK：`D:\sdk`。

当前 PowerShell 环境中 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 可能为空，但这不代表本机没有 SDK。执行 Gradle、ADB 或 Android 构建相关命令前，应在同一 PowerShell 会话中显式设置：

```powershell
$ErrorActionPreference = 'Stop'
$env:ANDROID_HOME = 'D:\sdk'
$env:ANDROID_SDK_ROOT = 'D:\sdk'
```

不要仅因为 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 为空就判定 Android SDK 不可用；应先检查 `D:\sdk`。

## 源码修改与编译的准入约束

1. **修改源码前必须先向用户提问并获准。**
   - 任何涉及源码的修改（native/、xposed/、zygisk/、daemon/、manager/、legacy/、examples/、
     构建脚本等）在动手前，必须先向用户提出修改方案（改什么、为什么、受哪些基准约束），
     用户明确同意后才能执行。
   - 禁止未经用户允许修改源码；包括“顺手修”“先斩后奏”“等最终验证再确认”等形式。
   - 可以先做只读性操作（读代码、logcat、/proc、dmesg 等取证与分析），分析结论可直接
     汇报给用户，但不构成修改授权。
2. **未经允许不得启动编译/打包/安装/重启。**
   - 不得自行触发 gradle 构建、产物打包、模块安装或设备重启；
     **推送到 fork 触发 CI 也算“启动编译”**，同样需要先获准。
   - 只有用户明确要求构建、安装或“一条龙”验证时才允许执行；用户明确指定“仅编译”或
     “不要安装/重启”时按其范围停住。
3. **与“编译验证节奏/Vector 构建自动衔接”的关系。**
   - 自动衔接规则仅在用户已明确获准的构建请求内生效：获准的流程从编译到验收连续跑完。
   - 自动衔接不授权任何新的源码修改；如跑链路中发现需要新的源码改动，必须停下并重新
     征求用户同意。

## 编译验证节奏

### Vector 构建走 CI，不走本地

**Vector 的构建产物一律通过 GitHub Actions 产出**，不在本机跑 `gradlew`。

**为什么**：

- 本机 Windows 缺 `ninja`，且 CI 的 `Remove Android's cmake` 步骤会故意删掉
  `$ANDROID_HOME/cmake` 以强制走 `externalNativeBuild` 的 ninja 路径 —— 本地环境
  与此不等价，本地能编不代表 CI 能编。
- CI 有 ccache 缓存（key 按 `build.gradle` 与 `CMakeLists.txt` 的 hash 分桶），
  native 重编比本地快；本机没有这套缓存。
- 公共仓库的 runner 不计费，没有省额度的必要。
- 真出问题的是 CI，本地成功反而会掩盖分歧。

**去哪跑**：本仓库 `origin` 直接指向上游 `JingMatrix/Vector`，**没有 push 权限**。
必须 fork 到自己的账号后用 fork 作为 remote：

```bash
gh repo fork JingMatrix/Vector --remote=false --clone=false
# 加为 remote（只加一次）
git remote add fork https://github.com/<your-account>/Vector.git
```

**怎么触发**：上游的 `core.yml` 只在 `master` push / `v*` tag / PR /`workflow_dispatch`
触发。为不污染 fork 的 `master`（保持与上游一致），按下面做：

```bash
# 1) 推当前开发分支到 fork（分支名保持原样）
git push fork feature/rustfrida-art-hook-wxshadow

# 2) 手动触发（workflow 文件在默认分支上，所以能在任意 ref 上跑）
gh workflow run core.yml \
  --repo <your-account>/Vector \
  --ref feature/rustfrida-art-hook-wxshadow

# 3) 找到刚触发的 run 并事件驱动等待
gh run list --repo <your-account>/Vector --limit 3
gh run watch <run-id> --repo <your-account>/Vector --exit-status --interval 30
echo "退出码: $?"
```

**产物**：CI 跑 `./gradlew zipAll`，同时产出 Release 与 Debug 两个包（`zygisk/release/`），
artifact 名就是 zip 文件名，形如：

| artifact | 内容 |
| --- | --- |
| `Vector-v<versionName>-<versionCode>-Release` | R8 混淆后的框架包，日常装这个 |
| `Vector-v<versionName>-<versionCode>-Debug` | 保留符号，排障用 |

下载（GitHub 的 artifact 需要登录，`gh` 已登录故无此限制）：

```bash
gh run download <run-id> --repo <your-account>/Vector \
  -n "Vector-v<versionName>-<versionCode>-Release" -D out/
```

CI 失败时用 `gh run view <run-id> --log` 取日志定位，**不要退回本地编译**。

### Vector 构建自动衔接

对 Vector 执行任何会生成新构建产物的修改或构建请求时，默认必须在编译成功后自动继续完成：
打包、安装当前新版本、重启设备、等待设备上线，以及 Vector/管理器状态验收。不得在“编译成功”
处自行停下，也不得继续使用旧版本产物。只有用户明确指定“仅编译”或“不要安装/重启”时，
才允许停在编译或打包阶段；若安装、重启或验收失败，应继续处理到完成，或明确报告阻塞原因。

> 走 CI 后，“编译成功”的位置在 CI 跑完、产物已下载到本地。安装仍可用模块自带的任务链
> （`installApatchRelease` 等），但也可以直接下载 CI 产物安装；两者产物等价（同一次构建）。

进行 Vector 改造或问题修复时，不要每修改一点就立即触发一次 CI。

应先完成当前问题相关的一组对齐改动，例如：

- 对照原版 rustFrida 找到对应实现；
- 完成 Vector 侧必要的逻辑同步；
- 修正同一问题链路上的调用、头文件、构建脚本或适配层；
- 做完静态检查和引用搜索。

确认这组改动已经收敛后，再一次性触发 CI。除非用户明确要求快速验证某个单点，或者某个
CI 失败本身就是当前唯一阻塞点，否则避免“边改一点边跑一次”的低效循环。

## 版本号更新规则

每次执行会生成新 Vector 构建产物的编译前，都必须先同步递增可见版本号/版本码，不得等到
安装、重启或设备验证阶段才补改版本。版本更新至少包含安装管理器显示的 `versionCode`，以及
项目中对应的版本来源字段。即使本轮只做本地编译验证、不立即安装，也必须使用新的版本号；
不得在同一个 `versionCode` 下生成或安装内容不同的构建产物。

这样每次安装后都能通过 APatch/Magisk/KernelSU 模块列表、Vector CLI 状态或设备端文件信息
清晰确认当前运行的是新构建，而不是旧产物或缓存产物。

## 安装重启验证节奏

当用户要求安装重启验证当前 Vector 构建时，应遵守以下流程：

1. 使用当前构建产物执行安装并重启设备。
2. 设备重新上线后，只确认 Vector/管理器状态是否为 `unknown`；这个状态确认属于安装结果验证，不属于原因排查。
3. 如果状态仍为 `unknown`，不要排查日志、安装结果、模块目录、Zygisk/NeoZygisk、权限标签或运行时崩溃原因；必须直接卸载当前 `zygisk_vector` 模块，重新安装当前构建产物并重启。
4. 上述“确认状态 -> unknown 则卸载 -> 安装 -> 重启”的流程必须循环执行，直到 Vector/管理器状态不再是 `unknown`；不得只执行一次重装就停止。
5. 每一轮安装和重启前后都要记录使用的构建产物、卸载/安装命令、设备上线状态和最终状态结果。

## 安装重启验收标准

一次安装重启验证只有同时满足以下条件才算成功，不能仅以模块已安装或 `module.prop` 版本正确作为成功依据：

1. Vector/管理器状态不是 `unknown`。
2. Vector 已实际激活：`system_server` 的 ART backend/hook 初始化成功，框架 hook 未因 `No ART backend installed`、ActivityTaskSupervisor hook 失败或同类错误而退出。
3. 管理器可正常被 Vector 拉起并完成连接/显示已激活状态；如果管理器未自动出现，必须通过 Vector 支持的启动入口触发后再确认。
4. 若状态为 `unknown`，继续执行上一节规定的“确认状态 -> 卸载 -> 安装 -> 重启”循环；在状态恢复为非 `unknown` 前不得进入原因分析，也不得宣称修复完成。
5. 最终汇报必须分别记录：构建版本、安装结果、重启后状态、ART hook 证据和管理器激活证据。

## 每次重装必须先验证 Vector 激活状态（硬性前置）

**任何验收、压测、崩溃统计、日志取证之前，必须先确认 Vector 已真正注入目标进程。**
激活未确认时得到的“零崩溃 / 零 ANR”一律视为**假阴性**，数据作废、必须重跑
（实测教训：曾出现 4 轮“零崩溃”，实为 `vectord` 注入失败进入重启循环、模块压根没注入，
被测代码一行没跑）。

### 激活判据（按顺序，缺一不可）

1. **注入链进程在**：`ps -A -o PID,NAME | grep -iE 'zygisk|vectord|apd'` 应含
   `zygisk-ptrace64`、`zygiskd64`、`vectord`。
2. **无注入失败循环**：logcat 中 `VectorDaemon: Failed to inject VectorService into
   system_server` / `System Server died` / `Restarting system_server` 必须为 0；
   出现任一即说明已进入重启循环，全机不再注入。
3. **目标进程内有本模块日志**：`VectorNative`、`startup-trace`（信号层另有 `WxShadow`）。
4. **有 hook 装上的证据**：如 duckusb 的
   `Settings spoof: hooks installed=4 detail: Global.getInt/3=1 …`。
5. **禁止**用 `/proc/<pid>/maps` 是否含 `zygisk_vector` 作判据：本模块用 CustomLinker
   匿名映射，linker 无 soinfo，正常激活时也查不到。

### 已知会破坏激活的两类原因与处置

- **代码侧（改动了 dispatch 依赖的 `HookContext::x[0]` 语义）**：
  router prologue 把帧槽 SP+0 从“被 hook 的原方法”换成 replacement 后，`ctx->x[0]` 随之改变，
  托管路由失效 → `vectord` 报 `Failed to inject VectorService into system_server` 并重启
  system_server 循环。**帧槽要改，只能改帧内存（走栈可见的那 8 字节），`ctx->x[0]` 必须
  保持“被 hook 的原方法”语义**（对照 `vector_art_hook.cpp::OnDoCallEnter`：inline 引擎里
  ctx 本身就是帧，故两者天然统一）。
- **设备侧（加载器登记表陈旧/残缺）**：连续多次 `installApatchAndRebootRelease` 可能让 Zygisk
  加载器登记表变旧：实测 `/data/adb/zygisksu/modules_info` 记录的 `.so` size 与实际文件不符
  （1511424 vs 1557872）、条目缺失，后果是全系统 0 条 Vector 日志。
  **❌ 绝对不要删除 `/data/adb/zygisksu/modules_info*`**：那是 NeoZygisk（id=zygisksu）
  自己的模块登记表，实测删除后它**不会被重建**，且进入“无模块可加载”的死胡同——
  症状是全模块（含未修改的 `zygisk_propmask`/`tricky_store`）的 helper 进程
  （`zygiskd64-<module>`）全部消失，**与代码无关**。
  **✅ 正确处置**：在 APatch 管理器里对模块做一次重新安装/重新启用，
  让 NeoZygisk 自己重建登记表；或重新跑模块安装流程（`installApatchAndRebootRelease`）。
  判定“加载器整体挂了还是只有我方模块挂了”：看 helper 进程
  `ps -A -o PID,NAME | grep zygiskd64-`，若有其它模块的 helper 而只有 vector 缺失，
  才可能是代码问题；全部缺失则是加载器/登记表层级问题。
  另外：`APatchD : apd::cli: command: Module { command: List }` 每 3 秒一次只是管理器
  轮询列表，**不是失败重试循环**，不得当作故障证据。
  结论：**同一 boot 周期内不要多次重装。**

### 汇报要求

汇报“已激活”时，必须同时给出：注入链进程、上述 4 条日志判据的**实测条数**，
以及本轮所用构建产物路径/时间戳；未给判据数字的“已激活”不算确认。

## 帧槽铁律：`HookContext::x[0]` 禁止在 pass-through 路径改写（zygote 崩因，2026-09-15 实测）

**背景（一次真实事故）**：为消除走栈崩溃，曾在 `vector_art_hook.cpp::OnDoCallEnter`
的三条提前返回路径上改写 `ctx->x[0]`（`!replacement` 写 0；`orig_bypass_consume_current_thread`
与 `!ShouldReplaceForManagedStack` 写 replacement）。结果：**zygote 崩溃** → NeoZygisk 反复重启
zygote → **所有应用都拿不到模块**（表现为“未激活”：0 条 `VectorNative`、无 `zygiskd64-<模块>`
helper、`modules_info` 不重建）。已全部撤销。

**机制（必须记住）**：`emit_save_hook_context`（`hook_engine_inline.c:51`）建的 `HookContext`
就是一块 ART `SaveRefsAndArgs` 形状的帧：

```
sub sp, sp, #352
stp x0,x1 -> [sp,#0]      ← SP+0 同时是：ART StackVisitor 读的“帧方法槽” + 保存的 x0
… x0..x30 @ SP+0..240, sp@248, pc@256, nzcv@264, trampoline@272, d0-d7@280, intercept_leave@344
```

一个内存字承担两个角色：**ART 的方法槽** 与 **原函数执行时必须为 ArtMethod 的 x0**。
pass-through / `$orig` 直通 / miss 的语义是“继续执行原函数”，此时 `x0` 必须还是原 ArtMethod；
改它就是给原函数送 NULL 或 malloc 假方法 → 立即崩（zygote 第一个中招）。

**对齐依据（rustFrida `art_controller.rs::on_do_call_enter`，已逐行核实）**：rustFrida 在
`hook_managed_reentry_guard_active` / `orig_bypass_consume_current_thread` / `callOriginal bypass` /
`!should_replace_for_stack` / `!replacement` **五条路径全部不改 `ctx.x[0]`**，
只有“真正替换”那条走到底才写 `ctx.x[0] = replacement; ctx.intercept_leave = 1`。
因此：**任何在此处“为了走栈安全”而改写 x[0] 的做法都是自创、且必然崩**；要走栈安全只能改
帧布局（把方法槽与保存的 x0 分开）或走被动修复层。

**诊断口诀**：模块整体不加载（无 helper、无 `VectorNative`）时，**先查 zygote 是否在崩**：
logcat/`logcat.last` 里的 `zygiskd64: zygiskd::zygiskd: Zygote restarted, cleaning up companion sockets`
或用户侧 “zygote crashed”。**不要**先怀疑加载器/登记表/NeoZygisk。

## 对齐差距表：rustFrida 走栈防护 8 层 vs Vector 现状（2026-09-15 核对）

rustFrida 自述（`art_controller.rs:1042` 初始化日志）分层：
`Layer1, Layer2, GC, OatHeader, Fixup, PrettyMethod, DecodeGcMasks, InlinePatch`。

| 层 | Vector 现状 | 说明 |
| --- | --- | --- |
| Layer1 / Layer2 | ✅ 已对齐 | 共享 JNI/Nterp 路由 |
| GC | ✅ | `OnGcSyncEnter/OnGcSyncLeave` |
| OatHeader | ✅ | `OnGetOatQuickMethodHeader`（`vector_art_hook.cpp:1219+`） |
| **Fixup** | ✅ **与 rf lean 模式一致 → 不加** | rustFrida 的 Fixup/GC/OAT 同属 `skip_global_sync_hooks`，**精简预装（lean preinit）模式会整体跳过**（`art_controller.rs:784`：“精简预装模式也跳过「全局同步 hook」(GC/OAT/FixupStaticTrampolines)”）；Vector 已对齐 lean 收口。
另 Vector `hook_engine` 已有注释说明排除理由：ClassLinker fixup 在持锁期，“doing replacement writes and icache maintenance there can block dex registration and the next GC checkpoint”。**结论：不加** |
| PrettyMethod | ✅ **与 rf 一致（禁止移植）** | rustFrida 该 hook 代码存在但**用 `if false &&` 永久禁用**（`art_controller.rs:988-990`，注释：*"PrettyMethod can run from ART/JD crash and signal paths. Routing it through our hook pool can leave a thread unsuspendable if a signal handler blocks there"*）→ 它改靠 OatHeader + InlinePatch 从源头避免走到 FATAL。**不得拿 tombstone_09 去 hook PrettyMethod** |
| DecodeGcMasks | ✅ 已移植 | 本轮 `OnDecodeGcMasksOnlyEnter` + `g_dummy_oat_header` |
| **InlinePatch** | ✅ **已修（2026-09-15）** | rustFrida：`hook_patch_inlined_oat_header_checks(bridge.get_oat_quick_method_header)`（`art_controller.rs:1022-1029`），**无条件调用**（在 lean 门控之外）。
Vector 原本已调用但条件是 `oat_header_ready`（= `g_is_system_process && InitializeOatHeaderLayer()`）→ **app 进程内联点未打补丁**（= tombstone_09 崩在 hunter app 进程的根因）。
现改为与 rf 一致：不论进程，只要能解析 `GetOatQuickMethodHeader` 本体地址就打补丁；验收日志 `WxShadow ART inlined OAT header patch ready: count=N exclude=0x…` |

**tombstone_09 族根因（与 rustFrida 同源）**：inline thunk 的 **wrap 路径**（`on_leave != NULL`，
Java hook 默认）不 tail-jump，`BLR` 原函数返回后执行 `str x0,[sp,#0]`（把**返回值**写回
`HookContext` 偏移 0）——而它就是 ART 的方法槽 → 走栈时拿到“返回值当方法” → `GetOatQuickMethodHeader`
失败 → FATAL 分支 → 拼 `PrettyMethod()` 时对 NULL DexFile 取址 SEGV。rustFrida 有**同一句代码**，
它的对策不是改这句，而是上面那两层防护（InlinePatch 源头拦截 + OatHeader/DecodeGcMasks 被动修复）。

**后续动作（按对齐优先）**：① **接入 InlinePatch**：在 Vector 初始化里调用 oat patch 的安装函数（对照 rustFrida `hook_patch_inlined_oat_header_checks(bridge.get_oat_quick_method_header)`，传 `GetOatQuickMethodHeader` 本体地址）——这是消除 tombstone_09 族的**对齐手段**；
② 核对 Fixup 层（GC hook 是否覆盖 `FixupStaticTrampolines`）；③ **不要** hook PrettyMethod（rustFrida 已验证会因信号/崩溃路径不可挂起而用 `if false &&` 禁用）。

## NeoZygisk denylist 日志判读

NeoZygisk 日志中的 `is on the denylist` 只表示该进程会执行 denylist 挂载还原/隐藏处理，不等同于
Vector 注入被跳过，也不得单独作为注入失败原因。排查注入失败时应继续查看后续模块加载日志，
例如 `dlopen_ext`、Zygisk module ELF 读取、`preAppSpecialize`、`postAppSpecialize`、
`VectorNative` 初始化和 framework dex 加载结果。

## simpleperf 在目标设备上禁用

在当前 `adb` 连接的设备上（**不固定机型**，以 `adb devices` 实测为准）**禁止使用 simpleperf
采样**（无论系统级 `record -a` 还是进程级）：实测 2026-09-05 两次系统级采样
（`simpleperf record -a -g -e cpu-clock -f 4000`）均在采样窗口内引发**设备自发重启**，且重启
与 Vector 注入与否无关。该内核 + wxshadow KPM 组合对 perf_event/NMI 路径脆弱（与 rustFrida
历史提交 0d9b7e3 临时禁用同页 LDR literal relocate 时代排查的重启问题同源）。

> 该结论是在一台运行 wxshadow KPM 的设备上得出的。换设备后若要在新机器上重新评估
> perf 可用性，按第 3 条先做卸载态验证，不得直接沿用“已禁用”或“已可用”的预设。

1. 排查 CPU/卡顿问题时不得再尝试 simpleperf；也不要向 AGENTS 之外的文档推荐该工具。
2. 替代手段：`/proc/<pid>/task/<tid>/stat` 的 utime/stime（用户态/内核态 CPU 占比）、
   `/proc/<pid>/task/<tid>/syscall`、`wchan`、`/proc/<pid>/task/<tid>/stack`（root）、
   ANR trace（`/data/anr/`）的 schedstat、MIUIScout HANG 日志。
3. 若未来确需 perf，必须先在 wxshadow KPM 卸载/禁用状态下单独验证 perf_event 路径不再
   引发重启，再考虑恢复使用。

## xff/com.demo.addapp 启动卡顿根因（wxshadow shadow 页 read/exec 翻转风暴）

### 结论（2026-09-05 锁定，铁证级）

Vector 与 rustFrida **都会**让 com.xff.launch / com.demo.addapp 启动卡 ~10 秒，根因不是
Vector 特有逻辑，而是「wxshadow patch libart 热桥 + GC 同步 hook」这一组合的固有缺陷，
两者实现机制相同，因此症状一致。

**卡点（xff 日志铁证，多次复现）：**
```text
16:50:38.594  Waiting for a blocking GC ObjectsAllocated
16:50:48.691  WaitForGcToComplete blocked ObjectsAllocated on Background for 10.097s
主线程 backtrace: handleStartActivity → callActivityOnStart →
                   LayoutInflater.inflate / FinalizerReference.add / Reference.<init>
```
阻塞式 GC 10.1s 期间，连 libbacktrace 的 `UnwindThread` 都
`BacktraceCurrent::UnwindThread: Timed out waiting for signal handler` 超时，说明进程在 GC
窗口内几乎完全停滞。

### 完整因果链（每步都有源码/dmesg 证据）

1. rustFrida 或 Vector 用 `Java.setStealth(1)` / wxshadow 模式，把 libart 热桥
   `quick_to_interpreter_bridge`（偏移 `...022320`）、`quick_resolution_trampoline`
   （`...022070`）、DoCall/GC 层（`...be4c48/52bc/5ec8` 等）patch 成 **shadow 页（`--x`
   可执行不可读）**。rustFrida 日志铁证：`ep=0x7888222320`、`artController 初始化完成:
   Layer1=2, Layer2=4, GC=2, OatHeader=active, Fixup=active`。

2. 两者都安装 **GC 同步 hook**（`art_controller.rs:771` 起：`GC CopyingPhase`、
   `GC CollectGarbageInternal`、`GC RunFlipFunction`），回调 `on_gc_sync_leave/enter`
   （`art_controller.rs:1585/1593`）在每次 GC 关键阶段调用 `synchronize_replacement_methods()`
   （`art_controller.rs:1678`），遍历 replacement 方法的 `declaring_class_` / `access_flags_`
   做同步。

3. 这些 `read_volatile`/`write_volatile` 访问的原始 ArtMethod 位于被 shadow 化的 libart 页上
   （`--x` 不可读），每次读触发 read fault → KPM 翻回 original（`r--`）；紧随其后的执行又触发
   exec fault → 翻回 shadow（`--x`）。dmesg 铁证（同一热桥页 `7bcfa22000` 的紧密翻转循环）：
   ```text
   [1670.403744] read fault at 7bcfa22000 → original (r--)
   [1670.403747] exec fault at 7bcfa22320 → shadow (--x)
   [1670.403749] read fault at 7bcfa22000 → original (r--)
   [1670.403751] exec fault at 7bcfa22320 → shadow (--x)   ← 间隔仅 3-4 微秒
   ```

4. 每次翻转都走 KPM `do_page_fault_before_impl`（`get_task_mm` + `spin_lock(global_lock)` +
   链表扫描 `find_page` + `mmput`）。xff 启动进入 onStart 布局 inflate → 大量对象分配 →
   触发 `ObjectsAllocated` 阻塞式 GC，GC 全局暂停窗口内的翻转风暴把一次本应 ~100ms 的 GC
   拖慢到 10 秒。

### 证据要点（勿再被如下错误理论误导）

- **「rustFrida 用 stealth=0 / normal 所以不卡」是错的**：rustFrida 走 wxshadow 时同样
  patch 了 `quick_to_interpreter_bridge` 且 `stealth=1`，地址/语义与 Vector 完全一致。
  早期旧日志（rf_hook_trace.txt）显示的 stealth=0 是另一套（非 wxshadow）路径，不代表当前
  rustFrida 的 wxshadow 行为。
- **Vector 的 4942 万 minor fault（pid 10349，主线程 minflt=49429502、stime=14.88s）与
  rustFrida 的 GC 阻塞 10s（pid 14158）是同一根因的两个表现**：前者卡在 splitPaths→stat()
  的巨量 page touch，后者卡在 GC 的 shadow 页翻转；本质都是 shadow 页 r/x 翻转 + KPM 全局锁
  放大。
- **dmesg 翻转总数（仅 151 条）与「fault 风暴」不矛盾**：KPM 的 `handle_read_fault` /
  `handle_exec_fault` 只在 `ret==0` 成功路径才打印日志，翻转风暴中被 KPM 快速处理/失败的
  fault 不会全部落日志；真正的开销在 `do_page_fault_before_impl` 对**每一次** fault 的
  get_task_mm + global_lock + list scan，而非日志条数。

### 修复方向（治标 vs 治本，见下方专节）

详见「wxshadow 启动卡顿：治标 vs 治本」一章。

## `bad ELF magic: 00000000` 处置

如果日志出现：

```text
dlopen_ext ... /memfd:wwb_so (deleted) ... has bad ELF magic: 00000000
```

按以下顺序处理：

1. 将问题判定为 **Zygisk/NeoZygisk 模块 native ELF 加载失败**，不要先排查 ART layout、
   `completeArtBootstrap()`、WxShadow 或应用启动耗时。该错误发生在 `VectorModule::onLoad()`
   之前，通常意味着 `preAppSpecialize/postAppSpecialize` 根本没有机会执行。
2. 检查设备上的 Vector native 文件，而不是只看文件大小或模块版本：
   ```powershell
   $ErrorActionPreference = 'Stop'
   $adb = 'D:\sdk\platform-tools\adb.exe'
   & $adb shell su -c 'dd if=/data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so bs=1 count=4 2>/dev/null | od -An -tx1'
   & $adb shell su -c 'sha256sum /data/adb/modules/zygisk_vector/zygisk/arm64-v8a.so'
   ```
   有效 ELF 的首四字节必须是 `7f 45 4c 46`。文件全零、首部不是 ELF、或 SHA256 与安装包中
   `lib/arm64-v8a/libzygisk.so.sha256` 不一致，都不能继续用于测试。
3. 不得把只存在于备份目录、但未经 ELF 首部和 SHA256 验证的文件当作“已知可用”回退版本。
   重新使用有效构建产物通过 APatch/模块安装流程安装，不能直接向挂载中的模块文件覆盖写入。
4. 检查 NeoZygisk 是否实际登记了 Vector：
   ```powershell
   & $adb shell su -c 'cat /data/adb/zygisksu/modules_info'
   & $adb shell su -c 'cat /data/adb/neozygisk/module.prop'
   ```
   应看到 `zygisk_vector` 的 64 位条目，以及 `zygote64: injected`。`modules_info` 有条目只能证明
   已登记，不能替代 linker 成功加载证据。
5. 若仍失败，临时停用其它 NeoZygisk 模块逐个隔离，尤其是同时改变 zygote/模块分发链路的模块；
   每次修改都必须递增 Vector `versionCode`，重启后确认没有 `bad ELF magic`，并检查：
   `VectorNative`、`VectorZygiskBridge`、`system_server` 注入和 ART backend 日志。
6. 只有确认 ELF 有效且 Vector native 已成功加载后，才继续分析 bridge 超时、manager 崩溃、
   ART hook 或 WxShadow。不要把该错误归因于 denylist；`is on the denylist` 本身不等于跳过 Vector。

## 安装重启后的启动约束

允许 Codex 在用户要求时执行 Vector 的编译、安装、设备重启，以及重启后的 Vector/管理器状态验收；
这些操作不属于启动普通 App。
只要本轮编译生成了新的 Vector 构建产物，编译、打包、安装、重启和安装结果验收默认视为同一套流程，
必须连续执行；只有用户明确要求“仅编译/不安装”时，才可以停在编译完成处。
禁止 Codex 主动启动任何目标或普通 App（包括 `am start`、`monkey`、点击启动入口或其它等效操作）。
需要采集普通 App 的启动日志时，必须等待用户手动启动目标 App 后再读取最新日志。安装重启验证完成后，
应向用户报告构建版本、安装结果和设备状态，不得顺带启动其它 App。

## 验证纪律：Vector 侧验收禁止同时挂 rustFrida 脚本

当验证目标是 **Vector 框架或 Vector 模块（如 `examples/duckusb`）的等效逻辑**时，
**不得在同一轮同时注入 rustFrida 脚本**（`rfclient -l xxx.js`）。

实证教训（2026-09-14）：duckusb 的 FLAG_DEBUGGABLE 清位实现有缺陷
（日志 `DuckUSB: debuggable spoof: ContextImpl not found`，hook 根本没装上），但同轮
rustFrida 脚本的 `[DBG-FLAG]` 把位清了，xff `jdwp-verdict` 照样 SAFE ——
**脚本掩盖了模块 bug**，“验证通过”是假的。

规则：

1. 验证模块等效性 = 纯 Vector + 模块 + KPM 链路：不 spawn、不 attach、不挂脚本。
2. 目标 App 由用户手动启动（见《安装重启后的启动约束》），日志只看模块自身 TAG
   （如 `DuckUSB`）与检测器 TAG（如 `AdbDbgProp` 的 `jdwp-verdict`/`adbd-runtime`）。
3. 每条判据必须能指回**模块自己的日志行**（如
   `debuggableSpoof[...] flags 0x…46 → 0x…44`），而不是脚本的 SPOOF/DBG 行。
4. rustFrida 脚本只用于：建立对照基准、或模块尚未实现该能力时的临时兜底；
   一旦宣称“等效已实现”，验收轮必须剥离脚本重新跑。

## 模块 APK 更新无需重启手机（仅 Vector 框架需要）

Vector 框架本体（zygisk / daemon / xposed 框架 jar / native core）更新后才需要重启手机；
**模块 APK（`examples/*`、被注入的宿主模块）更新不需要重启手机**，也不需要软重启：

1. Vector 的模块注入是**按进程 fork 时加载**：模块 APK 的新 dex/so 只会在下一次该 app
   进程被 fork 时生效。装完模块 APK 后，只需 **force-stop 目标 app（或重新打开它）**，
   新版本就生效。
2. 框架侧（daemon / 管理器）会在模块 APK 更新后自动重扫模块目录，不需要重启
   Vector 或系统。
3. 例外：改了框架侧机制（如 daemon 的模块发现、soLoader、native_init 装载、管理器
   binder 时序、scope 解析逻辑）时，才需要重启手机——那是框架更新，不是模块更新。

实操判据：本轮构建产物是**模块 APK**（如 `examples/duckusb`），装完只需 force-stop
目标 app；本轮构建产物是 **Vector 框架包**（`zygisk_vector` 模块 zip），必须走
「安装 + 重启手机 + 状态验收」全套流程。

## 模块 APK 必须使用非 debuggable 构建（release + debug 签名）

被 Vector 注入的模块 APK（如 `examples/native-hook-demo`）是"被加载方"：宿主的每个进程
fork 时都要重新加载它一遍。Android 规定 `debuggable=true` 的 APK 永远 `run-from-apk`
（系统拒绝 dex2oat 编译，`cmd package compile` 也会返回 Success 但实际不生成 odex），
导致模块 dex 每次进程启动都要全量解释执行 + 验证 + JIT。

实测后果（2026-09-04，demo v16 debug 版 1.4MB dex）：
- 宿主（com.demo.addapp）启动从 ~300ms 劣化到 5~10s，间歇性 start_timeout 被杀；
- simpleperf 采样宿主主线程：92% CPU 在内核态（`_raw_spin_lock` ~34% + 缺页/VMA ~32% +
  取指异常 hardening ~7%），不是锁死等，是解释执行/JIT 引发的内核风暴；
- 模块 dex 越大越严重；小 dex（几百 KB）时开销无感，大 dex（≥1MB）必须非 debuggable。

### 规则

1. 模块 APK 一律使用 release 构建（非 debuggable），配 `signingConfig = signingConfigs
   .getByName("debug")` 保证本地可安装。参考 `examples/native-hook-demo/build.gradle.kts`
   末尾的 buildTypes.release 配置。
2. 本地迭代可用 debug 构建快速验证功能，但涉及宿主启动性能的最终验证必须切回 release
   并重新安装。
3. release 安装后首次测速前，先触发编译并确认 odex 已生成：
   ```bash
   adb shell cmd package compile -m speed -f <模块包名>
   # 确认不是 run-from-apk（run-from-apk = debuggable，编译被系统忽略）：
   adb shell dumpsys package <模块包名> | grep -E 'arm64.*status'
   # 确认 odex 存在（路径从 pm path 的目录拼接 /oat/arm64/base.odex）：
   adb shell su -c 'ls -la "$(pm path <模块包名> | sed s/package://;s/base.apk/base.odex)"'
   ```
4. 排查"宿主启动慢/注入后变卡"时，先检查所有已启用模块 APK 是否 debuggable
   （dumpsys 看 run-from-apk），再考虑 Vector 框架侧原因。

## wxshadow 启动卡顿：治标 vs 治本

> 背景见「xff/com.demo.addapp 启动卡顿根因」一章。这里是修复路径的决策说明，
> 任何改动前必须先明确是走治标还是治本，两者目标、范围、风险完全不同。

### 治标（降低翻转频率 / 减少 GC 同步触发）

**思路**：不改变 wxshadow 的 shadow 页语义，而是减少「在 GC 密集窗口内触碰 shadow 页」的次数。

具体手段（可叠加）：
1. **减少 GC 同步时机**：不要在每次 GC CopyingPhase / CollectGarbageInternal 都调用
   `synchronize_replacement_methods()`，只在 replacement 方法的 `declaring_class_`（GcRoot）
   真正被移动时才同步。可用跟踪 GcRoot 哨兵替代全量遍历。
2. **缩小 GC hook 覆盖**：只 hook 真正会移动对象的阶段（Copying / Compacting GC），
   对非移动的 Sticky / Young GC 不装 on_leave 同步。
3. **KPM 快速路径**：`do_page_fault_before_impl` 对非 shadow 页的 fault 立即返回
   （当前每次 fault 都 get_task_mm + global_lock + list scan 后才判定），把单个 fault 的
   固定开销降下来。这属于 KPM 侧，改本机源码 `E:/52/mykpm/kpms/wxshadow/` 后走 CI 出包
   （见本文开头《wxshadow KPM 源码位置与构建》）。

**特征**：
- 保留 shadow 反检测（CRC/自校验读到原始字节）能力，不牺牲 stealth 语义。
- 改动局部、风险低，但只能「减轻」卡顿（翻转次数下降，单次翻转开销仍在）。
- 对 GC 密集的 app（xff 这种启动时大量 inflate + finalizer）只能缓解不能根治。

### 治本（libart 热桥不走 shadow，消除 r/x 翻转本源）

**思路**：让 ART 内部 **既要读又要执行** 的热桥（`quick_to_interpreter_bridge`、
`quick_resolution_trampoline` 等）**不用 shadow patch，改用 normal patch（mprotect，页保持
r-x 可读可执行）**，从而从源头消除 read/exec 翻转。

具体手段：
1. 对齐 rustFrida 源码里 `hot_shared_art_entries_enabled_by_default()` 的语义——它仅对
   `stealth_mode()==Recomp` 启用热桥共享路由；非该模式下热桥不应被 eager shadow patch。
2. Vector 侧 `CompleteInitializationImpl` 里 `EnsureInterpreterRouter` /
   `InitializeSharedLibartRouter` 目前**无条件** patch `quick_to_interpreter_bridge` /
   `quick_resolution_trampoline`，应加 `use_shared_jni_nterp_layer1 &&` 保护，让普通
   replacement 不触碰全局热桥。
3. 只对**被实际 hook 的业务方法**保留 shadow（反检测价值仍在），全局共享热桥走 normal。

**特征**：
- 从源头消除 GC/解释器读热桥时的 fault 翻转，GC 不再被拖慢 → **根治**卡顿。
- 代价：热桥不再有 shadow 隐藏能力（`quick_to_interpreter_bridge` 的原始字节可被进程内
  CRC/自校验读到真实值），对「检测 libart 热桥是否被篡改」的高级反检测**略降防护**。
  但对多数场景（检测的是业务方法/自身 so，不检测 ART 热桥）无影响。
- 改动涉及 ART router 语义，必须严格对照 rustFrida 的 stealth 判定逻辑，不得凭经验改。

### 决策建议

- **默认选治本**：卡顿是「热桥走 shadow」这一设计在 GC 密集场景的必然结果，治标只能缓解，
  且每次只缓解一部分最终还是要回到治本。治本直接消除矛盾根源。
- 改治本时，必须先读 rustFrida `art_controller.rs` 里 `stealth_mode()` /
  `hot_shared_art_entries_enabled_by_default()` 的完整判定，确认哪些路径该走 normal、
  哪些该走 shadow，再动手，避免把反检测语义一起破坏。
- 若产品硬性要求热桥也必须 shadow（高级反检测），则只能走治标 + 接受 GC 密集 app 的残留卡顿。

### 2026-09-11 决策（覆盖“默认选治本”）

用户已选定**方案 B：严格对齐 rustFrida 现状**，且明确未授权修改源码：

1. 热桥层（`quick_to_interpreter_bridge` / `quick_resolution_trampoline` / DoCall /
   配置化共享 JNI·Nterp 路由 / GC·OAT 层）**维持 rustFrida 现行行为**：wxshadow 模式下
   sflag=1（KPM shadow 页）。已逐行核实 rustFrida `prepare_hook_target`：`WxShadow => 1`。
2. 应用（含管理器）启动的卡顿/ANR（主线程 99% kernel + 千万级 minor fault）是“热桥走
   shadow”的已知固有缺陷，**按已知状态保留**，不再作为新缺陷排查为修复驱动。
3. 此决策**冻结**“默认选治本”路径：后续会话不得因 AGENTS 早前“默认选治本”建议自行改热桥
   stealth；任何 sul 法案及热桥 patch 策略的变动都必须先向用户提问获批（准入约束见
   “源码修改与编译的准入约束”一章）。
4. Vector 已对齐、不回退的 lean 收口仍维持：`UseSharedJniNterpLayer1()`（auto 全跳过）与
   `InitializeBootstrapImpl` 的 deferred（等价 lean preinit）。

