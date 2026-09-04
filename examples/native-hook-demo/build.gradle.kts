plugins {
    alias(libs.plugins.agp.app)
}

android {
    namespace = "com.example.nathookdemo"

    defaultConfig {
        applicationId = "com.example.nathookdemo"
        versionCode = 21
        versionName = "1.10.0"
    }

    // Enable AIDL support
    buildFeatures {
        aidl = true
    }

    // Build libvector_nathook_demo.so from src/main/cpp through the repo-wide CMake setup
    // (ndkVersion / abiFilters arm64-v8a / cmake version are injected by the root build.gradle.kts).
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }

    packaging {
        // META-INF/xposed/* must survive packaging verbatim so the daemon can discover
        // the module entry points (module.prop / java_init.list / native_init.list / scope.list).
        resources { merges += "META-INF/xposed/*" }

        // VectorModuleClassLoader.findLibrary() only accepts `apk!/lib/<abi>/<lib>.so` zip
        // entries that are STORED (uncompressed). AGP's default (useLegacyPackaging = false)
        // already stores native libs uncompressed; keep it explicit so a packaging change cannot
        // silently wedge the module's System.loadLibrary.
        jniLibs { useLegacyPackaging = false }
    }
}

dependencies {
    // The framework supplies these classes in the target process (WbModule / WbModuleInterface…).
    compileOnly(project(":xposed"))
}

// release（非 debuggable）构建：debuggable APK 永远 run-from-apk（系统拒绝 dex2oat），
// 模块 dex 每次进程 fork 都要解释执行+验证+JIT，实测把宿主启动拖慢 5~10s（内核锁自旋+
// 缺页风暴）。release 产物可用 debug 签名安装，换取 PM 的 dex2oat 编译资格。
android {
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}
