plugins {
    alias(libs.plugins.agp.app)
}

android {
    namespace = "com.example.lsphello"

    buildFeatures {
        buildConfig = false
    }

    defaultConfig {
        applicationId = "com.example.lsphello"
        versionCode = 1
        versionName = "1.0"
    }
}

// This sample is pure Java; AGP's built-in Kotlin support otherwise adds the Kotlin runtime.
configurations.configureEach {
    exclude(group = "org.jetbrains.kotlin", module = "kotlin-stdlib")
}

dependencies {
    // The framework supplies these classes in the target process.
    compileOnly(project(":xposed"))
}
