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

// Per-build enabled graphics backends. Default: all four unless the
// caller passes a production set (scripts/build-release-apk.sh does).
// Override with
// `-PtawcGraphics=libhybris,libhybris-zink,gfxstream,cpu`.
// Disabling gfxstream removes the Rust kumquat/gfxstream feature,
// skips libgfxstream_backend.so, and drops the Mesa gfxstream-vk
// assets. Disabling both gfxstream and libhybris-zink skips the Mesa
// cross-build entirely. See
// `me.phie.tawc.install.EnabledGraphicsBackends`.
val explicitTawcGraphics: Set<String>? = (project.findProperty("tawcGraphics") as String?)
    ?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() }?.toSet()
val enabledGraphics: Set<String> = explicitTawcGraphics
    ?: setOf("libhybris", "libhybris-zink", "gfxstream", "cpu")
val knownGraphics = setOf("libhybris", "libhybris-zink", "gfxstream", "cpu")
run {
    val unknown = enabledGraphics - knownGraphics
    require(unknown.isEmpty()) { "Unknown tawcGraphics: $unknown (allowed: $knownGraphics)" }
    require(enabledGraphics.isNotEmpty()) {
        "tawcGraphics must enable at least one backend (got empty set)"
    }
}
val libhybrisZinkEnabled: Boolean = "libhybris-zink" in enabledGraphics
val gfxstreamEnabled: Boolean = "gfxstream" in enabledGraphics
val mesaBuildNeeded: Boolean = gfxstreamEnabled || libhybrisZinkEnabled

fun booleanProjectProperty(name: String, default: Boolean): Boolean {
    val raw = project.findProperty(name) as String? ?: return default
    return when (raw.trim().lowercase()) {
        "1", "true", "yes", "on" -> true
        "0", "false", "no", "off" -> false
        else -> error("Invalid $name=$raw (expected true or false)")
    }
}

// Build and package the bionic Xwayland server for every enabled app
// ABI, overrideable for lean builds with `-PtawcXwayland=false`.
val xwaylandRequested: Boolean = booleanProjectProperty("tawcXwayland", true)

// Ship MANAGE_EXTERNAL_STORAGE (the external-storage binds feature,
// notes/external-binds.md)? Default yes; `-PtawcAllFilesAccess=false`
// strips the permission via a build-type manifest overlay for
// distribution channels that can't carry it (Google Play review). The
// app detects the stripped permission at runtime and hides the binds
// UI, so no code changes ride on this flag.
val allFilesAccess: Boolean = booleanProjectProperty("tawcAllFilesAccess", true)

// Build the Rust compositor for one or both Android ABIs and copy the
// resulting .so into jniLibs/. Override the default by setting the
// `tawcAbis` Gradle property: `-PtawcAbis=arm64-v8a` or
// `-PtawcAbis=x86_64` or `-PtawcAbis=arm64-v8a,x86_64`.
val tawcAbis: List<String> = (project.findProperty("tawcAbis") as String?
    ?: "arm64-v8a").split(",").map { it.trim() }.filter { it.isNotEmpty() }
val xwaylandScriptAbiFor = mapOf(
    "arm64-v8a" to "aarch64",
    "x86_64" to "x86_64",
)
val xwaylandPackageAbis: List<String> =
    if (xwaylandRequested) tawcAbis.filter { it in xwaylandScriptAbiFor } else emptyList()
val xwaylandPackaged: Boolean = xwaylandPackageAbis.isNotEmpty()

