package org.matrix.vector.impl.hookers

import de.wwb.android.wbosed.api.WbInterface
import org.matrix.vector.impl.core.VectorStartup
import org.matrix.vector.impl.di.VectorBootstrap
import org.matrix.vector.nativebridge.HookBridge
import org.matrix.vector.util.Utils

/**
 * Intercepts the early ApplicationThread attachment phase. Triggers the legacy compatibility layer
 * to load modern modules into the process.
 */
object AppAttachHooker : WbInterface.Hooker {
    private fun elapsedUs(startNs: Long): Long = (System.nanoTime() - startNs) / 1_000L

    private inline fun <T> traceAttachStep(label: String, block: () -> T): T {
        val startNs = System.nanoTime()
        Utils.logI("AppAttachHooker step begin: $label")
        return try {
            val result = block()
            Utils.logI("AppAttachHooker step end: $label elapsedUs=${elapsedUs(startNs)}")
            result
        } catch (t: Throwable) {
            Utils.logE("AppAttachHooker step failed: $label elapsedUs=${elapsedUs(startNs)}", t)
            throw t
        }
    }

    override fun intercept(chain: WbInterface.Chain): Any? {
        // Execute the actual attach() method first
        val totalStartNs = System.nanoTime()
        Utils.logI("[startup-trace] ActivityThread.attach intercept begin")
        val result = traceAttachStep("ActivityThread.attach.proceed") { chain.proceed() }
        Utils.logI("[startup-trace] ActivityThread.attach.proceed returned")
        traceAttachStep("markAppAttachCompleted") { VectorStartup.markAppAttachCompleted() }

        // rawVector reaches module loading at this boundary, after the real attach() returns. The
        // full rustFrida-aligned controller is also staged here: unlike rustFrida's paused-child
        // pre-init path, Vector app forkCommon can run concurrently with system activity.
        traceAttachStep("HookBridge.completeArtBootstrap[post-attach]") {
            if (!HookBridge.completeArtBootstrap()) {
                Utils.logE("WxShadow ART controller initialization failed after attach")
            }
        }

        val activityThread = chain.thisObject
        try {
            if (activityThread != null) {
                traceAttachStep("legacy.loadModules") {
                    VectorBootstrap.withLegacy { delegate -> delegate.loadModules(activityThread) }
                }
            } else {
                Utils.logW("AppAttachHooker: ActivityThread.attach thisObject is null")
            }
        } finally {
            traceAttachStep("VectorStartup.completeAppBootstrapAfterAttach") {
                VectorStartup.completeAppBootstrapAfterAttach()
            }
            // rawVector 依赖预挂载 LoadedApk hooks 在 bindApplication 创建 ClassLoader 时派发
            // 首包生命周期（onPackageLoaded 早于 AppComponentFactory 静态初始化）。但在后台
            // dexopt 重编译 boot oat 的设备上，getPackageInfo/LoadedApk.<init>/
            // createOrUpdateClassLoaderLocked 全部是编译代码，烘焙直连绕过入口点 router，
            // 这些 hooks 不会触发（oatdump 实测）。改为无条件 replay：生命周期派发有
            // per-package CAS 幂等保护（VectorLifecycleManager.loadedPackages/readyPackages），
            // 在旧 oat 上 hooks 自然派发在先、replay 落空；在重编译设备上由 replay 兜底，
            // 保证生命周期事件不丢失。replay 只派发生命周期、不重复 loadModules。
            traceAttachStep("VectorStartup.replayFirstPackageLifecycleAfterAttach") {
                VectorStartup.replayFirstPackageLifecycleAfterAttach("ActivityThread.attach")
            }
        }

        Utils.logI("[startup-trace] ActivityThread.attach hook total elapsedUs=${elapsedUs(totalStartNs)}")
        return result
    }
}
