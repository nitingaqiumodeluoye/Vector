# AGENTS.md

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
     hook 和缓存刷新等具体实现，必须优先对照 `D:\crack\rsfrida\rustFrida`。
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
   - 如果 `D:\crack\rsfrida\rawVector` 或 `D:\crack\rsfrida\rustFrida` 不存在、关键文件缺失或
     对应逻辑无法定位，不得凭经验继续实现；应先报告缺失的基准和具体阻塞点。

## Vector ART Hook 改造约束

当前项目正在进行 Vector 改造：将原本的 ART hook 实现改为 `D:\crack\rsfrida\rustFrida` 中的 wxshadow 模式 ART hook 实现。

后续涉及 ART hook、WxShadow、ArtMethod、router、native trampoline、inline hook、deopt、原方法调用、callback 参数封装、ARM64 relocator 或相关构建接入的修改时，必须遵守以下规则：

1. **不得凭经验或猜测实现。**
   - 修改前必须先对照 `D:\crack\rsfrida\rustFrida` 中对应的 wxshadow 模式 ART hook 实现。
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
   - 如果 `D:\crack\rsfrida\rustFrida` 不存在、文件缺失、实现路径无法定位或关键函数无法确认，应先报告阻塞点，再请求明确指示。

6. **排障时优先对比原版 rustFrida。**
   - 原版 `D:\crack\rsfrida\rustFrida` 已确认能在目标设备上正常使用。
   - 如果 Vector 移植逻辑在某个环节出现异常、崩溃、hook 不生效、参数/返回值错误、ART layout 推断失败、wxshadow patch 失败或行为不一致，应优先检查 Vector 当前实现与原版 rustFrida 对应逻辑的差异。
   - 排查结论应明确指出问题是来自 Vector 适配层、移植遗漏、构建差异、运行环境差异，还是与原版逻辑不一致。

## 当前 inline hook 基线

下一阶段工作是对齐 `D:\crack\rsfrida\rustFrida` 的 inline hook 行为。开始改动前，先以以下 Vector 当前状态作为基线，随后逐项对照 rustFrida 源实现：

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

当前阶段按原版 `D:\crack\rsfrida\rawVector` 的 linker 加载逻辑对齐，直到用户明确通知恢复
soLoader。在此阶段，模块 native so 由 Android linker 负责加载：模块 `ClassLoader` 可以暴露
APK 内 `lib/<abi>` 搜索路径，模块 Java 代码可以调用 `System.load()` 或
`System.loadLibrary()`，`native_init` 由 linker 已加载的 ELF 导出并返回库加载回调。

1. **当前 linker 基线。**
   - `VectorModuleClassLoader.findLibrary()` 可以返回模块 APK 内对应 ABI 的 zip entry 路径，
     形如 `module.apk!/lib/arm64-v8a/libfoo.so`，行为以 rawVector 的实现为准。
   - `ModuleCode.nativeLibraryDir` 可以作为模块 `ClassLoader` 的 native library search path
     传入，但不得凭经验改成其它目录或独立 loader 路径。
   - `META-INF/xposed/native_init.list` 与 `assets/native_init` 作为 native 库名元数据；模块
     入口 ELF 由 Android linker 加载后，通过导出的 `native_init` 完成初始化。
   - `native_init` 返回的库加载回调可以对 linker 提供的目标库 `handle` 调用
     `dlsym(handle, ...)`，用于定位目标库导出符号；这不等同于加载目标库。
   - 当前阶段不得因为看到 APK、`base.apk` 或 `apk!/lib/<abi>` 的 linker 映射就判定失败；应先
     对照 rawVector 的实际加载时机、`dlopen`/`native_init`/回调日志和目标库 handle。

2. **soLoader 暂停规则。**
   - 暂停调用 Vector 内置 `soLoader`、`ElfLoader::LoadLibraryFd()`、SharedMemory ELF 映射、
     `NativeAPI.loadNativeLibraries()` 作为模块 native so 的实际加载后端。
   - 暂停期间不得把 linker 路径和 soLoader 路径混用，也不得以 soLoader 的匿名/FD-backed
     maps 标准否定当前 linker 测试结果。
   - `NativeAPI.recordNativeEntrypoint()` 仅保留兼容记录语义，不得在 linker 已加载的库上重复
     执行 native_init 或再次加载同名库。
   - 只有用户明确通知恢复 soLoader 后，才恢复原先的 SharedMemory、FD、匿名映射和生命周期
     约束，并重新禁止模块直接走 linker。

3. **native inline hook 能力。**
   - 模块 native so 仍只能使用 Vector 提供的 `NativeAPIEntries` 和 Vector core/agent 的
     inline hook 能力，不得自行引入第二套 inline hook、mprotect fallback 或 Dobby runtime。
   - rawVector linker 加载只改变模块 ELF 的装载后端，不改变 ART hook、inline hook、router、
     trampoline 和原函数调用的 Vector/rustFrida 对齐要求。

4. **恢复 soLoader 后的历史基线。**
   - 恢复通知生效后，模块 native ELF 必须由 daemon 读取到只读 SharedMemory，再由 Vector core
     调用 `ElfLoader::LoadLibraryFd()`；不得向目标进程传递 APK 路径或 zip entry 路径。
   - 恢复后不得使用 `System.load()`、`System.loadLibrary()`、APK zip entry 或文件系统解压路径
     作为模块 native 加载后端；目标进程 maps 中出现模块 APK 或模块 native so 的文件路径可执行
     映射即视为未对齐。

## 本机 Android SDK 环境

本机存在可用 Android SDK：`D:\sdk`。

当前 PowerShell 环境中 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 可能为空，但这不代表本机没有 SDK。执行 Gradle、ADB 或 Android 构建相关命令前，应在同一 PowerShell 会话中显式设置：

```powershell
$ErrorActionPreference = 'Stop'
$env:ANDROID_HOME = 'D:\sdk'
$env:ANDROID_SDK_ROOT = 'D:\sdk'
```

不要仅因为 `ANDROID_HOME` / `ANDROID_SDK_ROOT` 为空就判定 Android SDK 不可用；应先检查 `D:\sdk`。

## 编译验证节奏

进行 Vector 改造或问题修复时，不要每修改一点就立即编译。

应先完成当前问题相关的一组对齐改动，例如：

- 对照原版 rustFrida 找到对应实现；
- 完成 Vector 侧必要的逻辑同步；
- 修正同一问题链路上的调用、头文件、构建脚本或适配层；
- 做完静态检查和引用搜索。

确认这组改动已经收敛后，再一次性执行编译验证。除非用户明确要求快速验证某个单点，或者某个编译错误本身是当前唯一阻塞点，否则避免“边改一点边编译”的低效循环。

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

## NeoZygisk denylist 日志判读

NeoZygisk 日志中的 `is on the denylist` 只表示该进程会执行 denylist 挂载还原/隐藏处理，不等同于
Vector 注入被跳过，也不得单独作为注入失败原因。排查注入失败时应继续查看后续模块加载日志，
例如 `dlopen_ext`、Zygisk module ELF 读取、`preAppSpecialize`、`postAppSpecialize`、
`VectorNative` 初始化和 framework dex 加载结果。

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
