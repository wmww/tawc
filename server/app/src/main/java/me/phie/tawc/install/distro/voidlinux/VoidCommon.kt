package me.phie.tawc.install.distro.voidlinux

import me.phie.tawc.install.InstallationMethod
import java.io.IOException

/**
 * Shared helpers for the two Void Linux flavours
 * ([VoidLinuxX86_64], [VoidLinuxAarch64]). Both use xbps; the only real
 * difference between them is the `XBPS_ARCH` value and the bootstrap
 * tarball URL.
 *
 * Slimming policy mirrors `arch/ArchPacmanCommon.kt` adapted to xbps:
 *
 *  - `xbps-remove -RFy` the bootstrap cruft packages right after
 *    `xbps-install -Suy xbps`. xbps tracks every file it owns, so
 *    naively `rm -rf`'ing a tracked subtree (the strategy on the Arch
 *    side) blows up the next upgrade with `failed to remove obsolete
 *    entry` ERRORs. Removing the package itself drops the files AND
 *    the DB record, no warnings.
 *  - `noextract=` directives in `/etc/xbps.d/00-noextract.conf` to
 *    keep future package installs from re-creating docs/locales.
 *  - `ignorepkg=` directives so dep resolution can't pull cruft back
 *    in via an optional/runtime dep.
 *  - Wipe `/var/cache/xbps/` at end of install (xbps's `-Oy` only
 *    removes orphans-of-current-pkg-versions; we never reinstall in
 *    place, so the whole cache is dead weight on NAND).
 *  - Log-line filtering: xbps emits five lines per package fetched
 *    (sig file, sig progress, sig avg-rate, pkg progress, pkg
 *    avg-rate, "verifying RSA signature"); we keep one ("verifying")
 *    so progress is observable, drop the rest.
 *
 * Void's package manager (`xbps`) signs every package with RSA. The
 * public keys ship in the rootfs tarball under `/var/db/xbps/keys/`
 * so no external keyring fetch is needed; `-y` auto-accepts.
 */
internal object VoidCommon {

    /**
     * `noextract=<glob>` directives appended to
     * `/etc/xbps.d/00-noextract.conf`. Matches **package-internal**
     * paths (no leading `./`); xbps applies these on every install
     * transaction so future `xbps-install` calls don't re-create the
     * trees we trim out of the bootstrap. fnmatch with `FNM_PATHNAME`
     * disabled, same as pacman's NoExtract.
     */
    private val NO_EXTRACT_PATTERNS: List<String> = listOf(
        // docs / man pages — no man reader, no doc viewer
        "/usr/share/man/*",
        "/usr/share/info/*",
        "/usr/share/doc/*",
        "/usr/share/gtk-doc/*",
        "/usr/share/help/*",
        // translation .mo files and locale-gen inputs. C/POSIX is
        // built into glibc; missing translations fall back to English.
        "/usr/share/locale/*",
        "/usr/share/i18n/locales/*",
        "/usr/share/i18n/charmaps/*",
        // GIR XML sources — runtime GI consumers read .typelib in
        // /usr/lib/girepository-1.0/ instead.
        "/usr/share/gir-1.0/*",
        // zoneinfo extras: leap seconds + ancient (pre-1970) tz data.
        // glibc tz lookups don't use either.
        "/usr/share/zoneinfo-leaps/*",
    )

    /**
     * Bootstrap-installed packages we don't want in a Wayland-only
     * chroot. Removed via `xbps-remove -RFy` (R = recursive deps,
     * F = force) right after the xbps self-upgrade — equivalent to
     * Arch's `pacman -Rdd` cruft pass. xbps refuses to remove a
     * package that's a hard dep of something installed; we only list
     * leaves of the dep graph plus their leaf-only deps.
     *
     * Anything pulled in transitively by `base-devel` / `gtk+3` /
     * `weston` (the packages in [DEFAULT_BASE_PACKAGES]) is left
     * alone — removing those would just see them reinstalled on the
     * next `xbps-install` of the base set.
     */
    private val CRUFT_PACKAGES: List<String> = listOf(
        // init system — there's no PID 1 in a chroot
        "runit", "runit-void",
        // network userland: we use /etc/resolv.conf, no DHCP, no SSH,
        // no firewall, no wireless management. Note: `avahi-libs` is
        // kept (libcups/libpulseaudio/libpipewire link against it via
        // shlib); `iproute2` is kept (parts of base-files reference
        // `ip` in startup scripts that won't run but xbps still tracks
        // the shlib dep).
        "dhcpcd", "iptables", "iputils", "iw", "traceroute", "openssh",
        // filesystem tools — no real block devices to format
        "btrfs-progs", "dosfstools", "e2fsprogs", "f2fs-tools", "xfsprogs",
        // console — chroot has no console keymap; xkb covers Wayland.
        // Note: `libwacom` kept (libinput shlib dep), `pciutils`/`hwids`
        // kept (cheap, libinput uses the HW ID database).
        "kbd",
        // editors / man infrastructure — no interactive shell, no man
        "nvi", "mdocml", "man-pages",
        // X11 dbus launcher — we run Wayland, not X
        "dbus-x11",
        // Void's deprecated-pkg-list tracker — pointless in a chroot
        "removed-packages",
        // mail-stack pulled in transitively by base-container-full's
        // mailx provider — none of the base packages link these
        "perl-Authen-SASL", "perl-Convert-BinHex", "perl-Digest-HMAC",
        "perl-IO-Socket-SSL", "perl-IO-stringy", "perl-MIME-tools",
        "perl-MailTools", "perl-Net-SMTP-SSL", "perl-Net-SSLeay",
        "perl-TimeDate", "perl-URI", "perl-Crypt-URandom",
    )

