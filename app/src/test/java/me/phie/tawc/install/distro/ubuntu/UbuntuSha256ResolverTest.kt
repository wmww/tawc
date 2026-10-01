package me.phie.tawc.install.distro.ubuntu

import me.phie.tawc.install.SignatureVerifier
import me.phie.tawc.install.distro.apt.AptCommon
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException

/**
 * The Ubuntu bootstrap trust path, against **real upstream vectors**:
 * `app/src/test/resources/ubuntu-base/SHA256SUMS` and its detached
 * `SHA256SUMS.gpg`, both taken verbatim from
 * `cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/` (fetched
 * 2026-10-01). No network access.
 *
 * The signature is the whole trust story here — the manifest and the
 * tarball share one origin — so these tests are the load-bearing ones:
 * a valid signature must resolve the newest point release, and every way
 * of breaking the manifest or its signature must throw.
 */
class UbuntuSha256ResolverTest {

    private fun resource(name: String): ByteArray =
        checkNotNull(javaClass.getResourceAsStream("/ubuntu-base/$name")) {
            "missing test resource ubuntu-base/$name"
        }.use { it.readBytes() }

    private val sums get() = resource("SHA256SUMS")
    private val sig get() = resource("SHA256SUMS.gpg")

    @Test
    fun resolvesTheNewestArm64PointRelease() {
        val r = UbuntuSha256Resolver.resolveFromManifest("arm64", sums, sig, "test")
        // 24.04.5 was the newest in the manifest when this vector was
        // captured; its digest is the one upstream publishes.
        assertEquals("ubuntu-base-24.04.5-base-arm64.tar.gz", r.filename)
        assertEquals(
            "a91d5a93010193712d346d761372b7c9db6dfcf093893161c64ca107f05914f2",
            r.sha256Hex,
        )
        assertEquals(
            "https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/${r.filename}",
            r.downloadUrl,
        )
    }

    @Test
    fun resolvesAmd64Independently() {
        val r = UbuntuSha256Resolver.resolveFromManifest("amd64", sums, sig, "test")
        assertEquals("ubuntu-base-24.04.5-base-amd64.tar.gz", r.filename)
        assertEquals(
            "e77b6f10c2590cef872b33ee9f635a0e3fd1f57fb074c0e52b5c7f56147a0c86",
            r.sha256Hex,
        )
    }

    @Test
    fun tamperedManifestIsRejected() {
        // Flip one digest: the signature no longer covers the bytes.
        val tampered = String(sums, Charsets.UTF_8)
            .replace("a91d5a93010193712d346d761372b7c9", "a91d5a93010193712d346d761372b7c8")
            .toByteArray()
        val e = assertThrows(IOException::class.java) {
            UbuntuSha256Resolver.resolveFromManifest("arm64", tampered, sig, "test")
        }
        assertTrue(e.message!!, e.message!!.contains("signature verification FAILED"))
    }

    @Test
    fun garbageSignatureIsRejected() {
        assertThrows(IOException::class.java) {
            UbuntuSha256Resolver.resolveFromManifest("arm64", sums, "not a signature".toByteArray(), "test")
        }
    }

    @Test
    fun emptySignatureIsRejected() {
        assertThrows(IOException::class.java) {
            UbuntuSha256Resolver.resolveFromManifest("arm64", sums, ByteArray(0), "test")
        }
    }

    /** In-repo guard: the shipped public key must be the one that signed the vector. */
    @Test
    fun bundledKeyMatchesTheManifestSignature() {
        val keyId = SignatureVerifier.verifyDetached(
            keyRing = UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY.byteInputStream(Charsets.UTF_8),
            keyLabel = "UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY",
            signatureBytes = sig,
            data = sums,
            sourceLabel = "test vector",
        )
        // Key id of 8439 38DF 228D 22F7 B374 2BC0 D94A A3F0 EFE2 1092.
        assertEquals("D94AA3F0EFE21092", java.lang.Long.toHexString(keyId).uppercase())
    }

    /**
     * `snapd`'s postinst needs `CAP_SETFCAP` (unavailable under tawcroot),
     * and a failed postinst leaves dpkg half-configured so every later apt
     * transaction exits non-zero. The distro therefore blocks it up front.
     */
    @Test
    fun snapdIsBlockedFromEveryOrigin() {
        assertEquals(listOf("snapd"), Ubuntu2404Aarch64.blockedPackages)
        val body = AptCommon.blockedPackagesBody(Ubuntu2404Aarch64.blockedPackages)
        assertEquals(
            listOf(
                "Package: snapd",
                "Pin: release *",
                "Pin-Priority: -1",
            ).joinToString("\n"),
            body,
        )
    }

    @Test
    fun bundledKeyParsesToThePublishedFingerprint() {
        val ring = UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY.byteInputStream(Charsets.UTF_8).use {
            SignatureVerifier.parseKeyRing(it, "UbuntuReleaseKeys.CDIMAGE_SIGNING_KEY")
        }
        val fingerprints = ring.keyRings.asSequence()
            .map { keyRing -> keyRing.publicKey.fingerprint.joinToString("") { "%02X".format(it) } }
            .toSet()
        assertEquals(setOf(UbuntuReleaseKeys.FINGERPRINT), fingerprints)
    }

    /**
     * Selection rules on synthetic manifests: an unauthenticated manifest
     * is never used for a real install, but the rules themselves must be
     * right, and no real signature can be produced for a reordered or
     * `24.04.10`-flavoured one.
     */
    @Test
    fun selectNewestIgnoresLineOrder() {
        val manifest = listOf(
            "1111111111111111111111111111111111111111111111111111111111111111 *ubuntu-base-24.04.9-base-arm64.tar.gz",
            "2222222222222222222222222222222222222222222222222222222222222222 *ubuntu-base-24.04.10-base-arm64.tar.gz",
            "3333333333333333333333333333333333333333333333333333333333333333 *ubuntu-base-24.04.5-base-arm64.tar.gz",
        ).joinToString("\n")
        assertEquals(
            "ubuntu-base-24.04.10-base-arm64.tar.gz",
            UbuntuSha256Resolver.selectNewest(manifest, "arm64").filename,
        )
    }

    @Test
    fun selectNewestFiltersByArch() {
        val manifest = listOf(
            "1111111111111111111111111111111111111111111111111111111111111111 *ubuntu-base-24.04.5-base-amd64.tar.gz",
            "2222222222222222222222222222222222222222222222222222222222222222 *ubuntu-base-24.04.3-base-arm64.tar.gz",
        ).joinToString("\n")
        assertEquals(
            "ubuntu-base-24.04.3-base-arm64.tar.gz",
            UbuntuSha256Resolver.selectNewest(manifest, "arm64").filename,
        )
        assertEquals(
            "ubuntu-base-24.04.5-base-amd64.tar.gz",
            UbuntuSha256Resolver.selectNewest(manifest, "amd64").filename,
        )
    }

    @Test
    fun selectNewestRejectsAManifestWithNoMatchingArch() {
        // armhf/riscv64 entries exist upstream; asking for an architecture
        // the series does not publish must fail rather than pick one.
        assertThrows(IOException::class.java) {
            UbuntuSha256Resolver.selectNewest(String(sums, Charsets.UTF_8), "mips64el")
        }
    }
}
