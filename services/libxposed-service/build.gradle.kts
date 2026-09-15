plugins { alias(libs.plugins.agp.lib) }

// Module-side counterpart of :services:daemon-service.
//
// services/libxposed is an upstream submodule (github.com/libxposed/service) that ships two
// independent halves: the AIDL interface and the client-side helper library. The daemon
// consumes both (it implements IWbService and needs the AIDL), but a *module* only ever needs
// the client half: WbProvider (declared at <applicationId>.WbService), WbServiceHelper and the
// WbService wrapper. Until now there was no build module for that half at all — the sources
// were reachable only through daemon-service's extra java/aidl source directories, which tied
// any consumer to the daemon's own aar.
//
// This module publishes exactly that client half, so an example module can depend on it and
// get three things:
//   - the .WbService ContentProvider merged into its manifest (WbProvider's manifest entry is
//     what makes daemon's ModuleAppService.sendBinder find a receiver at all);
//   - WbServiceHelper, whose registerListener delivers the framework's IWbService binder;
//   - WbService, the typed wrapper over that binder (scope, remote preferences, hot reload...).
//
// The source sets point into the submodule rather than duplicating it, so an upstream bump is
// a submodule update and nothing here changes.
android {
    // The AIDL under services/libxposed/interface compiles into de.wwb.android.wbosed.service.
    buildFeatures { aidl = true }

    buildTypes { release { isMinifyEnabled = false } }

    sourceSets {
        named("main") {
            java.directories.addAll(listOf("src/main/java", "../libxposed/service/src/main"))
            aidl.directories.addAll(
                listOf("src/main/aidl", "../libxposed/interface/src/main/aidl")
            )
        }
    }

    namespace = "de.wwb.android.wbosed.service.lib"
}

dependencies {
    // Annotations only: the helper library never runs inside the framework, so there is no
    // dependency on :xposed, on the daemon artifacts or on hidden API stubs.
    compileOnly(libs.androidx.annotation)
    compileOnly(libs.libxposed.annotation)
}
