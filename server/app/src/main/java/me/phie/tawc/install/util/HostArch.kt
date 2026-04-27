package me.phie.tawc.install.util

import android.os.Build

/**
 * Helpers for inspecting the host (Android) CPU architecture.
 *
 * The two namespaces in play:
 *
 * - **Android ABI** — what `Build.SUPPORTED_ABIS` reports
 *   (`"arm64-v8a"`, `"x86_64"`). Stored in `Installation.arch` for
 *   back-compat with existing on-disk metadata.
 * - **Linux uname -m** — what a chroot-side `uname -m` returns
 *   (`"aarch64"`, `"x86_64"`). Used in tarball URLs, pacman's `$arch`
 *   substitution, keyring names, and anywhere we present the
 *   architecture to a Linux audience (UI labels included).
 *
 * The mapping lives here so callers don't reinvent it. The fallback
 * `"arm64-v8a"` mirrors the original duplicated `primaryArch()` helpers
 * in `ArchInstaller` and `InstallActivity` — phones without
 * SUPPORTED_ABIS reporting are extremely unlikely, but the fallback
 * keeps the install gate from crashing.
 */
object HostArch {
    /** Primary Android ABI of the running device. */
    fun primaryAbi(): String =
        Build.SUPPORTED_ABIS.firstOrNull() ?: "arm64-v8a"

    /** Linux `uname -m` name corresponding to an Android ABI. */
    fun linuxArchFor(abi: String): String? = when (abi) {
        "x86_64" -> "x86_64"
        "arm64-v8a" -> "aarch64"
        else -> null
    }
}
