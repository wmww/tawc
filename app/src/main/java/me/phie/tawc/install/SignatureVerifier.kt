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
import org.bouncycastle.openpgp.operator.bc.BcPGPContentVerifierBuilderProvider
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import me.phie.tawc.R

/**
 * Detached-PGP-signature verification for downloaded bootstrap tarballs.
 *
 * This is the integrity barrier between [Downloader] writing bytes to
 * disk and [Archive.extractAsRoot] handing those bytes to root-running
 * tar. Anything that gets past this gate is treated as trustworthy
 * enough to lay down inside the chroot the user then runs Wayland apps
 * in — see notes/installation.md "Bootstrap integrity".
 *
 * The PGP consumers are both Arch flavours:
 *
 *  - [me.phie.tawc.install.distro.arch.ArchLinuxX86_64] — `.tar.zst.sig`
 *    signed by Pierre Schmitz's Arch developer key (fingerprint
 *    `3E80 CA1A 8B89 F69C BA57  D98A 76A5 EF90 5444 9A5C`, shipped at
 *    `res/raw/arch_signing_key.asc`).
 *  - [me.phie.tawc.install.distro.arch.ArchLinuxArm] — `.tar.gz.sig`
 *    signed by the ALARM build system key (fingerprint
 *    `68B3 537F 39A3 13B3 E574  D067 7719 3F15 2BDB E6A6`, shipped at
 *    `res/raw/archlinuxarm_signing_key.asc`).
 *
 * The remaining distros use [BootstrapVerification.Sha256] — see
 * notes/installation.md "Bootstrap integrity" for who declares what.
 */
object SignatureVerifier {
    private const val TAG = "tawc-install"

    /**
     * Verify [tarball] against [verification], throwing [IOException]
     * on any failure (download error, malformed signature, key-id
     * mismatch, bad signature). Caller must NOT proceed to extract on
     * exception — the gate is there to keep unverified bytes out of
     * the rootfs.
     *
     * @param mirrorProxy debug-builds-only knob: when non-null, the PGP
     *   `.sig` fetch here is routed through it. This does not weaken the
     *   check — the signature is still verified against the shipped
     *   key, so a proxy that tampers with the `.sig` fails closed — but
     *   it keeps the dev cache coherent: without it the proxy would
     *   serve a cached tarball while the `.sig` was fetched fresh
     *   upstream, and since both Arch bootstrap URLs are mutable
     *   `latest` paths that pair would mismatch until the proxy was
     *   manually cleared. Release builds always pass null.
     */
    fun verify(
        context: Context,
        tarball: File,
        verification: BootstrapVerification,
        mirrorProxy: me.phie.tawc.install.MirrorProxy? = null,
    ) {
        when (verification) {
            BootstrapVerification.ResolvedAtInstallTime -> throw IOException(
                "Bootstrap verification for ${tarball.name} is still the " +
                    "ResolvedAtInstallTime placeholder — the distro's " +
                    "resolveBootstrap() failed to substitute a real policy. " +
                    "Refusing to extract an unverified bootstrap.",
            )

            is BootstrapVerification.Pgp -> verifyPgp(context, tarball, verification, mirrorProxy)
            is BootstrapVerification.Sha256 -> verifySha256(tarball, verification)
        }
    }

