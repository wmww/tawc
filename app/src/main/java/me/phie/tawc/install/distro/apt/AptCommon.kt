package me.phie.tawc.install.distro.apt

import me.phie.tawc.install.InstallationMethod
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.ShellDefaults
import java.io.IOException

internal object AptCommon {
    private val PATH_EXCLUDES: List<String> = listOf(
        "/usr/share/doc/*",
        "/usr/share/gtk-doc/*",
        "/usr/share/help/*",
        "/usr/share/info/*",
        "/usr/share/lintian/*",
        "/usr/share/locale/*",
        "/usr/share/man/*",
        "/usr/share/gir-1.0/*",
    )

    private val POST_EXTRACT_PURGE_PATHS: List<String> = listOf(
        "/usr/share/doc",
        "/usr/share/gtk-doc",
        "/usr/share/help",
        "/usr/share/info",
        "/usr/share/lintian",
        "/usr/share/locale",
        "/usr/share/man",
        "/usr/share/gir-1.0",
        "/var/cache/apt/archives",
    )

    // No hostname provider needed here: Debian's `hostname` package is
    // Essential, so debootstrap bases always ship /usr/bin/hostname
    // (arch/void get inetutils in their base lists for this).
    // ca-certificates: debootstrap bases ship without a trust store, so
    // every https client (git, curl, wget) fails until it's installed.
    // Pacman bases get it via pacman→curl→ca-certificates and void via
    // xbps's ca-certificates dependency, so only the apt family lists it.
    // systemd-standalone-*: dbus-daemon (and many other packages) depend on
    // `systemd | systemd-standalone-X | systemd-X`. Without a provider in the
    // install set apt picks the first alternative — full systemd — whose
    // postinst cannot run here: systemd ≥260 requires kernel ≥5.10 and hard-
    // fails with EUNATCH when statx() lacks STATX_MNT_ID (kernel <5.8, e.g.
    // 5.4 phone kernels). The rootfs is systemd-less by design anyway, so
    // seed the standalone providers to steer the resolver away from it.
    val DEFAULT_BASE_PACKAGES: List<String> = listOf(
        "ca-certificates",
        "dbus-x11",
        "libwayland-client0",
        "libwayland-server0",
        "systemd-standalone-sysusers",
        "systemd-standalone-tmpfiles",
    )

    /** Rootfs-relative path of the apt pin file written for [blockedPackages]. */
    private const val PREFERENCES_REL = "etc/apt/preferences.d/00-tawc-blocked"

    /**
     * apt preferences body pinning [packages] to `Pin-Priority: -1`, i.e.
     * "never install this". Needed for packages whose postinst cannot run
     * here and which leave dpkg half-configured behind them; see the
     * Ubuntu flavour's `blockedPackages`.
     */
    fun blockedPackagesBody(packages: List<String>): String = packages.joinToString("\n\n") { pkg ->
        listOf(
            "Package: $pkg",
            "Pin: release *",
            "Pin-Priority: -1",
        ).joinToString("\n")
    }

