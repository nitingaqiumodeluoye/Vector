package com.example.duckusb

import android.app.Activity
import android.content.Context
import android.content.SharedPreferences
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.CompoundButton
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import com.example.duckusb.service.DuckServiceClient
import de.wwb.android.wbosed.service.WbService

/**
 * 配置界面。
 *
 * 这是对上游 939 行 Material 界面的**有意精简**移植。保留的是一个读者看懂模块是否工作所需
 * 的东西：框架是否在线、Vector 给它的作用域、所有开关、真实的 Settings 读取值（用来证明模块
 * 从不欺骗自己），以及框架模式的诊断信息。砍掉的是表现层：动态取色、主题切换、滚动位置维护
 * 和 GitHub 更新检查。
 *
 * 全部用原生 framework 控件以代码构建，与本仓库其它 example 一致，因此不依赖 appcompat/material。
 * 上游的 MaterialSwitch 在这里就是 [Switch]。
 *
 * 文案为中文，与 examples/native-hook-demo 的做法一致（字面量写在代码里，不走 R.string）。
 */
class MainActivity : Activity() {

    private lateinit var prefs: SharedPreferences
    private lateinit var statusHolder: LinearLayout
    private lateinit var controlsHolder: LinearLayout
    private lateinit var diagHolder: LinearLayout

    /** 为 true 表示 [prefs] 是框架的远程存储，而不是本地回退文件。 */
    private var usingRemotePrefs = false

    /**
     * 框架 binder 是异步送达的，可能在本 Activity 建好之后才到；那样界面会一直读本地回退
     * 配置、永远显示"未连接"。所以它出现（或消失）时重建相关卡片。
     */
    private val serviceListener: (WbService?) -> Unit = { svc ->
        runOnUiThread {
            if ((svc != null) != usingRemotePrefs) rebuildAll()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = resolvePrefs()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(24), dp(20), dp(28))
            setBackgroundColor(if (isDark()) Color.parseColor("#121212") else Color.WHITE)
        }
        root.addView(header())

        root.addView(sectionLabel("状态"))
        statusHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(statusHolder)

        root.addView(sectionLabel("伪装"))
        controlsHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(controlsHolder)

        root.addView(sectionLabel("诊断"))
        diagHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(diagHolder)

        root.addView(footer())