    /**
     * `ignorepkg=<name>` directives. Defence in depth — once the cruft
     * is removed, this keeps a future install from a deep dep chain
     * pulling the same packages back in. Per `xbps.d(5)`, "if a package
     * depends on an ignored package the dependency is always satisfied,
     * without installing the ignored package."
     */
    private val IGNORE_PACKAGES: List<String> = CRUFT_PACKAGES + listOf(
        // kernel-y stuff that's never in a chroot but a future package
        // (e.g. someone manually `xbps-install firefox`) might pull
        "linux-firmware",
    )

    // Runtime package mirror (xbps repos). The bootstrap-tarball mirror
    // lives at `live/current/` (see VoidSha256Resolver) — the trees are
    // separate on Void's CDN.
    private const val DEFAULT_MIRROR = "https://repo-default.voidlinux.org/current"

    /**
     * Configure the freshly-extracted rootfs:
     *   - DNS via /etc/resolv.conf
     *   - profile.d/00-path.sh so the chroot bash gets a sane PATH
     *   - xbps repo configuration
     *   - `noextract=` and `ignorepkg=` directives
     *
     * 01-tawc.sh (Wayland env) is regenerated on every chroot entry by
     * the install method, so it isn't written here.
     */
    fun configure(
        method: InstallationMethod,
        rootfs: String,
        log: (String) -> Unit,
    ) {
        // Heredoc terminators MUST be at column 0 (per shell `<<EOF`
        // semantics); we build the script line-by-line rather than
        // using `"""...""".trimIndent()`, because the interpolated
        // multi-line variables (noextract / ignorepkg lists) come in
        // unindented and would defeat trimIndent's uniform-strip.
        val script = buildString {
            appendLine("set -eu")
            appendLine("ROOTFS='$rootfs'")
            appendLine("rm -f \"\$ROOTFS/etc/resolv.conf\"")
            appendLine("echo nameserver 8.8.8.8 > \"\$ROOTFS/etc/resolv.conf\"")
            appendLine("mkdir -p \"\$ROOTFS/etc/profile.d\" \"\$ROOTFS/etc/xbps.d\"")

            appendLine("cat > \"\$ROOTFS/etc/profile.d/00-path.sh\" <<'PROF_EOF'")
            appendLine("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
            appendLine("export TMPDIR=/tmp")
            appendLine("export HOME=/root")
            appendLine("PROF_EOF")
            appendLine("chmod 644 \"\$ROOTFS/etc/profile.d/00-path.sh\"")

            appendLine("cat > \"\$ROOTFS/etc/xbps.d/00-repository-main.conf\" <<'REPO_EOF'")
            appendLine("repository=$DEFAULT_MIRROR")
            appendLine("REPO_EOF")

            appendLine("cat > \"\$ROOTFS/etc/xbps.d/00-noextract.conf\" <<'NOEX_EOF'")
            for (p in NO_EXTRACT_PATTERNS) appendLine("noextract=$p")
            appendLine("NOEX_EOF")

            appendLine("cat > \"\$ROOTFS/etc/xbps.d/00-ignorepkg.conf\" <<'IGN_EOF'")
            for (p in IGNORE_PACKAGES) appendLine("ignorepkg=$p")
            appendLine("IGN_EOF")

            appendLine("chmod 644 \"\$ROOTFS/etc/xbps.d/\"*.conf")
            appendLine("echo OK")
        }
        val r = method.runOutside(script) { log("conf: $it") }
        if (!r.ok) {
            throw IOException("Configure failed:\n${r.output}")
        }
    }

    /**
     * `xbps-install -Suy xbps` then `xbps-remove -RFy <cruft>`.
     *
     * The self-upgrade is its own transaction because xbps refuses to
     * upgrade other packages alongside itself — the next invocation
     * needs the new binary on PATH.
     *
     * The cruft-removal pass uses `-RFy`: R = recursive (also remove
     * deps that become orphaned), F = force (remove even if some other
     * package claims a soft dep on us). The list is filtered through
     * `xbps-query` first so passing entries that aren't installed is
     * harmless. `IgnorePkg` (written by [configure]) keeps the same
     * packages from coming back via dep resolution later.
     */
    fun initPackageManager(
        method: InstallationMethod,
        rootfs: String,
        log: (String) -> Unit,
    ) {
        val cruft = CRUFT_PACKAGES.joinToString(" ")
        // Per-package `xbps-remove -Ry` rather than one big `-RFy`:
        //   - `-R` orphan-cleans transitive deps the removed package
        //     was the only consumer of (good).
        //   - skipping `-F` lets xbps refuse to remove a package that
        //     still has reverse deps inside the rootfs (good — without
        //     this we cascaded into avahi-libs / libwacom which keep-
        //     packages still need at shlib level).
        // The `|| ...` swallow keeps a single refusal from killing the
        // whole pass; the package just stays installed.
        val res = method.runInside(
            rootfs,
            """
            export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
            set -e
            xbps-install -Suy xbps
            for pkg in $cruft; do
                if xbps-query "${'$'}pkg" >/dev/null 2>&1; then
                    if xbps-remove -Ry "${'$'}pkg" 2>&1; then
                        echo "removed cruft: ${'$'}pkg"
                    else
                        echo "kept (has reverse deps): ${'$'}pkg"
                    fi
                fi
            done
            """.trimIndent(),
            onLine = filteringLog(log),
        )
        if (!res.ok) {
            throw IOException("xbps self-upgrade / cruft removal failed (exit=${res.exitCode})")
        }
    }

    /**
     * `xbps-install -uy <packages>` then nuke the package cache. The
     * combined `-uy` (full system upgrade) + explicit package list
     * runs as one transaction, closing the version-skew window that
     * `ArchPacmanCommon.installBasePackages` documents.
     *
     * `find /var/cache/xbps -mindepth 1 -delete` (rather than `xbps-remove -Oy`, which
     * only drops orphans-of-current-versions): the install-time cache
     * holds the exact `.xbps` files we just unpacked, ~200 MB. None of
     * the typical follow-up operations actually consult them:
     *   - system upgrade pulls *new* versions, not the cached ones.
     *   - installing a new package fetches a fresh `.xbps` (the cache
     *     entries belong to different packages or older versions).
     *   - the only operation that would hit the cache is a same-version
     *     reinstall (`xbps-install -fy <pkg>`), which is rare and not
     *     worth carrying 200 MB of NAND for.
     * The cache directory itself stays — xbps repopulates it with
     * fresh `.xbps` files on later upgrades, which the user can sweep
     * separately if they care.
     *
     * (`testing/install-test-deps.sh` deliberately does NOT wipe its
     * cache because partial-failure retries of *that* script DO benefit
     * from the same-version cache hit — it's the install-time pass
     * that has nothing to retry.)
     */
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
            set -e
            xbps-install -uy ${packages.joinToString(" ")}
            # find -delete (not rm -rf .../xbps/*) so the glob can't
            # blow past ARG_MAX with hundreds of cached .xbps files;
            # -mindepth 1 keeps the dir for future xbps calls.
            find /var/cache/xbps -mindepth 1 -delete
            """.trimIndent(),
            onLine = filteringLog(log),
        )
        if (!res.ok) {
            throw IOException("xbps base-package install failed (exit=${res.exitCode})")
        }
    }

    val DEFAULT_BASE_PACKAGES: List<String> = listOf(
        "base-devel", "git", "wayland", "wayland-protocols",
        "weston", "gtk+3", "gtk+3-demo",
    )

    /**
     * Wrap [log] to drop xbps's per-package noise. xbps writes
     * progress + average-rate lines for both the `.sig2` and the
     * `.xbps`, plus a "verifying RSA signature" line. The progress
     * lines have ETA / avg rate spam that makes the install log
     * unreadable; we keep "verifying RSA signature..." (one line per
     * package) as the heartbeat so a stalled fetch is still visible.
     *
     * Anything not matching the noise patterns is passed through. The
     * caller still sees all summaries, errors, and unpacking lines.
     */
    private fun filteringLog(log: (String) -> Unit): (String) -> Unit {
        return { line ->
            val trimmed = line.trim()
            val drop =
                // " ETA: 00m12s" / "stalled" progress bars
                NOISE_ETA.containsMatchIn(trimmed) ||
                // "[avg rate: 12MB/s]" rate report
                NOISE_AVG_RATE.containsMatchIn(trimmed) ||
                // ".sig2" signature-blob fetch (60-byte file, no value)
                trimmed.contains(".xbps.sig2:") ||
                // One line per file skipped by NoExtract — adds up to
                // thousands of lines per install since perl/glibc-locales
                // alone have hundreds of man pages.
                trimmed.contains("won't be extracted, it matches a noextract pattern")
            if (!drop) log(line)
        }
    }

    private val NOISE_ETA = Regex("\\bETA:\\s*\\d+")
    private val NOISE_AVG_RATE = Regex("\\[avg rate:")
}