    /**
     * Verify [tarball] against a known-good SHA-256 hex digest. Used
     * when the upstream bootstrap source hands us the digest out of
     * band (e.g. GitHub Releases API `digest` field, OCI manifest
     * digest) — no PGP signature, no checksum sidecar, but the digest
     * is fetched over HTTPS by the caller's `resolveBootstrap` and
     * passed in here. How much that digest is worth depends on the
     * caller: for Debian/Manjaro it's "trust this single TLS
     * endpoint", so a compromised origin serves a matching
     * tarball/digest pair; for Void the digest comes from a manifest
     * with a verified minisign signature from an independent key
     * origin. See notes/installation.md "Bootstrap integrity".
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
        mirrorProxy: me.phie.tawc.install.MirrorProxy?,
    ) {
        Log.d(TAG, "Verifying PGP signature for ${tarball.name}")
        val sigBytes = downloadBytes(mirrorProxy?.wrap(v.signatureUrl) ?: v.signatureUrl)
        val keys = loadKeyRing(context, v.keyResource)
        val keyId = verifyDetached(
            keys = keys,
            signatureBytes = sigBytes,
            keyLabel = v.keyResource,
            sourceLabel = tarball.name,
            openData = { tarball.inputStream() },
        )
        Log.i(
            TAG,
            "Bootstrap PGP signature verified: ${tarball.name} signed by " +
                "0x${java.lang.Long.toHexString(keyId).uppercase()}",
        )
    }

    /**
     * Verify a detached signature over [data] (or over [openData], for
     * data too large to hold in memory) against [keyRing].
     *
     * The bytes-shaped sibling of [verifyPgp]: no `Context`, no `File`,
     * no URL, so a distro that verifies a *manifest* at resolve time can
     * use the exact same code path the tarball check does — see
     * [me.phie.tawc.install.distro.ubuntu.UbuntuSha256Resolver], which
     * runs before any `Context` exists. Keeping one implementation means
     * the two cannot drift into different trust decisions.
     *
     * Throws [IOException] on a bad signature, a malformed signature
     * blob, or a signer whose key is not in [keyRing]; returns the
     * signing key id for logging.
     */
    internal fun verifyDetached(
        keys: PGPPublicKeyRingCollection,
        signatureBytes: ByteArray,
        keyLabel: String,
        sourceLabel: String,
        openData: () -> InputStream,
    ): Long {
        val signature = parseDetachedSignature(signatureBytes)
        val key = resolveSigningKey(keys, signature, keyLabel)
        signature.init(BcPGPContentVerifierBuilderProvider(), key)
        openData().use { input ->
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
                "PGP signature verification FAILED for $sourceLabel. " +
                    "It is corrupt or tampered with.",
            )
        }
        return signature.keyID
    }

    /**
     * [verifyDetached] for an ASCII-armored keyring and an in-memory
     * payload — the shape a resolver holding a bundled key and a fetched
     * manifest needs.
     */
    internal fun verifyDetached(
        keyRing: InputStream,
        keyLabel: String,
        signatureBytes: ByteArray,
        data: ByteArray,
        sourceLabel: String,
    ): Long = parseKeyRing(keyRing, keyLabel).let { keys ->
        verifyDetached(
            keys = keys,
            signatureBytes = signatureBytes,
            keyLabel = keyLabel,
            sourceLabel = sourceLabel,
            openData = { ByteArrayInputStream(data) },
        )
    }

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
    internal fun parseDetachedSignature(bytes: ByteArray): PGPSignature {
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

    /**
     * Look [signature]'s issuer up in [keys]. Split out of [verifyPgp]
     * so the "signed by a key we don't ship" path is reachable from a
     * unit test without a network fetch — see `ShippedPgpKeysTest`.
     */
    internal fun resolveSigningKey(
        keys: PGPPublicKeyRingCollection,
        signature: PGPSignature,
        keyResource: String,
    ): PGPPublicKey = keys.getPublicKey(signature.keyID)
        ?: throw IOException(
            "Bootstrap signature key id 0x${java.lang.Long.toHexString(signature.keyID)} " +
                "not present in shipped keyring (resource $keyResource). " +
                "Either the upstream rotated their signing key, or this is a forged tarball.",
        )

    /**
     * Map of every shipped verification-key resource name to its
     * `res/raw` id — [BootstrapVerification.Pgp.keyResource] and
     * [me.phie.tawc.install.distro.PackageBootstrap.keyResource] both
     * resolve through it. Deliberately a hand-written map rather than
     * `Resources.getIdentifier`: it is greppable, and an unknown name
     * makes [loadKeyRing] fail closed instead of silently verifying
     * against nothing. Adding a distro with a new key means adding a
     * line here — `ShippedPgpKeysTest` and
     * `BootstrapVerificationFailClosedTest` fail if you forget.
     */
    internal val KEY_RESOURCE_IDS: Map<String, Int> = mapOf(
        "arch_signing_key" to R.raw.arch_signing_key,
        "archlinuxarm_signing_key" to R.raw.archlinuxarm_signing_key,
        "debian_archive_keyring" to R.raw.debian_archive_keyring,
    )

    internal fun rawKeyResourceId(resourceName: String): Int =
        KEY_RESOURCE_IDS[resourceName] ?: 0

    /**
     * Parse an ASCII-armored public-key bundle. [label] only names the
     * source in error messages. Split out of [loadKeyRing] so tests can
     * feed it the actual bytes of a shipped `res/raw` key off disk,
     * with no `Context` in play.
     */
    internal fun parseKeyRing(input: InputStream, label: String): PGPPublicKeyRingCollection =
        ArmoredInputStream(input).use { armored ->
            val factory = BcPGPObjectFactory(armored)
            val rings = mutableListOf<PGPPublicKeyRing>()
            while (true) {
                val obj = factory.nextObject() ?: break
                if (obj is PGPPublicKeyRing) rings.add(obj)
            }
            if (rings.isEmpty()) {
                throw IOException("No public key rings in $label")
            }
            PGPPublicKeyRingCollection(rings)
        }

    internal fun loadKeyRing(context: Context, resourceName: String): PGPPublicKeyRingCollection {
        val resId = rawKeyResourceId(resourceName)
        if (resId == 0) {
            throw IOException("Missing PGP key resource: res/raw/$resourceName")
        }
        return context.resources.openRawResource(resId).use { input ->
            parseKeyRing(input, "res/raw/$resourceName")
        }
    }

    /** Uppercase hex fingerprint, no spaces — used by tests and logs. */
    internal fun PGPPublicKey.fingerprintHex(): String =
        fingerprint.joinToString("") { "%02X".format(it) }
}

