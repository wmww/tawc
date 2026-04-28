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

    // BouncyCastle's three jars (bcpg, bcprov, bcutil) each carry the
    // same MR-jar OSGI manifest at META-INF/versions/9/OSGI-INF/MANIFEST.MF
    // and the Android packager refuses to merge identically-named
    // resources by default. Picking the first is safe — the manifests
    // are OSGi metadata, irrelevant at Android runtime.
    //
    // jniLibs.useLegacyPackaging=true pairs with
    // android:extractNativeLibs="true" in AndroidManifest.xml. AGP 8
    // prefers page-aligned-in-APK loading by default, but our proot
    // binary needs to be a real on-disk executable for execve(2);
    // legacy packaging forces extraction at install time.
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            pickFirsts.add("META-INF/versions/9/OSGI-INF/MANIFEST.MF")
        }
    }
}

dependencies {
    // The install package extracts bootstrap tarballs (.tar.gz, .tar.zst)
    // entirely in-process: commons-compress reads tar/gzip; zstd-jni decodes
    // zstd. Together this keeps the install path tool-free.
    implementation("org.apache.commons:commons-compress:1.27.1")
    implementation("com.github.luben:zstd-jni:1.5.6-9@aar")

    // BouncyCastle: detached-PGP-signature verification of the Arch
    // x86_64 bootstrap tarball before we extract it as root. See
    // notes/installation.md "Bootstrap integrity". `jdk18on` = JDK 1.8
    // and up (matches our `JavaVersion.VERSION_11`); `bcpg` brings the
    // OpenPGP layer, `bcprov` the underlying crypto provider.
    implementation("org.bouncycastle:bcpg-jdk18on:1.78.1")
    implementation("org.bouncycastle:bcprov-jdk18on:1.78.1")

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

// Cross-compile libhybris for aarch64 glibc on the host and pack it
// (with symlinks preserved) as an APK asset. Extracted at runtime by
// CompositorService.ensureLibhybrisExtracted into the app's filesDir
// and symlinked into each chroot rootfs at install time.
//
// Only aarch64 — libhybris is unsupported on the x86_64 emulator
// (notes/emulator.md). If the user runs with `-PtawcAbis=x86_64`
// only, we silently skip libhybris bundling.
//
// The actual cross-compile lives in client/build-libhybris-aarch64 so
// it can be run by hand for development. This Gradle task just invokes
// it and packs the result.
if ("arm64-v8a" in tawcAbis) {
    // The tawc repo root is one level up from the Gradle root (which
    // is `server/`). client/build-* and build/libhybris-* live there.
    val tawcRoot = rootProject.projectDir.parentFile
    val libhybrisAbi = "arm64-v8a"
    val libhybrisInstallDir = "$tawcRoot/build/libhybris-aarch64/install/usr/local"
    val libhybrisAssetFile = "src/main/assets/libhybris/$libhybrisAbi.tar"

    val buildLibhybrisTask = tasks.register<Exec>("buildLibhybris") {
        workingDir = tawcRoot
        commandLine("bash", "client/build-libhybris-aarch64")
    }

    val packLibhybrisTask = tasks.register<Exec>("packLibhybris") {
        dependsOn(buildLibhybrisTask)
        // Use tar(1) on the host because the Android packager strips
        // symlinks from individual assets, but happily ships an opaque
        // .tar that the runtime can untar with symlinks intact. We use
        // `--format=ustar` for portability and chdir into the install
        // tree so paths in the tar are relative ("lib/libhybris-...",
        // not "/home/ai/...").
        doFirst { mkdir(file(libhybrisAssetFile).parentFile) }
        workingDir = file(libhybrisInstallDir)
        // Exclude libtool's `.la` archives and pkg-config metadata —
        // they reference host-side cross-toolchain paths that don't
        // exist on-device and aren't read at runtime.
        commandLine("tar", "--format=ustar",
            "--exclude=*.la", "--exclude=pkgconfig",
            "-cf", "${project.projectDir}/$libhybrisAssetFile", "lib")
    }

    tasks.named("preBuild") {
        dependsOn(packLibhybrisTask)
    }
}
