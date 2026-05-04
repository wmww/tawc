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
    // The install package extracts bootstrap tarballs (.tar.gz, .tar.zst,
    // .tar.xz) entirely in-process: commons-compress reads tar/gzip;
    // zstd-jni decodes zstd; xz-java decodes xz/LZMA. Together this keeps
    // the install path tool-free.
    implementation("org.apache.commons:commons-compress:1.27.1")
    implementation("com.github.luben:zstd-jni:1.5.6-9@aar")
    implementation("org.tukaani:xz:1.10")

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

// Map an Android ABI to the (script flag, builddir name) pair
// `client/build-libxkbcommon` uses. The compositor's build.rs reads
// `libxkbcommon/<builddir>/libxkbcommon.a` for the matching arch.
val xkbForAbi = mapOf(
    "arm64-v8a" to ("aarch64" to "builddir"),
    "x86_64" to ("x86_64" to "builddir-x86_64"),
)

tawcAbis.forEach { abi ->
    val triple = rustTripleFor[abi] ?: error("Unsupported ABI: $abi")
    val capAbi = abi.replaceFirstChar { it.uppercase() }

    // Cross-build the static libxkbcommon the Rust compositor links
    // against. Same shape as buildLibhybris: invokes the host script,
    // skipped when the output artefact already exists.
    val tawcRoot = rootProject.projectDir.parentFile
    val (xkbAbiFlag, xkbBuilddir) = xkbForAbi[abi]
        ?: error("Unsupported ABI for libxkbcommon: $abi")
    val xkbStaticLib = "$tawcRoot/libxkbcommon/$xkbBuilddir/libxkbcommon.a"
    val buildLibxkbcommonTask = tasks.register<Exec>("buildLibxkbcommon$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("bash", "client/build-libxkbcommon", "--abi=$xkbAbiFlag")
        inputs.file("$tawcRoot/client/build-libxkbcommon")
        // Manifest + helper changes must invalidate the cache — otherwise a
        // bumped pin in client/deps.list silently no-ops while the .a stays
        // built against the old commit. See CLAUDE.md "Vendored deps".
        inputs.file("$tawcRoot/client/deps.list")
        inputs.file("$tawcRoot/client/deps-lib.sh")
        outputs.file(xkbStaticLib)
    }

    val buildTask = tasks.register<Exec>("buildRustLibrary$capAbi") {
        dependsOn(buildLibxkbcommonTask)
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

    // Cross-build proot (Termux fork) and stage libproot.so +
    // libproot-loader.so under jniLibs. Same shape as buildLibhybris:
    // invokes the host script, skipped when the output binaries
    // already exist. The script is itself incremental, so iteration
    // is `bash client/build-proot --abi=...` direct; Gradle just
    // makes a fresh checkout's `assembleDebug` self-contained.
    val abiToScriptArg = mapOf("arm64-v8a" to "aarch64", "x86_64" to "x86_64")
    val scriptAbi = abiToScriptArg[abi] ?: error("Unsupported ABI: $abi")
    val prootBin = "$tawcRoot/server/app/src/main/jniLibs/$abi/libproot.so"
    val prootLoader = "$tawcRoot/server/app/src/main/jniLibs/$abi/libproot-loader.so"
    val buildProotTask = tasks.register<Exec>("buildProot$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("bash", "client/build-proot", "--abi=$scriptAbi")
        inputs.file("$tawcRoot/client/build-proot")
        // Pin bumps in client/deps.list must invalidate the cache.
        inputs.file("$tawcRoot/client/deps.list")
        inputs.file("$tawcRoot/client/deps-lib.sh")
        outputs.files(prootBin, prootLoader)
    }
    tasks.named("preBuild") {
        dependsOn(buildProotTask)
    }

    // Cross-build tawcroot (the systrap-based proot replacement) and
    // stage libtawcroot.so under jniLibs. Same shape as buildProot.
    val tawcrootBin = "$tawcRoot/server/app/src/main/jniLibs/$abi/libtawcroot.so"
    val buildTawcrootTask = tasks.register<Exec>("buildTawcroot$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("bash", "tawcroot/build", "--abi=$scriptAbi")
        inputs.file("$tawcRoot/tawcroot/build")
        // Pin bumps in client/deps.list must invalidate the cache (cleat).
        inputs.file("$tawcRoot/client/deps.list")
        inputs.file("$tawcRoot/client/deps-lib.sh")
        outputs.file(tawcrootBin)
    }
    tasks.named("preBuild") {
        dependsOn(buildTawcrootTask)
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
        // The cross-compile script is itself incremental, but Gradle
        // still has to know when to invoke it. Tracked inputs:
        //   - the build script itself
        //   - the dep manifest + helper (so a pin bump invalidates the
        //     cache — otherwise a moved libhybris commit would silently
        //     keep shipping the old .so set; see CLAUDE.md "Vendored deps")
        // The output dir snapshot covers the rest.
        inputs.file("$tawcRoot/client/build-libhybris-aarch64")
        inputs.file("$tawcRoot/client/deps.list")
        inputs.file("$tawcRoot/client/deps-lib.sh")
        outputs.dir(libhybrisInstallDir)
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
        inputs.dir("$libhybrisInstallDir/lib")
        outputs.file(libhybrisAssetFile)
    }

    tasks.named("preBuild") {
        dependsOn(packLibhybrisTask)
    }

    // Cross-compile Xwayland (and its bionic-ported X11 / font / pixman
    // dep tree) and pack as `assets/xwayland/<abi>.tar`. Extracted at
    // runtime by `CompositorService.ensureXwaylandExtracted` into
    // `<filesDir>/xwayland/`, which the compositor then exec()s as the
    // X server child for any X11 client. The cross-compile lives in
    // `client/build-xwayland-aarch64`; see notes/xwayland.md for the
    // full pipeline. Aarch64-only — same reason as libhybris (no
    // emulator support; the bionic-Xwayland piece itself would build,
    // but there's no point shipping it without GPU acceleration).
    val xwaylandAbi = "arm64-v8a"
    val xwaylandInstallDir = "$tawcRoot/build/xwayland-aarch64/install"
    val xwaylandAssetFile = "src/main/assets/xwayland/$xwaylandAbi.tar"

    val buildXwaylandTask = tasks.register<Exec>("buildXwayland") {
        workingDir = tawcRoot
        commandLine("bash", "client/build-xwayland-aarch64")
        // Same incremental story as `buildLibhybris`. Tracked inputs:
        //   - build script
        //   - patches dir (a patch edit must rebuild)
        //   - dep manifest + helper (a pin bump must rebuild)
        inputs.file("$tawcRoot/client/build-xwayland-aarch64")
        inputs.dir("$tawcRoot/xwayland-patches")
        inputs.file("$tawcRoot/client/deps.list")
        inputs.file("$tawcRoot/client/deps-lib.sh")
        outputs.dir(xwaylandInstallDir)
    }

    val packXwaylandTask = tasks.register<Exec>("packXwayland") {
        dependsOn(buildXwaylandTask)
        // Pack only what Xwayland needs at runtime: the binary, the
        // xkbcomp helper it spawns to compile keymaps, the .so deps,
        // and the X11 keyboard data dir (a symlink to
        // xkeyboard-config-2, both included). `--format=ustar` for
        // portability; symlinks are preserved by default. Headers,
        // .a / .la / pkg-config / share/man / share/aclocal etc. are
        // build-only artefacts that we don't need on-device.
        doFirst { mkdir(file(xwaylandAssetFile).parentFile) }
        workingDir = file(xwaylandInstallDir)
        // Excludes:
        //   *.la, *.a — libtool / static archive files; build-only.
        //   pkgconfig — .pc files for downstream cross-compiles only.
        //   python3.14 — xcb-proto's Python module; only needed when
        //                running libxcb-codegen at build time.
        //   cmake — meson-generated CMake module files.
        commandLine("tar", "--format=ustar",
            "--exclude=*.la", "--exclude=*.a",
            "--exclude=pkgconfig", "--exclude=cmake",
            "--exclude=python3.*",
            "-cf", "${project.projectDir}/$xwaylandAssetFile",
            "bin/Xwayland", "bin/xkbcomp",
            "lib",
            "share/X11", "share/xkeyboard-config-2")
        inputs.file("$xwaylandInstallDir/bin/Xwayland")
        inputs.file("$xwaylandInstallDir/bin/xkbcomp")
        inputs.dir("$xwaylandInstallDir/lib")
        inputs.dir("$xwaylandInstallDir/share/xkeyboard-config-2")
        outputs.file(xwaylandAssetFile)
    }

    tasks.named("preBuild") {
        dependsOn(packXwaylandTask)
    }
}
