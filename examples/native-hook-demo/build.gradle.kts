plugins {
    alias(libs.plugins.agp.app)
}

android {
    namespace = "com.example.nathookdemo"

    defaultConfig {
        applicationId = "com.example.nathookdemo"
        versionCode = 11
        versionName = "1.5.5"
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
