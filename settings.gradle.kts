enableFeaturePreview("TYPESAFE_PROJECT_ACCESSORS")

pluginManagement {
    repositories {
        // Prefer the local network mirror for faster dependency resolution in China.
        maven { url = uri("https://maven.aliyun.com/repository/gradle-plugin") }
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositoriesMode = RepositoriesMode.FAIL_ON_PROJECT_REPOS
    repositories {
        // Prefer the local network mirror, retaining official repositories as fallback.
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        google()
        mavenCentral()
    }
}

rootProject.name = "Vector"

include(
    ":daemon",
    ":examples:duckusb",
    ":examples:lsp-hello",
    ":examples:native-hook-demo",
    ":external:axml",
    ":external:apache",
    ":hiddenapi:stubs",
    ":hiddenapi:bridge",
    ":legacy",
    ":manager",
    ":manager-ui",
    ":services:libxposed-service",
    ":services:manager-service",
    ":services:daemon-service",
    ":xposed",
    ":zygisk",
)
