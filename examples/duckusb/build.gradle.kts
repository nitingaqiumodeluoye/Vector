plugins {
    alias(libs.plugins.agp.app)
    // Kotlin itself comes from AGP 9's built-in support — applying
    // org.jetbrains.kotlin.android is an error since AGP 9.0 (the root build declares it with
    // `apply false` to pin the compiler version on the buildscript classpath).
}

// Ported from DuckUSB (github.com/Bouteillepleine/DuckUSB, LSPosed/libxposed API 101) to
// Vector's WbModule API. See README.md for the full list of what changed and why; the short
// version is: different API package names, soLoader instead of the Android linker for the
// native library, and no in-APK native library search path.
android {
    namespace = "com.example.duckusb"

    // IDuckService.aidl — the system_server <-> UI channel, unchanged from upstream.
    buildFeatures { aidl = true }

    // CMake rather than the upstream ndkBuild: the root build injects ndkVersion, the
    // arm64-v8a-only abiFilter and `-Wl,-z,max-page-size=16384` for every native module in
    // this repository, and an Android.mk would opt out of all three.
    externalNativeBuild {
        cmake { path = file("src/main/cpp/CMakeLists.txt") }
    }

    defaultConfig {
        applicationId = "com.example.duckusb"
        versionCode = 12
        versionName = "1.0.11"
    }

    packaging {
        // META-INF/xposed/* must survive packaging verbatim or the daemon cannot discover the
        // module entry points (module.prop / java_init.list / native_init.list / scope.list).
        resources { merges += "META-INF/xposed/*" }

        // The daemon reads lib/arm64-v8a/libvector_duckusb.so straight out of the APK zip and
        // loads it from sealed SharedMemory; STORED keeps that read a plain zip entry.
        jniLibs { useLegacyPackaging = false }
    }
}

dependencies {
    // Framework classes (WbModule / WbModuleInterface / WbInterface…) are supplied by Vector in
    // the target process, never packaged into the module.
    compileOnly(project(":xposed"))

    // The module-app half of the framework service: merges the <applicationId>.WbService
    // provider into this APK and supplies WbServiceHelper/WbService, which is how the UI reads
    // its own scope and shares preferences with the hook. Without the provider, the daemon's
    // ModuleAppService retries the binder delivery three times and then throttles for a minute.
    implementation(project(":services:libxposed-service"))
}

// release (non-debuggable) + debug signing: a debuggable APK is permanently run-from-apk, so
// the system never dex2oats it and every forked process re-verifies and JITs the module dex —
// measured at 5~10s of extra host startup for a dex this size.
android {
    buildTypes {
        release {
            isMinifyEnabled = false
            isShrinkResources = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}
