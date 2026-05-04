package me.phie.tawc.install.distro.voidlinux

import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Resolve the latest Void Linux ROOTFS tarball for a given arch by
 * fetching `sha256sum.txt` from `repo-default.voidlinux.org/live/current/`
 * and parsing out the matching line.
 *
 * Void publishes dated rootfs tarballs (`void-x86_64-ROOTFS-20250202.tar.xz`)
 * under a `current/` channel. There's no "latest" symlink to a stable
 * filename, so install-time resolution looks at the freshest manifest.
 *
 * Integrity story is comparable to ManjaroArm's GitHub-Releases path:
 * one trusted HTTPS endpoint hands us a SHA-256, and we verify the
 * downloaded tarball matches before extract. Weaker than PGP (no
 * detached-key chain) but defeats passive MITM and mid-download
 * corruption — see notes/installation.md "Bootstrap integrity".
 */
internal object VoidSha256Resolver {

    private const val MIRROR = "https://repo-default.voidlinux.org/live/current"

    data class Resolved(val downloadUrl: String, val filename: String, val sha256Hex: String)

    /**
     * Look up the latest ROOTFS tarball for [linuxArch] (e.g. `"x86_64"`,
     * `"aarch64"`) — the glibc variant, never musl. Throws [IOException]
     * if the manifest can't be fetched or the matching line is missing.
     */
    fun resolveLatest(linuxArch: String): Resolved {
        val manifest = downloadString("$MIRROR/sha256sum.txt")
        // Lines look like:
        //   SHA256 (void-x86_64-ROOTFS-20250202.tar.xz) = <64 hex>
        // We want the glibc rootfs, i.e. `void-<arch>-ROOTFS-*.tar.xz`,
        // explicitly excluding the `-musl-` variant.
        val pattern = Regex(
            """^SHA256 \((void-${Regex.escape(linuxArch)}-ROOTFS-\d+\.tar\.xz)\) = ([0-9a-f]{64})\s*$""",
            RegexOption.IGNORE_CASE,
        )
        // If multiple dated entries exist, pick the newest (highest
        // date prefix). In practice `current/` only has one rootfs per
        // arch, but be defensive in case Void ever ships a transition.
        val best = manifest.lineSequence()
            .mapNotNull { pattern.find(it.trim()) }
            .maxByOrNull { it.groupValues[1] }
            ?: throw IOException(
                "Void sha256sum.txt has no entry for void-$linuxArch-ROOTFS-*.tar.xz; " +
                    "manifest start: " + manifest.lineSequence().take(5).joinToString(" / "),
            )
        val filename = best.groupValues[1]
        val sha = best.groupValues[2].lowercase()
        return Resolved(
            downloadUrl = "$MIRROR/$filename",
            filename = filename,
            sha256Hex = sha,
        )
    }

    private fun downloadString(url: String): String {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 30_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "tawc-installer")
        }
        try {
            val code = conn.responseCode
            if (code !in 200..299) {
                throw IOException("GET $url returned HTTP $code")
            }
            return conn.inputStream.use { String(it.readBytes(), Charsets.UTF_8) }
        } finally {
            conn.disconnect()
        }
    }
}
