package me.phie.tawc.install.distro.ubuntu

import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.SignatureVerifier
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Resolve the newest Ubuntu base tarball for a given Ubuntu architecture
 * by fetching `SHA256SUMS` from
 * `cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/`, **verifying
 * its detached PGP signature** against [UbuntuReleaseKeys], and parsing
 * out the matching line.
 *
 * Ubuntu publishes one `SHA256SUMS` covering every point release and
 * every architecture (`ubuntu-base-24.04.5-base-arm64.tar.gz` and
 * friends), so a single signed file yields both the newest filename and
 * its digest — structurally the same story as Void's `sha256sum.txt` +
 * minisign signature, with PGP instead of minisign.
 *
 * The manifest and the tarball share an origin, so the SHA-256 alone
 * would only be a corruption / host-swap check. The signature is checked
 * **before** any digest parsed out of the manifest is used, and the key
 * comes from a different origin than the download, so forging a boot
 * strap needs both. See notes/installation.md "Bootstrap integrity".
 *
 * Fails closed throughout: an unfetchable signature, a bad signature, or
 * a manifest with no entry for this architecture all throw, and the
 * install aborts before anything is downloaded.
 */
internal object UbuntuSha256Resolver {

    private const val MIRROR = "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release"

    /**
     * Ubuntu point releases within an LTS series. Matched by
     * [POINT_RELEASE_RE], which also fixes the URL directory ([MIRROR]).
     */
    private const val SERIES = "24\\.04"

    data class Resolved(val downloadUrl: String, val filename: String, val sha256Hex: String)

    /**
     * Look up the newest Ubuntu base tarball for [ubuntuArch]
     * (`"arm64"` or `"amd64"`). Throws [IOException] if the manifest or
     * its signature can't be fetched, the signature doesn't verify, or
     * the matching line is missing.
     */
    fun resolveLatest(
        ubuntuArch: String,
        mirrorProxy: MirrorProxy? = null,
        log: (String) -> Unit = {},
    ): Resolved {
        val sumsUrl = "$MIRROR/SHA256SUMS"
        // Keep the raw bytes: the signature covers the file verbatim, so
        // re-encoding a decoded String could break verification.
        val manifestBytes = downloadBytes(mirrorProxy?.wrap(sumsUrl) ?: sumsUrl)
        val sigUrl = "$MIRROR/SHA256SUMS.gpg"
        val sigBytes = try {
            downloadBytes(mirrorProxy?.wrap(sigUrl) ?: sigUrl)
        } catch (e: IOException) {
            throw IOException(
                "Ubuntu SHA256SUMS.gpg could not be fetched from $sigUrl (${e.message}). " +
                    "Refusing to trust an unsigned SHA256SUMS.",
                e,
            )
        }
        return resolveFromManifest(
            ubuntuArch = ubuntuArch,
            manifestBytes = manifestBytes,
            signatureBytes = sigBytes,
            sigOrigin = sigUrl,
            log = log,
        )
    }

    /**
     * The pure half of [resolveLatest]: authenticate [manifestBytes]
     * with [signatureBytes] first, and only then pick the newest matching
     * entry out of it. Split out so the whole trust-critical sequence is
     * unit-testable against real upstream vectors without network access.
     */
    internal fun resolveFromManifest(
        ubuntuArch: String,
        manifestBytes: ByteArray,
        signatureBytes: ByteArray,
        sigOrigin: String,
        log: (String) -> Unit = {},
    ): Resolved {
        // Signature first: nothing parsed out of the manifest is trusted
        // until the manifest itself is.
        SignatureVerifier.verifyDetached(
            keyRing = UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY.byteInputStream(Charsets.UTF_8),
            keyLabel = "UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY (${UbuntuReleaseKeys.KEY_ORIGIN})",
            signatureBytes = signatureBytes,
            data = manifestBytes,
            sourceLabel = "Ubuntu SHA256SUMS ($sigOrigin)",
        )
        log("ubuntu: SHA256SUMS signature verified (key ${UbuntuReleaseKeys.FINGERPRINT})")

        return selectNewest(String(manifestBytes, Charsets.UTF_8), ubuntuArch)
    }

    /**
     * Pick the newest matching entry out of an **already authenticated**
     * [manifest]. Split out from [resolveFromManifest] so the selection
     * rules (architecture filter, highest point release, not line order)
     * are unit-testable against synthetic manifests a real signature
     * cannot be produced for.
     */
    internal fun selectNewest(manifest: String, ubuntuArch: String): Resolved {
        // Lines look like:
        //   <64 hex> *ubuntu-base-24.04.5-base-arm64.tar.gz
        val pattern = Regex(
            """^([0-9a-f]{64}) \*(ubuntu-base-($SERIES(?:\.\d+)?)-base-${Regex.escape(ubuntuArch)}\.tar\.gz)$""",
        )
        // One SUMS file covers every published point release; take the
        // newest by numeric version, not by line order.
        val best = manifest.lineSequence()
            .mapNotNull { pattern.find(it.trim()) }
            .maxWithOrNull(
                compareBy(VERSION_ORDER) { pointRelease(it.groupValues[3]) },
            )
            ?: throw IOException(
                "Ubuntu SHA256SUMS has no entry for ubuntu-base-24.04*-base-$ubuntuArch.tar.gz; " +
                    "manifest start: " + manifest.lineSequence().take(5).joinToString(" / "),
            )

        return Resolved(
            downloadUrl = "$MIRROR/${best.groupValues[2]}",
            filename = best.groupValues[2],
            sha256Hex = best.groupValues[1].lowercase(),
        )
    }

    /** `24.04.5` → `[24, 4, 5]`, compared component-wise. */
    private fun pointRelease(version: String): List<Int> = version.split('.').map(String::toInt)

    /**
     * Component-wise compare, zero-padding the shorter side so a bare
     * `24.04` and `24.04.0` order equally (and both below `24.04.1`).
     * Lexicographic ordering would put `24.04.10` before `24.04.9`.
     */
    private val VERSION_ORDER = Comparator<List<Int>> { a, b ->
        for (i in 0 until maxOf(a.size, b.size)) {
            val diff = (a.getOrNull(i) ?: 0) - (b.getOrNull(i) ?: 0)
            if (diff != 0) return@Comparator diff
        }
        0
    }

    private fun downloadBytes(url: String): ByteArray {
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
            return conn.inputStream.use { it.readBytes() }
        } finally {
            conn.disconnect()
        }
    }
}