        setContentView(ScrollView(this).apply {
            addView(root)
            isFillViewport = true
        })
        rebuildAll()
    }

    override fun onStart() {
        super.onStart()
        DuckUsbApp.addListener(serviceListener)
    }

    override fun onStop() {
        DuckUsbApp.removeListener(serviceListener)
        super.onStop()
    }

    /** 就地重建三个动态区块，这样滚动位置能保留。 */
    private fun rebuildAll() {
        prefs = resolvePrefs()
        statusHolder.removeAllViews()
        statusHolder.addView(statusCard())
        controlsHolder.removeAllViews()
        controlsHolder.addView(controlsCard())
        diagHolder.removeAllViews()
        diagHolder.addView(serviceCard())
        diagHolder.addView(readingsCard())
    }

    /**
     * 模块配置存放在框架的远程偏好里 —— hook 在 system_server 和各个作用域进程内就是这样读
     * 配置的。框架缺失时回退到本地文件，保证界面可用；那种状态下本来也没有任何 hook 生效，
     * 写哪里都一样。
     */
    private fun resolvePrefs(): SharedPreferences {
        DuckUsbApp.service?.let { svc ->
            runCatching { svc.getRemotePreferences(Config.PREFS_NAME) }.getOrNull()?.let { remote ->
                usingRemotePrefs = true
                return remote
            }
        }
        usingRemotePrefs = false
        return getSharedPreferences(Config.PREFS_NAME, Context.MODE_PRIVATE)
    }

    /** 本模块自己的 Vector 作用域，直接取自框架。 */
    private fun moduleScope(): List<String>? =
        runCatching { DuckUsbApp.service?.scope }.getOrNull()

    // ------------------------------------------------------------------ 各区块

    private fun header(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(0, 0, 0, dp(8))
        addView(textView("DuckUSB", 26f, bold = true))
        addView(textView("对你选定的应用隐藏 USB 调试状态", 13f, muted = true))
    }

    private fun statusCard(): View {
        val svc = DuckUsbApp.service
        val card = card()

        // 这里的"已激活"指框架把 service binder 推给了我们。它比"模块已加载进自己的进程"是
        // 强得多的信号：无论是否在作用域内，模块都会被加载进自己的 App 进程，所以自检证明不了
        // 任何东西。
        row(card, "框架", if (svc != null) "已激活" else "未连接")
        svc?.let {
            row(card, "框架名称", runCatching { it.frameworkName }.getOrNull() ?: "?")
            // getOrNull(), not toString() on the Result: the latter prints "Success(102)".
            row(card, "框架 API", runCatching { it.apiVersion }.getOrNull()?.toString() ?: "?")
        }
        row(card, "配置来源", if (usingRemotePrefs) "框架（远程）" else "本地文件")

        val scope = moduleScope()
        row(card, "作用域", scope?.joinToString("、") ?: "读取失败")
        if (scope != null) {
            // 注意 system_server 在作用域里的写法是 "system"；"android" 不匹配它，那只是普通
            // App 进程的框架包名。两个都认，但提示语要说清哪个才是必需的。
            val hasSystem = scope.any { it == "system" }
            val hasAndroidOnly = !hasSystem && scope.any { it == "android" }
            row(
                card, "已勾选 system",
                when {
                    hasSystem -> "是"
                    hasAndroidOnly -> "只勾了 android —— 框架模式无法工作"
                    else -> "否 —— 框架模式无法工作"
                }
            )
        }
        return card
    }

    private fun controlsCard(): View {
        val card = card()
        card.addView(
            toggle(
                "总暂停",
                "停止所有 hook 逻辑。不会卸载 hook，也不是\"关闭模块\"；真正的开关在 Vector 管理器里。",
                Config.KEY_PAUSED, false
            )
        )
        divider(card)
        card.addView(
            toggle(
                "伪装 USB 调试",
                "adb_enabled / adb_wifi_enabled / 开发者选项。",
                Config.KEY_SPOOF, true
            )
        )
        divider(card)
        card.addView(
            toggle(
                "框架模式",
                "在 system_server 里做服务端伪装。实验性功能，需要勾选 system 并重启。",
                Config.KEY_FRAMEWORK_MODE, false
            )
        )
        divider(card)
        card.addView(
            toggle(
                "单应用 Settings 伪装",
                "在客户端 hook getter，逐个注入到作用域内的应用。",
                Config.KEY_CLIENT_FALLBACK, true
            )
        )
        divider(card)
        card.addView(
            toggle(
                "隐藏通知",
                "抑制常驻的\"USB 调试已启用\"通知。",
                Config.KEY_HIDE_NOTIF, true
            )
        )
        divider(card)
        card.addView(
            toggle(
                "清 FLAG_DEBUGGABLE",
                "作用域内应用读取 applicationInfo.flags 时看不到 debuggable 位（等效 rustFrida 脚本层）",
                Config.KEY_DEBUGGABLE_SPOOF, true
            )
        )
        divider(card)
        card.addView(
            toggle(
                "详细日志",
                "每次注入打一行日志。",
                Config.KEY_VERBOSE_LOG, false
            )
        )
        return card
    }

    private fun serviceCard(): View {
        val card = card()
        val svc = DuckServiceClient.get(this)
        if (svc == null) {
            row(card, "system_server 服务", "未就绪（框架模式未开，或未勾选 system）")
            return card
        }
        val state = runCatching { svc.state }.getOrNull()
        if (state == null) {
            row(card, "system_server 服务", "已就绪，但状态读取失败")
            return card
        }
        val installedAt = state.getLong("installedAtRealtimeMs", 0L)
        val uptime = if (installedAt > 0L) {
            ((state.getLong("nowRealtimeMs", 0L) - installedAt) / 1000L).toString() + " 秒"
        } else "—"
        row(card, "hook 数量", state.getInt("hookCount", 0).toString())
        row(card, "运行时长", uptime)
        row(card, "已暂停", if (state.getBoolean("paused", false)) "是" else "否")
        row(card, "已欺骗的调用方", state.getInt("recordCount", 0).toString())

        val records = runCatching { svc.records }.getOrNull().orEmpty()
        if (records.isNotEmpty()) {
            card.addView(textView("", 6f))
            for (r in records.take(20)) {
                val label = r.getString("label", "unknown")
                val uid = r.getInt("uid", -1)
                val count = r.getInt("count", 0)
                val keys = r.getString("keys", "")
                card.addView(textView("$label ($uid)：$count 次 —— $keys", 12f, muted = true))
            }
        }
        return card
    }

    /**
     * 真实设备状态，用普通公开 API 读取。模块有意从不伪装自己的进程（见
     * DuckUsbModule.onPackageReady 与 native 侧的 should_skip_hooks），所以这张卡显示的是
     * 真相 —— 这是区分"hook 生效"和"作用域配错"的唯一办法。
     */
    private fun readingsCard(): View {
        val card = card()
        val cr = contentResolver
        row(card, "adb_enabled", Settings.Global.getInt(cr, "adb_enabled", -1).toString())
        row(card, "adb_wifi_enabled", Settings.Global.getInt(cr, "adb_wifi_enabled", -1).toString())
        row(
            card, "development_settings_enabled",
            Settings.Global.getInt(cr, "development_settings_enabled", -1).toString()
        )
        card.addView(
            textView(
                "这三个值是在本进程读的，模块从不伪装本进程。可以拿它和\"作用域内的应用看到的值\"对比。",
                11.5f, muted = true
            )
        )
        return card
    }

    private fun footer(): View = textView(
        "本模块不锁定作用域，可在 Vector 管理器里直接勾选要保护的应用（system 与 " +
            "com.android.systemui 已作为推荐项高亮）。详见 README.md。",
        11.5f, muted = true
    ).apply { setPadding(0, dp(16), 0, 0) }

    // ------------------------------------------------------------------ 小控件构造

    private fun card(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(16), dp(12), dp(16), dp(12))
        setBackgroundColor(if (isDark()) Color.parseColor("#1E1E1E") else Color.parseColor("#F5F5F5"))
    }

    private fun divider(parent: LinearLayout) {
        parent.addView(View(this).apply {
            setBackgroundColor(if (isDark()) Color.parseColor("#333333") else Color.parseColor("#DDDDDD"))
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1))
        })
    }

    /**
     * 标签左、值右的一行。
     *
     * 值太长时改为上下两行（标签在上、值在下、左对齐）：两列布局里长值会把标签挤成碎行，
     * 诊断区的"system_server 服务"就撞上了这个，显示成三行断字。20 字符是实测出的分界。
     */
    private fun row(parent: LinearLayout, label: String, value: String) {
        if (value.length > 20) {
            parent.addView(LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(0, dp(6), 0, dp(6))
                addView(textView(label, 14f))
                addView(textView(value, 13f, muted = true))
            })
            return
        }
        parent.addView(LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(6), 0, dp(6))
            addView(textView(label, 14f).apply {
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            })
            addView(textView(value, 14f, muted = true).apply { gravity = Gravity.END })
        })
    }

    private fun sectionLabel(text: String): View = textView(text, 13f, muted = true).apply {
        setPadding(dp(4), dp(18), 0, dp(6))
    }

    @Suppress("DEPRECATION")
    private fun toggle(
        title: String,
        subtitle: String,
        key: String,
        default: Boolean,
    ): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setPadding(0, dp(10), 0, dp(10))
        addView(LinearLayout(this@MainActivity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            addView(textView(title, 15f, bold = true))
            addView(textView(subtitle, 12f, muted = true))
        })
        val sw = Switch(this@MainActivity).apply {
            isChecked = prefs.getBoolean(key, default)
            setOnCheckedChangeListener { _: CompoundButton, checked: Boolean ->
                prefs.edit()?.putBoolean(key, checked)?.apply()
                // 框架模式的服务端副本是实时推送的，所以无需重启即可生效；其它开关由 hook 每次
                // 调用时从偏好里读取。
                DuckServiceClient.pushConfig(
                    this@MainActivity,
                    prefs.getBoolean(Config.KEY_PAUSED, false),
                    prefs.getBoolean(Config.KEY_SPOOF, true),
                    prefs.getBoolean(Config.KEY_HIDE_NOTIF, true),
                )
                if (key == Config.KEY_FRAMEWORK_MODE) {
                    toast("框架模式已改变 —— 需重启以应用 hook 安装")
                }
            }
        }
        addView(sw)
    }

    private fun textView(text: String, sizeSp: Float, bold: Boolean = false, muted: Boolean = false): TextView =
        TextView(this).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
            setTextColor(
                when {
                    muted && isDark() -> Color.parseColor("#AAAAAA")
                    muted -> Color.parseColor("#666666")
                    isDark() -> Color.WHITE
                    else -> Color.BLACK
                }
            )
            if (bold) typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        }

    private fun isDark(): Boolean =
        (resources.configuration.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK) ==
            android.content.res.Configuration.UI_MODE_NIGHT_YES

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
