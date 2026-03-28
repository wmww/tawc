plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "me.phie.tawc"
    compileSdk = 34
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "me.phie.tawc"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = "11"
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
    }
}

// Task to build the Rust native library via cargo-ndk
tasks.register<Exec>("buildRustLibrary") {
    workingDir = file("${rootProject.projectDir}/smithay-android")
    environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
    commandLine(
        "cargo", "ndk",
        "--target", "arm64-v8a",
        "--platform", "29",
        "--",
        "build", "--release"
    )
}

// Copy the built .so into jniLibs
tasks.register<Copy>("copyRustLibrary") {
    dependsOn("buildRustLibrary")
    from("${rootProject.projectDir}/smithay-android/target/aarch64-linux-android/release/libsmithay_android.so")
    into("src/main/jniLibs/arm64-v8a/")
}

tasks.named("preBuild") {
    dependsOn("copyRustLibrary")
}
