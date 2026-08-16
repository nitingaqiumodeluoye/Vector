# AGENTS.md

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

为了对齐 rustFrida，Vector 模块 APK 只作为 DEX/脚本逻辑输入；模块自带 native so 不得在目标进程中运行时加载。

1. **禁止把模块 APK native lib 暴露给 ClassLoader/linker。**
   - 不得向 `BaseDexClassLoader` 传入 `module.apkPath!/lib/<abi>`。
   - `VectorModuleClassLoader.findLibrary()` 不得返回模块 APK 内的 zip entry 路径。
   - `ModuleCode.nativeLibraryDir` 只能保留为兼容字段，不得作为目标进程 native library search path 使用。

2. **禁止执行模块 native_init。**
   - `META-INF/xposed/native_init.list` 与 `assets/native_init` 只允许作为 legacy metadata 解析或记录。
   - `NativeAPI.recordNativeEntrypoint()` 必须保持 no-op，不得调用 `dlopen`、`dlsym` 或 `RegisterNativeLib()`。
   - daemon 不得为模块 APK 解压/暂存 native so 给 system_server 或普通 app 进程执行。

3. **native inline hook 能力只能来自 Vector core/agent。**
   - 需要测试 native inline hook 时，应在 Vector core 内置 probe 或 native bridge 中实现，然后由 Java/DEX 模块触发。
   - 测试模块不得通过 `System.loadLibrary()` 加载自己的 native so 来提供 hook 逻辑。
   - 若目标进程 maps 中出现模块包名、模块 `base.apk` 或模块 native so 的可执行映射，视为未对齐 rustFrida。

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

## 安装重启验证节奏

当用户要求安装重启验证当前 Vector 构建时，应遵守以下流程：

1. 使用当前构建产物执行安装并重启设备。
2. 设备重新上线后，只确认 Vector/管理器状态是否为 `unknown`；这个状态确认属于安装结果验证，不属于原因排查。
3. 如果状态仍为 `unknown`，不要排查日志、安装结果、模块目录、Zygisk/NeoZygisk、权限标签或运行时崩溃原因；必须直接卸载当前 `zygisk_vector` 模块，重新安装当前构建产物并重启。
4. 上述“确认状态 -> unknown 则卸载 -> 安装 -> 重启”的流程必须循环执行，直到 Vector/管理器状态不再是 `unknown`；不得只执行一次重装就停止。
5. 每一轮安装和重启前后都要记录使用的构建产物、卸载/安装命令、设备上线状态和最终状态结果。
