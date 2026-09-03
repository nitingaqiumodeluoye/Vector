package org.matrix.vector.impl.core

import android.app.Instrumentation
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.res.AssetManager
import android.content.res.Configuration
import android.content.res.Resources
import android.util.DisplayMetrics
import android.util.TypedValue

/** Defines a strongly-typed signature for an executable that requires deoptimization. */
data class TargetExecutable(
    val className: String,
    val methodName: String,
    val params: Array<Class<*>>,
) {
    val isConstructor: Boolean
        get() = methodName == "<init>"

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (javaClass != other?.javaClass) return false
        other as TargetExecutable
        if (className != other.className) return false
        if (methodName != other.methodName) return false
        return params.contentEquals(other.params)
    }

    override fun hashCode(): Int {
        var result = className.hashCode()
        result = 31 * result + methodName.hashCode()
        result = 31 * result + params.contentHashCode()
        return result
    }
}

/** Provides a registry of methods known to inline target framework hooks. */
object VectorInlinedCallers {
    const val KEY_BOOT_IMAGE = "boot_image"
    const val KEY_BOOT_IMAGE_MIUI_RES = "boot_image_miui_res"
    const val KEY_SYSTEM_SERVER = "system_server"

    private val callers = mutableMapOf<String, List<TargetExecutable>>()

    init {
        callers[KEY_BOOT_IMAGE] =
            listOf(
                TargetExecutable(
                    "android.app.Instrumentation",
                    "newApplication",
                    arrayOf(ClassLoader::class.java, String::class.java, Context::class.java),
                ),
                TargetExecutable(
                    "android.app.Instrumentation",
                    "newApplication",
                    arrayOf(ClassLoader::class.java, Context::class.java),
                ),
                TargetExecutable(
                    "android.app.LoadedApk",
                    "makeApplicationInner",
                    arrayOf(
                        Boolean::class.javaPrimitiveType!!,
                        Instrumentation::class.java,
                        Boolean::class.javaPrimitiveType!!,
                    ),
                ),
                TargetExecutable(
                    "android.app.LoadedApk",
                    "makeApplicationInner",
                    arrayOf(Boolean::class.javaPrimitiveType!!, Instrumentation::class.java),
                ),
                TargetExecutable(
                    "android.app.LoadedApk",
                    "makeApplication",
                    arrayOf(Boolean::class.javaPrimitiveType!!, Instrumentation::class.java),
                ),
                TargetExecutable(
                    "android.app.ContextImpl",
                    "getSharedPreferencesPath",
                    arrayOf(String::class.java),
                ),
                // ActivityThread.main 不再列入 boot-image deopt 目标。
                // 历史：3228 曾尝试 deopt main 以让 attach 经 resolution router 触发，实测
                // 结果 deopt 后 attach 仍未触发且出现首启慢。3236 恢复 HookBridge.deoptimizeMethod
                // 对未 hook 方法（对齐 rustFrida deoptimize_method）后，这条 deopt 真正生效，
                // 把 main 强制回解释执行：attach 虽能触发，但 forkCommon 后 main 的解释执行首启
                // 延迟达 7.6s，并频繁触发 start_timeout 被杀（4 连冷启中 2 次）。
                // 结论：attach 的 clone+replace entrypoint 替换在未 deopt 时即可快速触发
                // （实测 8~14ms），deopt main 属有害冗余策略，予以移除。
                //
                // 若后续仍需定位 attach 偶发不触发（如 3235 的 12087），应检查 attach 的
                // entrypoint 是否被 AOT direct-call 绕过，而非 deopt 整个 main。
            )

        callers[KEY_BOOT_IMAGE_MIUI_RES] =
            listOf(
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "init",
                    arrayOf(String::class.java),
                ),
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "updateMiuiImpl",
                    emptyArray(),
                ),
                // Simplified string-based resolution for unavailable classes
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "loadOverlayValue",
                    arrayOf(TypedValue::class.java, Int::class.javaPrimitiveType!!),
                ),
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "getThemeString",
                    arrayOf(CharSequence::class.java),
                ),
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "<init>",
                    arrayOf(ClassLoader::class.java),
                ),
                TargetExecutable("android.content.res.MiuiResources", "<init>", emptyArray()),
                TargetExecutable(
                    "android.content.res.MiuiResources",
                    "<init>",
                    arrayOf(
                        AssetManager::class.java,
                        DisplayMetrics::class.java,
                        Configuration::class.java,
                    ),
                ),
                TargetExecutable(
                    "android.miui.ResourcesManager",
                    "initMiuiResource",
                    arrayOf(Resources::class.java, String::class.java),
                ),
                TargetExecutable(
                    "android.app.LoadedApk",
                    "getResources",
                    arrayOf(Resources::class.java),
                ),
                TargetExecutable(
                    "android.content.res.Resources",
                    "getSystem",
                    arrayOf(Resources::class.java),
                ),
                TargetExecutable(
                    "android.app.ApplicationPackageManager",
                    "getResourcesForApplication",
                    arrayOf(ApplicationInfo::class.java),
                ),
                TargetExecutable(
                    "android.app.ContextImpl",
                    "setResources",
                    arrayOf(Resources::class.java),
                ),
            )

        callers[KEY_SYSTEM_SERVER] = emptyList()
    }

    fun get(where: String): List<TargetExecutable> = callers[where] ?: emptyList()
}
