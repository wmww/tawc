package me.phie.tawc.install

import android.content.Context
import android.content.ContextWrapper
import me.phie.tawc.install.distro.BootstrapFlavor
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.install.distro.PackageBootstrap
import me.phie.tawc.install.distro.TarballBootstrap
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.IOException
import java.security.MessageDigest

/**
 * The bootstrap verify gate must fail closed: if a distro's static
 * [BootstrapVerification.ResolvedAtInstallTime] placeholder ever
 * survives to the verify stage (a `resolveBootstrap` override dropped
 * or forgotten), the install must abort, not proceed unverified.
 * See notes/installation.md "Bootstrap integrity".
 */
class BootstrapVerificationFailClosedTest {

    // Context is unused by the branches under test; android.jar is
    // stubbed (returnDefaultValues) so this is a plain dummy.
    private val context: Context = ContextWrapper(null)

    private fun tempTarball(bytes: ByteArray = "payload".toByteArray()): File =
        File.createTempFile("bootstrap-test", ".tar").apply {
            deleteOnExit()
            writeBytes(bytes)
        }

    @Test
    fun placeholderVerificationThrowsInsteadOfPassing() {
        val e = assertThrows(IOException::class.java) {
            SignatureVerifier.verify(
                context, tempTarball(), BootstrapVerification.ResolvedAtInstallTime,
            )
        }
        assertTrue(e.message!!.contains("ResolvedAtInstallTime"))
    }

    @Test
    fun sha256MismatchThrows() {
        assertThrows(IOException::class.java) {
            SignatureVerifier.verify(
                context, tempTarball(), BootstrapVerification.Sha256("0".repeat(64)),
            )
        }
    }

    @Test
    fun sha256MatchPasses() {
        val data = "tawc test payload".toByteArray()
        val hex = MessageDigest.getInstance("SHA-256").digest(data)
            .joinToString("") { "%02x".format(it) }
        SignatureVerifier.verify(context, tempTarball(data), BootstrapVerification.Sha256(hex))
    }

    /**
     * Walk **every flavor of every distro** and assert each states its
     * trust root: tarball flavors keep the placeholder-iff-resolves-
     * live biconditional; package flavors must name a keyring that is
     * actually registered and shipped. It must stay impossible to add
     * a flavor without a trust story.
     */
    @Test
    fun everyFlavorOfEveryDistroDeclaresItsTrustRoot() {
        val rawDir = sequenceOf(
            "src/main/res/raw",
            "app/src/main/res/raw",
            "../app/src/main/res/raw",
        ).map(::File).firstOrNull { it.isDirectory }
            ?: error("cannot locate res/raw")
        for (distro in DistroRegistry.all) {
            // Declared, not shipped: the trust story must hold for
            // every flavor a distro implements, independent of which
            // ones this build ships (EnabledBootstrapFlavors).
            val flavors = distro.declaredBootstrapFlavors
            assertTrue(
                "${distro.displayName}: supported flavor ${distro.supportedFlavor} " +
                    "is not in bootstrapFlavors",
                distro.supportedFlavor in flavors,
            )
            assertTrue(
                "${distro.displayName}: supported flavor ${distro.supportedFlavor} " +
                    "must be shipped by every build",
                distro.supportedFlavor in distro.bootstrapFlavors,
            )
            assertEquals(
                "${distro.displayName}: the TARBALL flavor entry must be the " +
                    "static bootstrap field",
                distro.bootstrap, flavors[BootstrapFlavor.TARBALL],
            )
            for ((flavor, b) in flavors) {
                when (b) {
                    is TarballBootstrap -> {
                        val isPlaceholder =
                            b.verification is BootstrapVerification.ResolvedAtInstallTime
                        val resolvesLive = distro.key in setOf(
                            Installation.DISTRO_VOID,
                            Installation.DISTRO_MANJARO,
                            Installation.DISTRO_DEBIAN_SID,
                            Installation.DISTRO_UBUNTU,
                        )
                        assertTrue(
                            "${distro.displayName} (${distro.linuxArch}) $flavor: static " +
                                "verification ${b.verification::class.simpleName} does not " +
                                "match its resolveBootstrap strategy",
                            isPlaceholder == resolvesLive,
                        )
                    }
                    is PackageBootstrap -> {
                        assertTrue(
                            "${distro.displayName} $flavor: blank keyResource",
                            b.keyResource.isNotBlank(),
                        )
                        assertTrue(
                            "${distro.displayName} $flavor: keyResource " +
                                "'${b.keyResource}' is not registered in " +
                                "SignatureVerifier.KEY_RESOURCE_IDS — loadKeyRing " +
                                "would fail closed at install time",
                            b.keyResource in SignatureVerifier.KEY_RESOURCE_IDS,
                        )
                        assertTrue(
                            "${distro.displayName} $flavor: res/raw/${b.keyResource}.asc missing",
                            File(rawDir, "${b.keyResource}.asc").isFile,
                        )
                    }
                }
            }
        }
    }

    /** Debian implements the packages flavor; nothing else does (yet). */
    @Test
    fun packagesFlavorShipsWhereExpected() {
        for (distro in DistroRegistry.all) {
            val hasPackages = BootstrapFlavor.PACKAGES in distro.declaredBootstrapFlavors
            assertEquals(
                "${distro.displayName} (${distro.linuxArch})",
                distro.key == Installation.DISTRO_DEBIAN_SID, hasPackages,
            )
            // Nothing has promoted a non-tarball flavor to supported.
            assertEquals(BootstrapFlavor.TARBALL, distro.supportedFlavor)
        }
    }
}
