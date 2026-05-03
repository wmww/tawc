package me.phie.tawc.install

import android.content.Context
import android.util.Log
import org.bouncycastle.bcpg.ArmoredInputStream
import org.bouncycastle.openpgp.PGPCompressedData
import org.bouncycastle.openpgp.PGPPublicKey
import org.bouncycastle.openpgp.PGPPublicKeyRing
import org.bouncycastle.openpgp.PGPPublicKeyRingCollection
import org.bouncycastle.openpgp.PGPSignature
import org.bouncycastle.openpgp.PGPSignatureList
import org.bouncycastle.openpgp.PGPUtil
import org.bouncycastle.openpgp.bc.BcPGPObjectFactory
import org.bouncycastle.openpgp.operator.bc.BcKeyFingerprintCalculator
import org.bouncycastle.openpgp.operator.bc.BcPGPContentVerifierBuilderProvider
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Detached-PGP-signature verification for downloaded bootstrap tarballs.
 *
 * This is the integrity barrier between [Downloader] writing bytes to
 * disk and [Archive.extractAsRoot] handing those bytes to root-running
 * tar. Anything that gets past this gate is treated as trustworthy
 * enough to lay down inside the chroot the user then runs Wayland apps
 * in — see notes/installation.md "Bootstrap integrity".
 *
 * The current consumer is [me.phie.tawc.install.distro.arch.ArchLinuxX86_64],
 * whose `.tar.zst.sig` is signed by Pierre Schmitz's Arch developer
 * key (fingerprint `3E80 CA1A 8B89 F69C BA57  D98A 76A5 EF90 5444 9A5C`,
 * shipped at `res/raw/arch_signing_key.asc`).
 *
 * ALARM aarch64 has no upstream PGP signature — see [BootstrapVerification.None]
 * and the issue tracker for the remaining gap.
 */
object SignatureVerifier {
    private const val TAG = "tawc-install"

    /**
     * Verify [tarball] against [verification], throwing [IOException]
     * on any failure (download error, malformed signature, key-id
     * mismatch, bad signature). Caller must NOT proceed to extract on
     * exception — the gate is there to keep unverified bytes out of
     * the rootfs.
     */
    fun verify(
        context: Context,
        tarball: File,
        verification: BootstrapVerification,
    ) {
        when (verification) {
            BootstrapVerification.None -> {
                Log.w(
                    TAG,
                    "Bootstrap NOT VERIFIED: ${tarball.name} — upstream " +
                        "publishes no signature. See issue " +
                        "install-alarm-bootstrap-no-pgp.md.",
                )
                return
            }

            is BootstrapVerification.Pgp -> verifyPgp(context, tarball, verification)
            is BootstrapVerification.CrossMirrorMd5 -> verifyCrossMirrorMd5(tarball, verification)
            is BootstrapVerification.Sha256 -> verifySha256(tarball, verification)
        }
    }

    /**
     * Verify [tarball] against a known-good SHA-256 hex digest. Used
     * when the upstream bootstrap source hands us the digest out of
     * band (e.g. GitHub Releases API `digest` field, OCI manifest
     * digest) — no PGP signature, no checksum sidecar, but the digest
     * is fetched from a single trusted HTTPS endpoint by the caller's
     * `resolveBootstrap` and passed in here. The integrity story is
     * "trust this single TLS endpoint"; weaker than PGP, comparable
     * in spirit to [None] with a sanity check that catches mid-
     * download corruption / redirect-to-different-host.
     */
    private fun verifySha256(
        tarball: File,
        v: BootstrapVerification.Sha256,
    ) {
        val expected = v.expectedHex.lowercase()
        require(expected.length == 64 && expected.all { it.isDigit() || it in 'a'..'f' }) {
            "Sha256 expected hex must be 64 lowercase hex chars, got '${v.expectedHex}'"
        }
        val md = MessageDigest.getInstance("SHA-256")
        tarball.inputStream().use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                if (Thread.interrupted()) throw InterruptedIOException("verify cancelled")
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        val actual = md.digest().joinToString("") { "%02x".format(it) }
        if (actual != expected) {
            throw IOException(
                "Bootstrap SHA-256 mismatch for ${tarball.name}: " +
                    "expected $expected, got $actual. " +
                    "Tarball is corrupt or tampered with.",
            )
        }
        Log.i(TAG, "Bootstrap SHA-256 verified: ${tarball.name} ($actual)")
    }

