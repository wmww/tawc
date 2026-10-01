package me.phie.tawc.install.distro

import me.phie.tawc.install.Installation
import me.phie.tawc.install.distro.arch.ArchLinuxArm
import me.phie.tawc.install.distro.arch.ArchLinuxX86_64
import me.phie.tawc.install.distro.debian.DebianSidAarch64
import me.phie.tawc.install.distro.debian.DebianSidX86_64
import me.phie.tawc.install.distro.manjaro.ManjaroArm
import me.phie.tawc.install.distro.ubuntu.Ubuntu2404Aarch64
import me.phie.tawc.install.distro.ubuntu.Ubuntu2404X86_64
import me.phie.tawc.install.distro.voidlinux.VoidLinuxAarch64
import me.phie.tawc.install.distro.voidlinux.VoidLinuxX86_64
import me.phie.tawc.install.util.HostArch

/**
 * Catalogue of supported [Distro] implementations and the only place
 * that maps `(metadata.distro, metadata.arch)` to a concrete instance.
 *
 * Existing on-disk records use `distro = "arch"` for both Arch Linux
 * and Arch Linux ARM, so disambiguation is by Android ABI.
 */
object DistroRegistry {
    val all: List<Distro> = listOf(
        ArchLinuxX86_64,
        ArchLinuxArm,
        ManjaroArm,
        VoidLinuxX86_64,
        VoidLinuxAarch64,
        DebianSidX86_64,
        DebianSidAarch64,
        Ubuntu2404X86_64,
        Ubuntu2404Aarch64,
    )

    /**
     * Resolve [inst]'s metadata back to the implementation. Returns
     * null if the (distro, arch) pair has no match — the caller should
     * surface this as a refused operation rather than silently
     * succeeding.
     */
    fun forInstallation(inst: Installation): Distro? =
        all.firstOrNull { it.key == inst.distro && it.androidAbi == inst.arch }

    /**
     * User-facing name for an installation: the user's label if set,
     * else the distro's default label (what the install form would
     * have pre-filled, e.g. "Arch") for legacy records that predate
     * the label field, else a raw "<distro> (<arch>)" fall-back for
     * unknown-distro records.
     */
    fun displayLabel(inst: Installation): String =
        inst.label
            ?: forInstallation(inst)?.defaultLabel
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"

    /**
     * Distros that can be installed on this host (matching the host's
     * primary Android ABI), supported ones first. The install activity
     * uses this for its distro radio; the service uses [forKey] to
     * resolve the user's pick.
     */
    fun availableForHost(): List<Distro> =
        all.filter { it.androidAbi == HostArch.primaryAbi() }
            .sortedByDescending { it.supported }

    /**
     * The distros we actually support for users (see [Distro.supported]),
     * installable on this host. The install form lists these directly;
     * [otherForHost] goes behind an expander.
     */
    fun supportedForHost(): List<Distro> = availableForHost().filter { it.supported }

    /** Host-installable distros that are shipped but not supported. */
    fun otherForHost(): List<Distro> = availableForHost().filterNot { it.supported }

    /**
     * Distro auto-selected for a fresh install on this host when the
     * caller doesn't specify one. First match wins — used by the
     * broker `install` action when the caller omits `distro=…`, and
     * by the in-app form on a single-distro host where the radio
     * group isn't rendered.
     */
    fun defaultForHost(): Distro? = availableForHost().firstOrNull()

    /**
     * Resolve a `(key, host-abi)` pair to a Distro. Used by
     * [me.phie.tawc.install.InstallationService] to validate the
     * `--es distro` install extra against the device's actual ABI.
     */
    fun forKey(distroKey: String): Distro? =
        availableForHost().firstOrNull { it.key == distroKey }
}
