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

dependencies {
    // The install package extracts bootstrap tarballs (.tar.gz, .tar.zst)
    // entirely in-process: commons-compress reads tar/gzip; zstd-jni decodes
    // zstd. Together this keeps the install path tool-free.
    implementation("org.apache.commons:commons-compress:1.27.1")
    implementation("com.github.luben:zstd-jni:1.5.6-9@aar")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // Material Components powers the app's chrome on non-compositor screens:
    // Material3 DayNight theme (auto light/dark), MaterialToolbar with the
    // back-arrow up affordance, and MaterialButton for the accented /
    // destructive button styles. AppCompat is pulled in transitively.
    implementation("com.google.android.material:material:1.12.0")
}

// Build the Rust compositor for one or both Android ABIs and copy the
// resulting .so into jniLibs/. Override the default by setting the
// `tawcAbis` Gradle property: `-PtawcAbis=arm64-v8a` or
// `-PtawcAbis=x86_64` or `-PtawcAbis=arm64-v8a,x86_64`.
val tawcAbis: List<String> = (project.findProperty("tawcAbis") as String?
    ?: "arm64-v8a").split(",").map { it.trim() }.filter { it.isNotEmpty() }

val rustTripleFor = mapOf(
    "arm64-v8a" to "aarch64-linux-android",
    "x86_64" to "x86_64-linux-android",
)

tawcAbis.forEach { abi ->
    val triple = rustTripleFor[abi] ?: error("Unsupported ABI: $abi")
    val capAbi = abi.replaceFirstChar { it.uppercase() }

    val buildTask = tasks.register<Exec>("buildRustLibrary$capAbi") {
        workingDir = file("${rootProject.projectDir}/compositor")
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine(
            "cargo", "ndk",
            "--target", abi,
            "--platform", "29",
            "--",
            "build", "--release"
        )
    }

    val copyTask = tasks.register<Copy>("copyRustLibrary$capAbi") {
        dependsOn(buildTask)
        from("${rootProject.projectDir}/compositor/target/$triple/release/libcompositor.so")
        into("src/main/jniLibs/$abi/")
    }

    tasks.named("preBuild") {
        dependsOn(copyTask)
    }
}