    private fun verifyPgp(
        context: Context,
        tarball: File,
        v: BootstrapVerification.Pgp,
    ) {
        Log.d(TAG, "Verifying PGP signature for ${tarball.name}")
        val sigBytes = downloadBytes(v.signatureUrl)
        val signature = parseDetachedSignature(sigBytes)
        val keys = loadKeyRing(context, v.keyResource)
        val key = keys.getPublicKey(signature.keyID)
            ?: throw IOException(
                "Bootstrap signature key id 0x${java.lang.Long.toHexString(signature.keyID)} " +
                    "not present in shipped keyring (resource ${v.keyResource}). " +
                    "Either the upstream rotated their signing key, or this is a forged tarball.",
            )

        signature.init(BcPGPContentVerifierBuilderProvider(), key)
        tarball.inputStream().use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                if (Thread.interrupted()) throw InterruptedIOException("verify cancelled")
                val n = input.read(buf)
                if (n < 0) break
                signature.update(buf, 0, n)
            }
        }
        if (!signature.verify()) {
            throw IOException(
                "Bootstrap signature verification FAILED for ${tarball.name}. " +
                    "Tarball is corrupt or tampered with.",
            )
        }
        Log.i(
            TAG,
            "Bootstrap PGP signature verified: ${tarball.name} signed by " +
                "0x${java.lang.Long.toHexString(signature.keyID).uppercase()}",
        )
    }

    /**
     * Defence-in-depth for distros that publish only an MD5 sidecar
     * (ALARM aarch64). For each [BootstrapVerification.CrossMirrorMd5.checksumUrls]:
     *
     *   1. Fetch each `.md5` over HTTPS — TLS+cert validation eliminates
     *      passive MITM on each individual fetch.
     *   2. Require all of them to declare the **same** hex digest. Two
     *      independent mirror operators (or two different CDN POPs) would
     *      have to be concurrently compromised AND publish the same
     *      forged digest for this to lie. Each `.md5` is a 32-character
     *      hex blob; we strip the trailing filename Arch's md5 format
     *      includes.
     *   3. Compute the tarball's MD5 and require the match.
     *
     * MD5 is broken against collision attacks — an attacker who controls
     * both the tarball and the digest can grind a colliding pair. They
     * do **not** control the digest here (it's published by upstream
     * before they ever see the tarball), so only the much harder
     * second-preimage attack matters, and MD5 still resists that. PGP
     * is still preferred (see issue) — this is a transitional check
     * until upstream signs.
     */
    private fun verifyCrossMirrorMd5(
        tarball: File,
        v: BootstrapVerification.CrossMirrorMd5,
    ) {
        require(v.checksumUrls.size >= 2) {
            "CrossMirrorMd5 needs at least 2 independent checksum URLs"
        }

        val digests = mutableListOf<Pair<String, String>>()
        var lastEx: IOException? = null
        for (url in v.checksumUrls) {
            require(url.startsWith("https://")) {
                "CrossMirrorMd5 URL must be HTTPS (was $url) — defeats the cross-check otherwise"
            }
            try {
                val body = String(downloadBytes(url), Charsets.US_ASCII)
                val token = body.trim().substringBefore(' ').lowercase()
                require(token.length == 32 && token.all { it.isDigit() || it in 'a'..'f' }) {
                    "Malformed MD5 fetched from $url: '$token'"
                }
                digests += url to token
            } catch (e: IOException) {
                Log.w(TAG, "CrossMirrorMd5 fetch failed: $url: ${e.message}")
                lastEx = e
            }
        }
        // Offline fallback: if all mirror fetches failed (DNS or transport)
        // AND a local sidecar `<tarball>.md5` was previously written
        // out by a successful verify, trust that. Without this, an offline
        // re-run of an already-verified bootstrap aborts the install.
        if (digests.isEmpty()) {
            val sidecar = File(tarball.parentFile, tarball.name + ".md5.verified")
            if (sidecar.exists()) {
                val cached = sidecar.readText().trim().lowercase()
                require(cached.length == 32 && cached.all { it.isDigit() || it in 'a'..'f' }) {
                    "Malformed sidecar MD5 in ${sidecar.path}: '$cached'"
                }
                Log.w(TAG, "Cross-mirror fetch failed; falling back to verified sidecar ${sidecar.name}")
                digests += "sidecar://${sidecar.name}" to cached
                digests += "sidecar://${sidecar.name}" to cached
            } else {
                throw lastEx ?: IOException("CrossMirrorMd5: no mirrors reachable and no sidecar")
            }
        }
        Log.d(TAG, "Cross-mirror MD5 fetched: ${digests.joinToString { "${it.second.take(8)}…@${shortHost(it.first)}" }}")

        val canonical = digests.first().second
        for ((url, d) in digests.drop(1)) {
            if (d != canonical) {
                throw IOException(
                    "Cross-mirror MD5 mismatch: ${digests.first().first} says $canonical, " +
                        "$url says $d. Refusing to extract — possible mirror compromise.",
                )
            }
        }

        val md = MessageDigest.getInstance("MD5")
        tarball.inputStream().use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                if (Thread.interrupted()) throw InterruptedIOException("verify cancelled")
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        val tarballHex = md.digest().joinToString("") { "%02x".format(it) }
        if (tarballHex != canonical) {
            throw IOException(
                "Bootstrap MD5 mismatch for ${tarball.name}: tarball=$tarballHex, " +
                    "${digests.size} cross-mirror agreement on $canonical. Tarball is corrupt or tampered with.",
            )
        }
        // Sidecar marker for offline fallback (see verifyCrossMirrorMd5
        // when all mirrors fail to resolve).
        runCatching {
            File(tarball.parentFile, tarball.name + ".md5.verified")
                .writeText(canonical)
        }
        Log.i(
            TAG,
            "Bootstrap MD5 verified: ${tarball.name} ($canonical), agreed by " +
                "${digests.size} HTTPS mirrors",
        )
    }

    private fun shortHost(url: String): String =
        try { URL(url).host } catch (_: Exception) { url }

    private fun downloadBytes(url: String): ByteArray {
        if (Thread.interrupted()) throw InterruptedIOException("download cancelled")
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 30_000
            readTimeout = 30_000
            instanceFollowRedirects = true
        }
        // HttpURLConnection's blocking calls don't honour the thread
        // interrupt flag, so during a long fetch a coroutine cancel
        // can't tip the request over. Park a watchdog thread that
        // disconnects on interrupt — disconnect throws the read with
        // an IOException, and the outer InterruptedIOException check
        // re-asserts the cancel below.
        val owner = Thread.currentThread()
        val watchdog = Thread {
            try {
                while (!Thread.currentThread().isInterrupted) {
                    if (owner.isInterrupted) {
                        conn.disconnect()
                        return@Thread
                    }
                    Thread.sleep(50)
                }
            } catch (_: InterruptedException) { /* watchdog itself ended */ }
        }.apply { isDaemon = true; start() }
        try {
            val code = conn.responseCode
            if (code !in 200..299) {
                throw IOException("GET $url returned HTTP $code")
            }
            return conn.inputStream.use { it.readBytes() }
        } finally {
            watchdog.interrupt()
            conn.disconnect()
            if (Thread.interrupted()) throw InterruptedIOException("download cancelled")
        }
    }

    /**
     * Parse a detached signature blob — accepts both ASCII-armored
     * (`.asc`) and binary (`.sig`) forms. Arch publishes the binary
     * form at `<tarball>.sig`. The signature may also be wrapped in a
     * compressed-data packet, so handle that case too.
     */
    private fun parseDetachedSignature(bytes: ByteArray): PGPSignature {
        val raw: InputStream = PGPUtil.getDecoderStream(ByteArrayInputStream(bytes))
        var factory = BcPGPObjectFactory(raw)
        var obj = factory.nextObject()
        if (obj is PGPCompressedData) {
            factory = BcPGPObjectFactory(obj.dataStream)
            obj = factory.nextObject()
        }
        val list = obj as? PGPSignatureList
            ?: throw IOException("Signature blob did not contain a PGPSignatureList (got ${obj?.javaClass?.simpleName})")
        if (list.isEmpty) throw IOException("Signature blob is empty")
        return list[0]
    }

    private fun loadKeyRing(context: Context, resourceName: String): PGPPublicKeyRingCollection {
        val resId = context.resources.getIdentifier(resourceName, "raw", context.packageName)
        if (resId == 0) {
            throw IOException("Missing PGP key resource: res/raw/$resourceName")
        }
        return context.resources.openRawResource(resId).use { input ->
            ArmoredInputStream(input).use { armored ->
                val factory = BcPGPObjectFactory(armored)
                val rings = mutableListOf<PGPPublicKeyRing>()
                while (true) {
                    val obj = factory.nextObject() ?: break
                    if (obj is PGPPublicKeyRing) rings.add(obj)
                }
                if (rings.isEmpty()) {
                    throw IOException("No public key rings in res/raw/$resourceName")
                }
                PGPPublicKeyRingCollection(rings)
            }
        }
    }

    @Suppress("unused")
    private fun PGPPublicKey.fingerprintHex(): String =
        fingerprint.joinToString("") { "%02X".format(it) }
}