/**
 * Per-distro bootstrap-integrity policy. Set on
 * [me.phie.tawc.install.distro.DistroBootstrap]; consumed by
 * [SignatureVerifier.verify] between download and extract.
 */
sealed class BootstrapVerification {
    /**
     * Placeholder for distros whose real policy is only known at
     * install time: the static [me.phie.tawc.install.distro.Distro.bootstrap]
     * declares this, and `resolveBootstrap()` must replace it with a
     * concrete variant (today always [Sha256] from a live digest
     * lookup). **Fails closed**: if this value ever reaches
     * [SignatureVerifier.verify] — a new distro forgot the override,
     * or a refactor dropped it — the install throws instead of
     * proceeding unverified. There is deliberately no "skip
     * verification" variant; a distro that genuinely cannot be
     * verified must add one back, visibly, and justify it against
     * notes/installation.md "Bootstrap integrity".
     */
    object ResolvedAtInstallTime : BootstrapVerification()

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
     * Compare a known-good SHA-256 hex digest (passed in by the
     * Distro's `resolveBootstrap` after fetching it from a single
     * trusted HTTPS endpoint, e.g. the GitHub Releases REST API
     * `digest` field or an OCI manifest blob digest). Catches mid-
     * download corruption and redirect-to-different-host as a sanity
     * check; the security stance still rests on whatever produced the
     * digest — when that is a TLS endpoint which also serves the
     * tarball (Debian, Manjaro today), a compromised origin defeats
     * it. Void is the exception: its digest comes out of a manifest
     * whose minisign signature
     * [me.phie.tawc.install.distro.voidlinux.VoidSha256Resolver] has
     * already checked against a key from a second origin, so the same
     * variant carries a stronger story there. Weaker than [Pgp] (no
     * detached-key chain) in the general case.
     */
    data class Sha256(
        val expectedHex: String,
    ) : BootstrapVerification()
}
