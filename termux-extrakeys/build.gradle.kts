// Termux's extra-keys row (the ESC/TAB/CTRL/arrows toolbar above the
// soft keyboard), compiled straight from the vendored deps/termux-app
// checkout — no termux source is copied into this repo. Only the
// extrakeys widget and its TerminalView glue are cherry-picked; the
// rest of termux-shared (Logger, markwon, guava, NDK code, ...) is not
// built. src/main/java holds a tiny local stand-in for the one
// termux-shared helper the widget needs (ThemeUtils) so the upstream
// dependency chain stays out.
//
// License: these classes are GPLv3-only (termux-shared/LICENSE.md puts
// com/termux/shared/termux/* under GPLv3; the Apache-2.0 exception
// covers only terminal-emulator/terminal-view). Shipping them makes
// distributed APKs subject to GPLv3 even though the tawc sources are
// MIT. See notes/terminal.md.
plugins {
    id("com.android.library")
}

// AGP ignores include/exclude filters on java source sets, so the
// cherry-pick is a Sync into build/ instead of a filtered srcDir.
val syncExtrakeysSources = tasks.register<Sync>("syncExtrakeysSources") {
    from(rootProject.file("deps/termux-app/termux-shared/src/main/java")) {
        include("com/termux/shared/termux/extrakeys/**")
        include("com/termux/shared/termux/terminal/io/TerminalExtraKeys.java")
    }
    into(layout.buildDirectory.dir("termux-extrakeys-src"))
}

android {
    namespace = "com.termux.shared"
    compileSdk = (project.properties["compileSdkVersion"] as String).toInt()

    defaultConfig {
        minSdk = (project.properties["minSdkVersion"] as String).toInt()
    }

    sourceSets["main"].java.srcDir(layout.buildDirectory.dir("termux-extrakeys-src"))
}

// srcDir(Provider) does not reliably carry the producing task, so
// hook the sync in front of the build explicitly.
tasks.named("preBuild") {
    dependsOn(syncExtrakeysSources)
}

dependencies {
    // TerminalExtraKeys' API surfaces TerminalView/TerminalSession.
    api(project(":terminal-view"))
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.annotation:annotation:1.9.0")
}
