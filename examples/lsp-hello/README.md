# LSP Hello 示例模块

这是一个 LibXposed/LSP 示例模块。目标应用注入成功后，它会在宿主应用左上角添加 `LSP` 按钮。点击按钮可打开可滚动的 console 日志窗口；日志同时继续输出到 Vector/logcat：

```text
com.example.target hello
```

## 模块代码

入口类位于 `src/main/java/com/example/lsphello/HelloModule.java`，入口注册在：

```text
src/main/resources/META-INF/xposed/java_init.list
```

该目录已经接入 Vector 根 Gradle 工程，并把 LibXposed API 作为编译期依赖。构建：

```powershell
$ErrorActionPreference = 'Stop'
$env:ANDROID_HOME = 'D:\sdk'
$env:ANDROID_SDK_ROOT = 'D:\sdk'
./gradlew.bat :examples:lsp-hello:assembleDebug
```

APK 输出在：

```text
examples/lsp-hello/build/outputs/apk/debug/lsp-hello-debug.apk
```

安装 APK 后，在 Vector Manager 中启用模块并把目标应用加入作用域。查看日志：

```powershell
$ErrorActionPreference = 'Stop'
adb logcat -s LspHello
```

日志窗口保留当前宿主进程最近 300 条模块日志，进程重启后清空。按钮使用宿主窗口的 application panel，不需要悬浮窗权限。

如果只想在每个进程的首个包输出一次，可将回调改为：

```java
if (param.isFirstPackage()) {
    log(Log.INFO, "LspHello", param.getPackageName() + " hello");
}
```