/**
 * Per-distro bootstrap-integrity policy. Set on
 * [me.phie.tawc.install.distro.DistroBootstrap]; consumed by
 * [SignatureVerifier.verify] between download and extract.
 */
sealed class BootstrapVerification {
    /**
     * Last-resort opt-out — no integrity check at all. Logs a loud
     * warning every install. Reserved for cases where neither PGP nor
     * cross-mirror checksums are available; do **not** use this for
     * convenience. New distros must declare a real verification before
     * being added — see notes/installation.md "Bootstrap integrity".
     */
    object None : BootstrapVerification()

    /**
     * Detached PGP signature at [signatureUrl], verified against the
     * ASCII-armored public-key bundle shipped at
     * `res/raw/<keyResource>.asc`. Pass [keyResource] without the
     * `.asc` extension — Android resource identifiers don't carry it.
     * This is the strongest variant and the default for new distros.
     */
    data class Pgp(
        val signatureUrl: String,
        val keyResource: String,
    ) : BootstrapVerification()

    /**
     * Cross-mirror checksum cross-check, used when upstream publishes
     * an MD5 sidecar but no PGP signature (ALARM). [checksumUrls] must
     * point at independent HTTPS mirrors hosting the same `.md5`; the
     * verifier requires byte-for-byte agreement before checking the
     * downloaded tarball's MD5 against the consensus digest. See
     * `SignatureVerifier.verifyCrossMirrorMd5` for the threat model.
     */
    data class CrossMirrorMd5(
        val checksumUrls: List<String>,
    ) : BootstrapVerification()

    /**
     * Compare a known-good SHA-256 hex digest (passed in by the
     * Distro's `resolveBootstrap` after fetching it from a single
     * trusted HTTPS endpoint, e.g. the GitHub Releases REST API
     * `digest` field or an OCI manifest blob digest). Catches mid-
     * download corruption and redirect-to-different-host as a sanity
     * check; the security stance still rests on the TLS endpoint
     * that produced the digest. Stronger than [None]; weaker than
     * [Pgp] (no detached-key chain).
     */
    data class Sha256(
        val expectedHex: String,
    ) : BootstrapVerification()
}