    fun configure(
        method: InstallationMethod,
        rootfs: String,
        suites: List<String>,
        repoUrl: String,
        signedBy: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
        /** apt `Components:`; Debian is `main`, noble needs `main universe`. */
        components: String = "main",
        /** Packages apt must refuse; see [blockedPackagesBody]. */
        blockedPackages: List<String> = emptyList(),
    ) {
        val effectiveRepoUrl = mirrorProxy?.wrap(repoUrl) ?: repoUrl
        val pathExcludeLines = PATH_EXCLUDES.joinToString("\n") { "path-exclude=$it" }
        val purgeList = POST_EXTRACT_PURGE_PATHS.joinToString(" ") { "\"\$ROOTFS$it\"" }
        val script = buildString {
            appendLine("set -eu")
            appendLine("ROOTFS='$rootfs'")
            appendLine("rm -f \"\$ROOTFS/etc/resolv.conf\"")
            appendLine("echo nameserver 8.8.8.8 > \"\$ROOTFS/etc/resolv.conf\"")
            appendLine("rm -f \"\$ROOTFS/etc/apt/sources.list\"")
            appendLine("mkdir -p \"\$ROOTFS/etc/apt/sources.list.d\" \"\$ROOTFS/etc/apt/apt.conf.d\" \"\$ROOTFS/etc/dpkg/dpkg.cfg.d\" \"\$ROOTFS/etc/profile.d\"")
            appendLine("cat > \"\$ROOTFS/etc/apt/sources.list.d/tawc.sources\" <<'SRC_EOF'")
            appendLine("Types: deb")
            appendLine("URIs: $effectiveRepoUrl")
            // deb822 takes whitespace-separated lists for both fields.
            appendLine("Suites: ${suites.joinToString(" ")}")
            appendLine("Components: $components")
            appendLine("Signed-By: $signedBy")
            appendLine("SRC_EOF")
            // The distro's own source file must go with it: Debian ships
            // `debian.sources`, Ubuntu's base image ships `ubuntu.sources`
            // (pointing at the archive host, which is the *wrong* one for
            // arm64 — that archive lives on `ports`). Leaving either in
            // place means apt keeps fetching from a source we did not
            // configure and did not pin.
            appendLine("rm -f \"\$ROOTFS/etc/apt/sources.list.d/debian.sources\" \"\$ROOTFS/etc/apt/sources.list.d/ubuntu.sources\"")
            if (blockedPackages.isNotEmpty()) {
                appendLine("mkdir -p \"\$ROOTFS/etc/apt/preferences.d\"")
                appendLine("cat > \"\$ROOTFS/$PREFERENCES_REL\" <<'PREF_EOF'")
                appendLine("# tawc: packages that cannot work in this rootfs.")
                appendLine(blockedPackagesBody(blockedPackages))
                appendLine("PREF_EOF")
            }
            appendLine("cat > \"\$ROOTFS/etc/apt/apt.conf.d/90tawc\" <<'APT_EOF'")
            appendLine("APT::Install-Recommends \"0\";")
            appendLine("APT::Install-Suggests \"0\";")
            appendLine("APT::Sandbox::User \"root\";")
            appendLine("Acquire::Languages \"none\";")
            appendLine("Dpkg::Use-Pty \"0\";")
            appendLine("Binary::apt::APT::Keep-Downloaded-Packages \"0\";")
            appendLine("APT::Archives::MaxAge \"0\";")
            appendLine("APT::Update::Post-Invoke-Success { \"rm -f /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/*.deb || true\"; };")
            appendLine("DPkg::Post-Invoke { \"rm -f /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/*.deb || true\"; };")
            appendLine("APT_EOF")
            appendLine("cat > \"\$ROOTFS/etc/dpkg/dpkg.cfg.d/01-tawc-noextract\" <<'DPKG_EOF'")
            appendLine(pathExcludeLines)
            appendLine("DPKG_EOF")
            appendLine("cat > \"\$ROOTFS/etc/profile.d/tawc.sh\" <<'PROFILE_EOF'")
            appendLine("# TAWC apt-family rootfs defaults.")
            appendLine("case \":\${PATH:-}:\" in")
            appendLine("  *:/usr/games:*) ;;")
            appendLine("  *) PATH=\"\${PATH:+\$PATH:}/usr/games\" ;;")
            appendLine("esac")
            appendLine("export PATH")
            appendLine("PROFILE_EOF")
            append(ShellDefaults.configureScript())
            appendLine("rm -rf $purgeList")
            appendLine("mkdir -p \"\$ROOTFS/var/cache/apt/archives/partial\"")
            appendLine("echo OK")
        }
        val r = method.runOutside(script) { log("conf: $it") }
        if (!r.ok) {
            throw IOException("Configure failed:\n${r.output}")
        }
    }

    fun initPackageManager(
        method: InstallationMethod,
        rootfs: String,
        log: (String) -> Unit,
    ) {
        val res = method.runInside(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            export DEBIAN_FRONTEND=noninteractive
            set -e
            apt-get update
            """.trimIndent(),
            onLine = filteringLog(log),
        )
        if (!res.ok) {
            throw IOException("apt-get update failed (exit=${res.exitCode})")
        }
    }

    fun installBasePackages(
        method: InstallationMethod,
        rootfs: String,
        packages: List<String>,
        log: (String) -> Unit,
    ) {
        val res = method.runInside(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            export DEBIAN_FRONTEND=noninteractive
            set -e
            apt-get -y dist-upgrade
            apt-get -y install --no-install-recommends ${packages.joinToString(" ")}
            apt-get clean
            rm -f /var/cache/apt/archives/*.deb /var/cache/apt/archives/partial/*.deb
            """.trimIndent(),
            onLine = filteringLog(log),
        )
        if (!res.ok) {
            throw IOException("apt base-package install failed (exit=${res.exitCode})")
        }
    }

    private fun filteringLog(log: (String) -> Unit): (String) -> Unit = { line ->
        val trimmed = line.trim()
        val drop = trimmed.startsWith("Get:") ||
            trimmed.startsWith("Hit:") ||
            trimmed.startsWith("Ign:") ||
            trimmed.startsWith("Fetched ") ||
            trimmed.matches(Regex("""^\d+% \[.*"""))
        if (!drop) log("apt: $line")
    }
}
