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
    val DEFAULT_BASE_PACKAGES: List<String> = listOf(
        "ca-certificates",
        "dbus-x11",
        "libwayland-client0",
        "libwayland-server0",
    )

    fun configure(
        method: InstallationMethod,
        rootfs: String,
        suite: String,
        repoUrl: String,
        signedBy: String,
        mirrorProxy: MirrorProxy?,
        log: (String) -> Unit,
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
            appendLine("Suites: $suite")
            appendLine("Components: main")
            appendLine("Signed-By: $signedBy")
            appendLine("SRC_EOF")
            appendLine("rm -f \"\$ROOTFS/etc/apt/sources.list.d/debian.sources\"")
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