android {
    namespace = "me.phie.tawc"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "me.phie.tawc"
        minSdk = 29
        targetSdk = 36
        // Plain release counter, single source of truth; see notes/release.md.
        versionName = "1"
        versionCode = versionName!!.toInt()
        ndk {
            abiFilters.addAll(tawcAbis)
        }
    }

    buildTypes {
        getByName("debug") {
            // LogScreenActivity is exported in debug only — the export
            // exists so `am start … --es operationId` from adb can
            // attach to a running op; release keeps it app-internal.
            manifestPlaceholders["logScreenExported"] = "true"
            buildConfigField("boolean", "METHOD_TAWCROOT_ENABLED", "${"tawcroot" in debugMethods}")
            buildConfigField("boolean", "METHOD_PROOT_ENABLED",    "${"proot" in debugMethods}")
            buildConfigField("boolean", "METHOD_CHROOT_ENABLED",   "${"chroot" in debugMethods}")
            buildConfigField("boolean", "GRAPHICS_LIBHYBRIS_ENABLED",      "${"libhybris" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_LIBHYBRIS_ZINK_ENABLED", "${"libhybris-zink" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_GFXSTREAM_ENABLED",      "${"gfxstream" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_CPU_ENABLED",            "${"cpu" in enabledGraphics}")
            buildConfigField("boolean", "XWAYLAND_ENABLED", "$xwaylandPackaged")
            buildConfigField("boolean", "TINT_BUFFERS_BY_TYPE_DEFAULT", "true")
        }
        getByName("release") {
            manifestPlaceholders["logScreenExported"] = "false"
            // Deliberately unminified: R8 would roughly halve the APK
            // (~13 vs ~29 MB, mostly BouncyCastle dex), but obfuscated
            // crash traces need per-release mapping.txt juggling and we
            // don't care about size below ~50 MB. Debugging beats
            // megabytes. If this is ever flipped on, proguard-rules.pro
            // already carries the keep rules R8 needs (zstd-jni JNI,
            // line-number attributes). Native libs also ship unstripped
            // — see packaging {} below.
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            buildConfigField("boolean", "METHOD_TAWCROOT_ENABLED", "${"tawcroot" in releaseMethods}")
            buildConfigField("boolean", "METHOD_PROOT_ENABLED",    "${"proot" in releaseMethods}")
            buildConfigField("boolean", "METHOD_CHROOT_ENABLED",   "${"chroot" in releaseMethods}")
            buildConfigField("boolean", "GRAPHICS_LIBHYBRIS_ENABLED",      "${"libhybris" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_LIBHYBRIS_ZINK_ENABLED", "${"libhybris-zink" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_GFXSTREAM_ENABLED",      "${"gfxstream" in enabledGraphics}")
            buildConfigField("boolean", "GRAPHICS_CPU_ENABLED",            "${"cpu" in enabledGraphics}")
            buildConfigField("boolean", "XWAYLAND_ENABLED", "$xwaylandPackaged")
            buildConfigField("boolean", "TINT_BUFFERS_BY_TYPE_DEFAULT", "false")
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
            if (!xwaylandPackaged) {
                variant.packaging.jniLibs.excludes.addAll(
                    "**/libxwayland.so",
                    "**/libxkbcomp.so",
                    "**/libX11-xcb.so",
                    "**/libX11.so",
                    "**/libXau.so",
                    "**/libXfont2.so",
                    "**/libdrm.so",
                    "**/libffi.so",
                    "**/libfontenc.so",
                    "**/libfreetype.so",
                    "**/libmd.so",
                    "**/libpixman-1.so",
                    "**/libwayland-client.so",
                    "**/libwayland-cursor.so",
                    "**/libwayland-egl.so",
                    "**/libwayland-server.so",
                    "**/libxcb*.so",
                    "**/libxcvt.so",
                    "**/libxkbfile.so",
                    "**/libxshmfence.so",
                )
            }
            if (!gfxstreamEnabled) {
                variant.packaging.jniLibs.excludes.addAll(
                    "**/libgfxstream_backend.so",
                    "**/libc++_shared.so",
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

    // TerminalSessionsTest constructs real TerminalSessions, whose
    // android.os.Handler field must no-op (not throw) on the plain JVM.
    testOptions {
        unitTests.isReturnDefaultValues = true
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
        }
        // Build-type manifests merge with higher priority than main's,
        // so pointing them at the shared `tools:node="remove"` overlay
        // strips MANAGE_EXTERNAL_STORAGE from every variant. Neither
        // build type has a manifest of its own otherwise.
        if (!allFilesAccess) {
            getByName("debug") {
                manifest.srcFile("src/overlays/no-all-files-access/AndroidManifest.xml")
            }
            getByName("release") {
                manifest.srcFile("src/overlays/no-all-files-access/AndroidManifest.xml")
            }
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
            // AGP strips jniLibs by default; keep symbol tables instead
            // so debuggerd emits symbolized native tombstones straight
            // from the device — no hunting for the matching unstripped
            // artifact. Same debugging-beats-megabytes call as the
            // unminified release block above.
            keepDebugSymbols.add("**/*.so")
        }
        resources {
            pickFirsts.add("META-INF/versions/9/OSGI-INF/MANIFEST.MF")
            // bcprov data blobs for classes we never reach (R8 strips the
            // classes but not their java resources): ~1.2 MB of picnic
            // post-quantum matrices + CertPathReviewer message catalogs.
            // The PGP verify path (SignatureVerifier) touches neither.
            excludes.add("org/bouncycastle/pqc/**")
            excludes.add("org/bouncycastle/x509/*.properties")
        }
    }

    if (!xwaylandPackaged) {
        androidResources {
            ignoreAssetsPatterns.add("xwayland")
        }
    }
    if (!gfxstreamEnabled) {
        androidResources {
            ignoreAssetsPatterns.add("mesa-gfxstream")
        }
    }
    if (!libhybrisZinkEnabled) {
        androidResources {
            ignoreAssetsPatterns.add("mesa-zink")
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

    // ShortcutManagerCompat + IconCompat for pinned home-screen
    // shortcuts (EntryShortcuts). Already on the classpath transitively
    // via material; explicit because we compile against it directly.
    implementation("androidx.core:core-ktx:1.15.0")

    // Material Components powers the app's chrome on non-compositor screens:
    // Material3 DayNight theme (auto light/dark), MaterialToolbar with the
    // back-arrow up affordance, and MaterialButton for the accented /
    // destructive button styles. AppCompat is pulled in transitively.
    implementation("com.google.android.material:material:1.12.0")

    // Termux's terminal widget (Apache-2.0): VT emulation + pty spawn
    // (terminal-emulator, pulled in transitively) and the Android View
    // with IME/scroll/selection handling (terminal-view). Vendored from
    // deps/termux-app — see settings.gradle.kts. Used by TerminalActivity.
    implementation(project(":terminal-view"))
    // Termux's extra-keys row (GPLv3 — see termux-extrakeys/build.gradle.kts).
    implementation(project(":termux-extrakeys"))

    // Host-side unit tests (src/test): `./gradlew :app:testDebugUnitTest`.
    // The real org.json artifact shadows the throw-on-use stubs in the
    // mockable android.jar so metadata (de)serialization is testable
    // off-device.
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
}

val checkInputConnectionAudit = tasks.register<Exec>("checkInputConnectionAudit") {
    workingDir = rootProject.projectDir
    commandLine("scripts/check-inputconnection-audit.sh")
    inputs.file("$rootDir/app/build.gradle.kts")
    inputs.file("$rootDir/scripts/check-inputconnection-audit.sh")
}

tasks.named("check") {
    dependsOn(checkInputConnectionAudit)
}

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

// Working-tree fingerprint (HEAD + tracked-edit hash) for the named deps,
// via `ensure-deps.sh --tree-state`. Declared as an input property on every
// task whose artifact embodies dep *sources*, so local edits — and their
// later discard by update-deps.sh — rebuild the artifact instead of letting
// it ship stale. An arg ending in "/" selects every dep whose deps.list
// dest lives under that prefix (no hardcoded list to drift).
fun depTreeState(vararg deps: String): Provider<String> = providers.exec {
    workingDir(rootDir)
    commandLine(listOf("scripts/ensure-deps.sh", "--tree-state") + deps)
}.standardOutput.asText

// Ensure the smithay checkout exists before cargo runs. Cargo's
// `[patch.crates-io] smithay = { path = "../deps/smithay" }` errors
// up front if the dir is missing — so this has to come before the
// per-ABI `buildRustLibrary*` tasks. Pin lives in deps/deps.list.
val setupSmithayTask = tasks.register<Exec>("setupSmithay") {
    workingDir = tawcRootForSmithay
    commandLine("scripts/ensure-deps.sh", "smithay")
    inputs.file("$tawcRootForSmithay/scripts/ensure-deps.sh")
    inputs.file("$tawcRootForSmithay/deps/deps.list")
    inputs.file("$tawcRootForSmithay/scripts/lib/deps.sh")
    outputs.file(smithayCargoToml)
}

// Apply our rutabaga_gfx patches (in particular
// `03-kumquat-server-as-lib.patch`, which exposes the `KumquatBuilder`
// type the compositor links against). The compositor's
// `kumquat_virtio = { path = "../deps/rutabaga_gfx/kumquat/server" }`
// dep resolves at cargo metadata time, so this has to run before
// `buildRustLibrary<Abi>`. Sentinel-gated inside the script, so
// gradle just re-invokes it cheaply when patches change.
val rutabagaPatchSentinel = "$tawcRootForSmithay/deps/rutabaga_gfx/kumquat/server/src/lib.rs"
val setupRutabagaTask = tasks.register<Exec>("setupRutabaga") {
    workingDir = tawcRootForSmithay
    commandLine("scripts/ensure-deps.sh", "--patches", "rutabaga_gfx", "deps/rutabaga-patches/rutabaga_gfx")
    inputs.file("$tawcRootForSmithay/scripts/ensure-deps.sh")
    inputs.dir("$tawcRootForSmithay/deps/rutabaga-patches")
    inputs.file("$tawcRootForSmithay/deps/deps.list")
    inputs.file("$tawcRootForSmithay/scripts/lib/deps.sh")
    // src/lib.rs only exists post-patch (03-kumquat-server-as-lib
    // creates it), so its presence is a valid up-to-date signal for
    // Gradle's incremental tracking.
    outputs.file(rutabagaPatchSentinel)
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
        commandLine("scripts/build-libxkbcommon.sh", "--abi=$xkbAbiFlag")
        inputs.file("$tawcRoot/scripts/build-libxkbcommon.sh")
        // Manifest + helper changes must invalidate the cache — otherwise a
        // bumped pin in deps/deps.list silently no-ops while the .a stays
        // built against the old commit. See AGENTS.md "Vendored deps".
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
        inputs.property("depTreeState", depTreeState("libxkbcommon"))
        outputs.file(xkbStaticLib)
    }

    val buildTask = tasks.register<Exec>("buildRustLibrary$capAbi") {
        dependsOn(buildLibxkbcommonTask, setupSmithayTask)
        if (gfxstreamEnabled) {
            dependsOn(setupRutabagaTask)
        }
        workingDir = file("${rootProject.projectDir}/compositor")
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        val cargoArgs = mutableListOf(
            "cargo", "ndk",
            "--target", abi,
            "--platform", "29",
            "--",
            "build", "--release",
        )
        if (gfxstreamEnabled) {
            cargoArgs += listOf("--features", "gfxstream")
        } else {
            cargoArgs += "--no-default-features"
        }
        commandLine(cargoArgs)
        inputs.files(
            "${rootProject.projectDir}/compositor/Cargo.toml",
            "${rootProject.projectDir}/compositor/Cargo.lock",
            "${rootProject.projectDir}/compositor/build.rs",
            "$tawcRoot/deps/deps.list",
            xkbStaticLib,
        )
        inputs.dir("${rootProject.projectDir}/compositor/src")
        inputs.dir("${rootProject.projectDir}/compositor/protocols")
        inputs.dir("${rootProject.projectDir}/compositor/native")
        inputs.files(fileTree("$tawcRoot/deps/smithay") {
            exclude(".git/**", "target/**")
        })
        inputs.property("gfxstreamEnabled", gfxstreamEnabled)
        if (gfxstreamEnabled) {
            inputs.files(fileTree("$tawcRoot/deps/rutabaga_gfx") {
                exclude(".git/**", "build/**", "target/**")
            })
        }
        outputs.file("${rootProject.projectDir}/compositor/target/$triple/release/libcompositor.so")
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
    // is `scripts/build-proot.sh --abi=...` direct; Gradle just
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
            commandLine("scripts/build-proot.sh", "--abi=$scriptAbi")
            inputs.file("$tawcRoot/scripts/build-proot.sh")
            // Pin bumps in deps/deps.list must invalidate the cache.
            inputs.file("$tawcRoot/deps/deps.list")
            inputs.file("$tawcRoot/scripts/lib/deps.sh")
            inputs.property("depTreeState", depTreeState("proot"))
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
        commandLine("tawcroot/build.sh", "--abi=$scriptAbi")
        inputs.file("$tawcRoot/tawcroot/build.sh")
        // Source + header changes must invalidate the cache. Without this,
        // adding a new .c file (and listing it in `tawcroot/build.sh`) is the
        // only kind of edit that gets noticed — pure source/header edits
        // are silently dropped, leaving a stale binary in jniLibs. The
        // chroot.c regression (added in commit 4244bbb but not rebuilt
        // for aarch64 until this fix) was exactly that.
        inputs.dir("$tawcRoot/tawcroot/src")
        inputs.dir("$tawcRoot/tawcroot/include")
        // Pin bumps in deps/deps.list must invalidate the cache (cleat).
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
        inputs.property("depTreeState", depTreeState("cleat"))
        outputs.file(tawcrootBin)
    }
    tasks.named("preBuild") {
        dependsOn(buildTawcrootTask)
    }

    // Cross-build the ando guest client (static bionic) and stage
    // libando.so under jniLibs. Same shape as buildTawcroot.
    val andoBin = "$tawcRoot/app/src/main/jniLibs/$abi/libando.so"
    val buildAndoTask = tasks.register<Exec>("buildAndo$capAbi") {
        workingDir = tawcRoot
        environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
        commandLine("tawcroot/ando/build.sh", "--abi=$scriptAbi")
        inputs.file("$tawcRoot/tawcroot/ando/build.sh")
        inputs.dir("$tawcRoot/tawcroot/ando/src")
        outputs.file(andoBin)
    }
    tasks.named("preBuild") {
        dependsOn(buildAndoTask)
    }
}

// Cross-build the gfxstream host renderer (libgfxstream_backend.so) for
// every enabled Android ABI and stage it under jniLibs/ alongside
// libcompositor.so when the gfxstream backend is enabled.
//
// When enabled, libcompositor.so links against `gfxstream_backend` via
// the `kumquat_virtio` dep (compositor/Cargo.toml `gfxstream` feature),
// which expects to find the .so at the path in `GFXSTREAM_PATH_RELEASE`.
// The kumquat server itself runs as a thread of the compositor process —
// no separate daemon, no broker plumbing. See notes/gfxstream-bridge.md.
val gfxstreamAbiToScriptArg = mapOf("arm64-v8a" to "aarch64", "x86_64" to "x86_64")
tawcAbis.forEach { abi ->
    val tawcRoot = rootProject.projectDir
    val scriptAbi = gfxstreamAbiToScriptArg[abi] ?: error("Unsupported ABI: $abi")
    val capAbi = abi.replaceFirstChar { it.uppercase() }
    val bridgeJniLibsDir = "$tawcRoot/app/src/main/jniLibs/$abi"
    val gfxstreamLib = "$bridgeJniLibsDir/libgfxstream_backend.so"
    val libcppLib = "$bridgeJniLibsDir/libc++_shared.so"
    val gfxstreamOutDir = "$tawcRoot/build/gfxstream-android-$scriptAbi"

    if (gfxstreamEnabled) {
        val buildGfxstreamBackendTask = tasks.register<Exec>("buildGfxstreamBackend$capAbi") {
            workingDir = tawcRoot
            environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
            commandLine("scripts/build-gfxstream-backend.sh", "--abi=$scriptAbi")
            inputs.file("$tawcRoot/scripts/build-gfxstream-backend.sh")
            inputs.dir("$tawcRoot/deps/gfxstream-patches")
            // Pin bumps in deps/deps.list (gfxstream) must invalidate.
            inputs.file("$tawcRoot/deps/deps.list")
            inputs.file("$tawcRoot/scripts/lib/deps.sh")
            inputs.property("depTreeState", depTreeState("gfxstream"))
            outputs.files(gfxstreamLib, libcppLib)
        }

        // The cargo build needs the .so present *and* its location in
        // `GFXSTREAM_PATH_RELEASE` so rutabaga's build.rs emits the right
        // `-L` / `-l` flags. We extend the existing `buildRustLibrary<Abi>`
        // task (registered above in the per-ABI loop) instead of
        // duplicating the cross-build wrapper.
        tasks.named<Exec>("buildRustLibrary$capAbi") {
            dependsOn(buildGfxstreamBackendTask)
            environment("GFXSTREAM_PATH_RELEASE", gfxstreamOutDir)
            inputs.file("$gfxstreamOutDir/libgfxstream_backend.so")
        }
    } else {
        val deleteGfxstreamBackendTask = tasks.register<Delete>("deleteGfxstreamBackend$capAbi") {
            delete(gfxstreamLib, libcppLib)
        }
        tasks.named("preBuild") {
            dependsOn(deleteGfxstreamBackendTask)
        }
    }
}

// Cross-compile libhybris for aarch64 glibc on the host and pack it
// (with symlinks preserved) as an APK asset. Extracted at runtime by
// CompositorService.ensureLibhybrisExtracted into the app's filesDir
// and copied into each rootfs as real files by LibhybrisInstallProvider.
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
    // build-libhybris.sh now passes `--prefix=/usr/lib/hybris
    // --libdir=/usr/lib/hybris` so all the on-device paths libhybris
    // bakes into its .so files (DT_RUNPATH, PKGLIBDIR, LINKER_PLUGIN_DIR)
    // line up with where [LibhybrisInstallProvider] copies them. The
    // DESTDIR install therefore lays files at
    // `install/usr/lib/hybris/{libfoo.so, libhybris/, gl-shims/}` —
    // pack from there.
    val libhybrisInstallDir = "$tawcRoot/build/libhybris-aarch64/install/usr/lib/hybris"
    val libhybrisAssetFile = "src/main/assets/libhybris/$libhybrisAbi.tar"

    val buildLibhybrisTask = tasks.register<Exec>("buildLibhybris") {
        workingDir = tawcRoot
        commandLine("scripts/build-libhybris.sh")
        // The cross-compile script is itself incremental, but Gradle
        // still has to know when to invoke it. Tracked inputs:
        //   - the build script itself
        //   - the dep manifest + helper (so a pin bump invalidates the
        //     cache — otherwise a moved libhybris commit would silently
        //     keep shipping the old .so set; see AGENTS.md "Vendored deps")
        // The output dir snapshot covers the rest.
        inputs.file("$tawcRoot/scripts/build-libhybris.sh")
        inputs.file("$tawcRoot/deps/deps.list")
        inputs.file("$tawcRoot/scripts/lib/deps.sh")
        inputs.property("depTreeState", depTreeState("libhybris", "android-headers"))
        outputs.dir(libhybrisInstallDir)
    }

    val packLibhybrisTask = tasks.register<Exec>("packLibhybris") {
        dependsOn(buildLibhybrisTask)
        // Use tar(1) on the host because the Android packager strips
        // symlinks from individual assets, but happily ships an opaque
        // .tar that the runtime can untar with symlinks intact. We use
        // `--format=ustar` for portability and chdir into the install
        // tree so paths in the tar are relative.
        //
        // Tar contents are flat (libEGL.so, libhybris/, gl-shims/, …
        // at the tar root) — `CompositorService.ensureLibhybrisExtracted`
        // extracts them into `<filesDir>/libhybris/` and
        // [LibhybrisInstallProvider] walks that dir directly.
        doFirst { mkdir(file(libhybrisAssetFile).parentFile) }
        workingDir = file(libhybrisInstallDir)
        // Exclude:
        //  - `.la`        — libtool archives, reference host paths
        //  - `pkgconfig/` — `.pc` files, also reference host paths
        //  - `include/`   — C headers, not needed at runtime
        //  - `bin/`       — `getprop`/`setprop` utilities, not used
        commandLine("tar", "--format=ustar",
            "--exclude=*.la", "--exclude=pkgconfig",
            "--exclude=include", "--exclude=bin",
            "-cf", "${project.projectDir}/$libhybrisAssetFile", ".")
        inputs.dir(libhybrisInstallDir)
        outputs.file(libhybrisAssetFile)
    }

    tasks.named("preBuild") {
        dependsOn(packLibhybrisTask)
    }
} // end libhybris (arm64-v8a in tawcAbis)

// Cross-build Mesa's chroot-side graphics bits when a Mesa-backed
// backend is enabled:
//   - gfxstream-vk assets for GraphicsBackend.GFXSTREAM
//   - Mesa-Zink tarball for GraphicsBackend.LIBHYBRIS_ZINK
//
// The Mesa source emits the ICD JSON with an arch-suffixed name
// (`gfxstream_vk_icd.aarch64.json` / `gfxstream_vk_icd.x86_64.json`).
// We rename to a single `gfxstream_vk_icd.json` in the asset dir so
// the runtime extractor doesn't need to know the arch.
val mesaGfxstreamAbiToScriptArg = mapOf(
    "arm64-v8a" to ("aarch64" to "aarch64"),
    "x86_64"    to ("x86_64"  to "x86_64"),
)
tawcAbis.forEach { abi ->
    val tawcRoot = rootProject.projectDir
    val (scriptAbi, mesonCpu) = mesaGfxstreamAbiToScriptArg[abi] ?: error("Unsupported ABI: $abi")
    val capAbi = abi.replaceFirstChar { it.uppercase() }
    val mesaInstallRoot = "$tawcRoot/build/mesa-$scriptAbi/install/usr/lib"
    val mesaGfxstreamLib = "$mesaInstallRoot/gfxstream/libvulkan_gfxstream.so"
    val mesaGfxstreamIcd = "$mesaInstallRoot/gfxstream/gfxstream_vk_icd.$mesonCpu.json"
    val mesaGfxstreamAssetDir = "src/main/assets/mesa-gfxstream/$abi"
    // Mesa-Zink tarball (libEGL_mesa.so.0 + libgallium-X.Y.Z.so + libgbm.so.1 +
    // soname symlinks) — see scripts/build-mesa-gfxstream.sh "Mesa-Zink".
    // Consumed by the LIBHYBRIS_ZINK graphics backend; see
    // notes/libhybris-zink.md.
    val mesaZinkTar = "$mesaInstallRoot/mesa-zink-$mesonCpu.tar"
    val mesaZinkAssetDir = "src/main/assets/mesa-zink/$abi"

    val buildMesaGfxstreamTask = if (mesaBuildNeeded) {
        tasks.register<Exec>("buildMesaGfxstream$capAbi") {
            workingDir = tawcRoot
            val args = mutableListOf("bash", "scripts/build-mesa-gfxstream.sh", "--abi=$scriptAbi")
            if (!gfxstreamEnabled) args += "--no-gfxstream"
            if (!libhybrisZinkEnabled) args += "--no-zink"
            commandLine(args)
            // Same incremental-input contract as buildLibhybris:
            //   - the build script
            //   - the patches dir (a patch edit must rebuild)
            //   - dep manifest + helper (a Mesa pin bump must rebuild)
            inputs.file("$tawcRoot/scripts/build-mesa-gfxstream.sh")
            inputs.file("$tawcRoot/scripts/build-host-sysroot.sh")
            inputs.dir("$tawcRoot/deps/mesa-patches")
            inputs.file("$tawcRoot/deps/deps.list")
            inputs.file("$tawcRoot/scripts/lib/deps.sh")
            inputs.property("depTreeState", depTreeState("mesa", "wayland-protocols"))
            inputs.property("gfxstreamEnabled", gfxstreamEnabled)
            inputs.property("libhybrisZinkEnabled", libhybrisZinkEnabled)
            val out = mutableListOf<Any>()
            if (gfxstreamEnabled) out.addAll(listOf(mesaGfxstreamLib, mesaGfxstreamIcd))
            if (libhybrisZinkEnabled) out.add(mesaZinkTar)
            outputs.files(out)
        }
    } else {
        null
    }

    val packMesaGfxstreamTask = if (gfxstreamEnabled) {
        tasks.register<Copy>("packMesaGfxstream$capAbi") {
            dependsOn(buildMesaGfxstreamTask!!)
            // Two raw assets — no symlinks, so no tar wrapper needed (unlike
            // libhybris). Rename the arch-suffixed ICD JSON to a single
            // generic name so the runtime extractor opens the same file on
            // both ABIs.
            into("${project.projectDir}/$mesaGfxstreamAssetDir")
            from(mesaGfxstreamLib)
            from(mesaGfxstreamIcd) {
                rename { "gfxstream_vk_icd.json" }
            }
        }
    } else {
        tasks.register<Delete>("packMesaGfxstream$capAbi") {
            delete("${project.projectDir}/$mesaGfxstreamAssetDir")
        }
    }

    // Skip when libhybris-zink is disabled — no Mesa-Zink build runs, no
    // tar to pack, nothing to ship. Wipe any stale asset from a previous
    // enabled build so the APK doesn't smuggle 22 MB of dead weight.
    val packMesaZinkTask = if (libhybrisZinkEnabled) {
        tasks.register<Copy>("packMesaZink$capAbi") {
            dependsOn(buildMesaGfxstreamTask!!)
            // Tarball with symlinks (libEGL_mesa.so → libEGL_mesa.so.0 → …).
            // Same shape as the libhybris asset; runtime extractor mirrors.
            into("${project.projectDir}/$mesaZinkAssetDir")
            from(mesaZinkTar) {
                rename { "mesa-zink.tar" }
            }
        }
    } else {
        tasks.register<Delete>("packMesaZink$capAbi") {
            delete("${project.projectDir}/$mesaZinkAssetDir")
        }
    }

    tasks.named("preBuild") {
        dependsOn(packMesaGfxstreamTask)
        dependsOn(packMesaZinkTask)
    }
}

if (xwaylandPackaged) {
    val tawcRoot = rootProject.projectDir

    // Cross-compile Xwayland (and its bionic-ported X11 / font / pixman
    // dep tree) and stage the result for the APK. Binaries + DT_NEEDED
    // libs ride in `jniLibs/<abi>/lib*.so` (so they land in
    // `nativeLibraryDir`, the only on-disk place untrusted_app may
    // exec on Android 10+); the XKB data tree is tarred into
    // `assets/xwayland/share.tar` because Xwayland reads it via fopen
    // at the baked-in `-Dxkb_dir` path. Extracted/symlinked at runtime
    // by `CompositorService.ensureXwaylandExtracted` into
    // `<filesDir>/xwayland/`, which the compositor then exec()s as the
    // X server child for any X11 client. The cross-compile lives in
    // `scripts/build-xwayland.sh`; see notes/xwayland.md for the
    // full pipeline.
    val xwaylandBuildTasks = mutableListOf<TaskProvider<Exec>>()
    val xwaylandStageTasks = mutableListOf<TaskProvider<Copy>>()
    val xwaylandInstallDirForAbi = mutableMapOf<String, String>()

    // Stage Xwayland's exec'ables and runtime libs as `lib*.so` files
    // under `jniLibs/<abi>/`. Files in nativeLibraryDir get the
    // `apk_data_file` SELinux type, which untrusted_app *can* exec —
    // unlike `app_data_file` (the type assigned to anything we extract
    // into filesDir), where `execute_no_trans` is denied on Android 10+
    // and would otherwise force us to ship a `magiskpolicy --live` rule
    // via su. Same trick proot already uses (libproot.so).
    //
    // Naming: jniLibs entries must match `lib*.so`, so `Xwayland` →
    // `libxwayland.so` and `xkbcomp` → `libxkbcomp.so`. The Kotlin side
    // creates `<filesDir>/xwayland/bin/{Xwayland,xkbcomp}` symlinks
    // pointing at these so the compositor's existing PATH lookup is
    // unaffected.
    //
    // The build's `lib/` already ships flat `lib*.so` files (no
    // version-suffix symlinks — see scripts/build-xwayland.sh), so they
    // can be copied straight into jniLibs without flattening.
    xwaylandPackageAbis.forEach { abi ->
        val capAbi = abi.replaceFirstChar { it.uppercase() }
        val scriptAbi = xwaylandScriptAbiFor[abi]
            ?: error("Unsupported ABI for Xwayland: $abi")
        val xwaylandInstallDir = "$tawcRoot/build/xwayland-$scriptAbi/install"
        xwaylandInstallDirForAbi[abi] = xwaylandInstallDir

        val buildXwaylandTask = tasks.register<Exec>("buildXwayland$capAbi") {
            workingDir = tawcRoot
            environment("ANDROID_NDK_HOME", "${android.ndkDirectory}")
            commandLine("scripts/build-xwayland.sh", "--abi=$scriptAbi")
            // Same incremental story as `buildLibhybris`. Tracked inputs:
            //   - build script
            //   - patches dir (a patch edit must rebuild)
            //   - dep manifest + helper (a pin bump must rebuild)
            inputs.file("$tawcRoot/scripts/build-xwayland.sh")
            inputs.dir("$tawcRoot/deps/xwayland-patches")
            inputs.file("$tawcRoot/deps/deps.list")
            inputs.file("$tawcRoot/scripts/lib/deps.sh")
            inputs.property("depTreeState", depTreeState("deps/xwayland-src/"))
            outputs.dir(xwaylandInstallDir)
        }
        xwaylandBuildTasks += buildXwaylandTask

        val stageXwaylandJniLibsTask = tasks.register<Copy>("stageXwaylandJniLibs$capAbi") {
            dependsOn(buildXwaylandTask)
            into("${project.projectDir}/src/main/jniLibs/$abi")
            from("$xwaylandInstallDir/bin/Xwayland") { rename { "libxwayland.so" } }
            from("$xwaylandInstallDir/bin/xkbcomp") { rename { "libxkbcomp.so" } }
            from("$xwaylandInstallDir/lib") { include("*.so") }
        }
        xwaylandStageTasks += stageXwaylandJniLibsTask
    }

    // The XKB data files (`share/X11`, `share/xkeyboard-config-2`)
    // can't be flattened into jniLibs — Xwayland reads them via fopen
    // and the files reference each other by relative paths inside the
    // tree. Ship them as a tar asset and extract at runtime as before.
    val xwaylandShareAbi = xwaylandPackageAbis.first()
    val xwaylandShareBuildTask = xwaylandBuildTasks.first()
    val xwaylandShareInstallDir = xwaylandInstallDirForAbi[xwaylandShareAbi]
        ?: error("No Xwayland install dir for $xwaylandShareAbi")
    val xwaylandShareAssetFile = "src/main/assets/xwayland/share.tar"
    val packXwaylandShareTask = tasks.register<Exec>("packXwaylandShare") {
        dependsOn(xwaylandShareBuildTask)
        doFirst { mkdir(file(xwaylandShareAssetFile).parentFile) }
        workingDir = file(xwaylandShareInstallDir)
        commandLine("tar", "--format=ustar",
            "-cf", "${project.projectDir}/$xwaylandShareAssetFile",
            "share/X11", "share/xkeyboard-config-2")
        inputs.dir("$xwaylandShareInstallDir/share/X11")
        inputs.dir("$xwaylandShareInstallDir/share/xkeyboard-config-2")
        outputs.file(xwaylandShareAssetFile)
    }

    tasks.named("preBuild") {
        dependsOn(xwaylandStageTasks)
        dependsOn(packXwaylandShareTask)
    }
}

// assets/xwayland must contain exactly share.tar. Generated files under
// src/main/assets ship silently, so a stale artifact from a retired
// packaging scheme rides along in every APK unnoticed (the pre-jniLibs
// arm64-v8a.tar shipped ~3.6 MB of dead weight for two months). Prune
// anything else before packaging.
val pruneStaleXwaylandAssets = tasks.register<Delete>("pruneStaleXwaylandAssets") {
    delete(fileTree("src/main/assets/xwayland") { exclude("share.tar") })
}
tasks.named("preBuild") {
    dependsOn(pruneStaleXwaylandAssets)
}
