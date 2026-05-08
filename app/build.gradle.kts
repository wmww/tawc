plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Per-build enabled install methods. Defaults: debug ships all three
// (tawcroot/proot/chroot for dev-loop coverage), release ships only
// tawcroot (the default and only officially supported method —
// chroot/proot are dev-only). Override either side with
// `-PtawcMethods=tawcroot[,proot[,chroot]]`. tawcroot must always be
// enabled — it's the default for new installs and the fallback for
// uninstalls of legacy slots that recorded a now-disabled method.
val explicitTawcMethods: Set<String>? = (project.findProperty("tawcMethods") as String?)
    ?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() }?.toSet()
val debugMethods: Set<String> = explicitTawcMethods ?: setOf("tawcroot", "proot", "chroot")
val releaseMethods: Set<String> = explicitTawcMethods ?: setOf("tawcroot")
val knownMethods = setOf("tawcroot", "proot", "chroot")
run {
    val unknown = (debugMethods + releaseMethods) - knownMethods
    require(unknown.isEmpty()) { "Unknown tawcMethods: $unknown (allowed: $knownMethods)" }
    require("tawcroot" in debugMethods && "tawcroot" in releaseMethods) {
        "tawcroot must be enabled (got tawcMethods=$explicitTawcMethods)"
    }
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
        getByName("debug") {
            buildConfigField("boolean", "METHOD_TAWCROOT_ENABLED", "${"tawcroot" in debugMethods}")
            buildConfigField("boolean", "METHOD_PROOT_ENABLED",    "${"proot" in debugMethods}")
            buildConfigField("boolean", "METHOD_CHROOT_ENABLED",   "${"chroot" in debugMethods}")
        }
        getByName("release") {
            isMinifyEnabled = false
            buildConfigField("boolean", "METHOD_TAWCROOT_ENABLED", "${"tawcroot" in releaseMethods}")
            buildConfigField("boolean", "METHOD_PROOT_ENABLED",    "${"proot" in releaseMethods}")
            buildConfigField("boolean", "METHOD_CHROOT_ENABLED",   "${"chroot" in releaseMethods}")
        }
    }

    // Drop the proot binaries from any APK that doesn't ship the proot
    // method. The Gradle build still produces them when *some* enabled
    // variant uses proot (the debug variant by default), so the per-
    // variant exclusion is the bit that actually matters for the
    // shipped APK size / surface area.
    androidComponents {
        onVariants { variant ->
            val variantMethods = if (variant.buildType == "release") releaseMethods else debugMethods
            if ("proot" !in variantMethods) {
                variant.packaging.jniLibs.excludes.addAll(
                    "**/libproot.so",
                    "**/libproot-loader.so",
                )
            }
        }
    }

    // BuildConfig.DEBUG gates dev-only paths (e.g. mirror cache plumbing
    // in InstallationService); off by default in AGP 8.
    buildFeatures {
        buildConfig = true
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
// `scripts/build-libxkbcommon.sh` uses. The compositor's build.rs reads
// `deps/libxkbcommon/<builddir>/libxkbcommon.a` for the matching arch.
val xkbForAbi = mapOf(
    "arm64-v8a" to ("aarch64" to "builddir"),
    "x86_64" to ("x86_64" to "builddir-x86_64"),
)

val tawcRootForSmithay = rootProject.projectDir
val smithayCargoToml = "$tawcRootForSmithay/deps/smithay/Cargo.toml"

// Ensure the smithay checkout exists before cargo runs. Cargo's
// `[patch.crates-io] smithay = { path = "../deps/smithay" }` errors
// up front if the dir is missing — so this has to come before the
// per-ABI `buildRustLibrary*` tasks. Pin lives in deps/deps.list;
// the script is just `dep_ensure smithay`.
val setupSmithayTask = tasks.register<Exec>("setupSmithay") {
    workingDir = tawcRootForSmithay
    commandLine("bash", "scripts/setup-smithay.sh")
    inputs.file("$tawcRootForSmithay/scripts/setup-smithay.sh")
    inputs.file("$tawcRootForSmithay/deps/deps.list")
    inputs.file("$tawcRootForSmithay/scripts/lib/deps.sh")
    outputs.file(smithayCargoToml)
}

tawcAbis.forEach { abi ->
    val triple = rustTripleFor[abi] ?: error("Unsupported ABI: $abi")
    val capAbi = abi.replaceFirstChar { it.uppercase() }

    // Cross-build the static libxkbcommon the Rust compositor links
    // against. Same shape as buildLibhybris: invokes the host script,
    // skipped when the output artefact already exists.
    val tawcRoot = rootProject.projectDir
    val (xkbAbiFlag, xkbBuilddir) = xkbForAbi[abi]
        ?: error("Unsupported ABI for libxkbcommon: $abi")
    val xkbStaticLib = "$tawcRoot/deps/libxkbcommon/$xkbBuilddir/libxkbcommon.a"
    val buildLibxkbcommonTask = tasks.register<Exec>("buildLibxkbcommon$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("bash", "scripts/build-libxkbcommon.sh", "--abi=$xkbAbiFlag")
        inputs.file("$tawcRoot/scripts/build-libxkbcommon.sh")
        // Manifest + helper changes must invalidate the cache — otherwise a
        // bumped pin in deps/deps.list silently no-ops while the .a stays
        // built against the old commit. See CLAUDE.md "Vendored deps".
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
        outputs.file(xkbStaticLib)
    }

    val buildTask = tasks.register<Exec>("buildRustLibrary$capAbi") {
        dependsOn(buildLibxkbcommonTask, setupSmithayTask)
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
    // is `bash scripts/build-proot.sh --abi=...` direct; Gradle just
    // makes a fresh checkout's `assembleDebug` self-contained.
    //
    // Skipped entirely when no enabled variant ships the proot method
    // (e.g. `-PtawcMethods=tawcroot` everywhere) — the per-variant
    // packaging exclusion above also drops the staged .so files from
    // any APK that doesn't use them.
    val abiToScriptArg = mapOf("arm64-v8a" to "aarch64", "x86_64" to "x86_64")
    val scriptAbi = abiToScriptArg[abi] ?: error("Unsupported ABI: $abi")
    val anyVariantUsesProot = "proot" in debugMethods || "proot" in releaseMethods
    if (anyVariantUsesProot) {
        val prootBin = "$tawcRoot/app/src/main/jniLibs/$abi/libproot.so"
        val prootLoader = "$tawcRoot/app/src/main/jniLibs/$abi/libproot-loader.so"
        val buildProotTask = tasks.register<Exec>("buildProot$capAbi") {
            workingDir = tawcRoot
            environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
            commandLine("bash", "scripts/build-proot.sh", "--abi=$scriptAbi")
            inputs.file("$tawcRoot/scripts/build-proot.sh")
            // Pin bumps in deps/deps.list must invalidate the cache.
            inputs.file("$tawcRoot/deps/deps.list")
            inputs.file("$tawcRoot/scripts/lib/deps.sh")
            outputs.files(prootBin, prootLoader)
        }
        tasks.named("preBuild") {
            dependsOn(buildProotTask)
        }
    }

    // Cross-build tawcroot (the systrap-based proot replacement) and
    // stage libtawcroot.so under jniLibs. Same shape as buildProot.
    val tawcrootBin = "$tawcRoot/app/src/main/jniLibs/$abi/libtawcroot.so"
    val buildTawcrootTask = tasks.register<Exec>("buildTawcroot$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("bash", "tawcroot/build", "--abi=$scriptAbi")
        inputs.file("$tawcRoot/tawcroot/build")
        // Source + header changes must invalidate the cache. Without this,
        // adding a new .c file (and listing it in `tawcroot/build`) is the
        // only kind of edit that gets noticed — pure source/header edits
        // are silently dropped, leaving a stale binary in jniLibs. The
        // chroot.c regression (added in commit 4244bbb but not rebuilt
        // for aarch64 until this fix) was exactly that.
        inputs.dir("$tawcRoot/tawcroot/src")
        inputs.dir("$tawcRoot/tawcroot/include")
        // Pin bumps in deps/deps.list must invalidate the cache (cleat).
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
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
// The actual cross-compile lives in scripts/build-libhybris.sh so
// it can be run by hand for development. This Gradle task just invokes
// it and packs the result.
if ("arm64-v8a" in tawcAbis) {
    val tawcRoot = rootProject.projectDir
    val libhybrisAbi = "arm64-v8a"
    val libhybrisInstallDir = "$tawcRoot/build/libhybris-aarch64/install/usr/local"
    val libhybrisAssetFile = "src/main/assets/libhybris/$libhybrisAbi.tar"

    val buildLibhybrisTask = tasks.register<Exec>("buildLibhybris") {
        workingDir = tawcRoot
        commandLine("bash", "scripts/build-libhybris.sh")
        // The cross-compile script is itself incremental, but Gradle
        // still has to know when to invoke it. Tracked inputs:
        //   - the build script itself
        //   - the dep manifest + helper (so a pin bump invalidates the
        //     cache — otherwise a moved libhybris commit would silently
        //     keep shipping the old .so set; see CLAUDE.md "Vendored deps")
        // The output dir snapshot covers the rest.
        inputs.file("$tawcRoot/scripts/build-libhybris.sh")
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
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
    // `scripts/build-xwayland.sh`; see notes/xwayland.md for the
    // full pipeline. Aarch64-only — same reason as libhybris (no
    // emulator support; the bionic-Xwayland piece itself would build,
    // but there's no point shipping it without GPU acceleration).
    val xwaylandAbi = "arm64-v8a"
    val xwaylandInstallDir = "$tawcRoot/build/xwayland-aarch64/install"
    val xwaylandAssetFile = "src/main/assets/xwayland/$xwaylandAbi.tar"

    val buildXwaylandTask = tasks.register<Exec>("buildXwayland") {
        workingDir = tawcRoot
        commandLine("bash", "scripts/build-xwayland.sh")
        // Same incremental story as `buildLibhybris`. Tracked inputs:
        //   - build script
        //   - patches dir (a patch edit must rebuild)
        //   - dep manifest + helper (a pin bump must rebuild)
        inputs.file("$tawcRoot/scripts/build-xwayland.sh")
        inputs.dir("$tawcRoot/deps/xwayland-patches")
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
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
