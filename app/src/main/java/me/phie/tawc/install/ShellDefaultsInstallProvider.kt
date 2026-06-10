package me.phie.tawc.install

import android.content.Context
import java.io.File

/**
 * Ships [ShellDefaults.GUEST_BASHRC_PATH] (`/usr/lib/tawc/bashrc`;
 * prompt + color aliases)
 * into every rootfs. Content lives in [ShellDefaults] as a Kotlin
 * constant; entries() materialises it under filesDir so the generic
 * COPY machinery in [TawcInstaller] applies. Refreshed on app upgrade
 * like every other provider, which is the point: defaults changes
 * reach existing installs. Users opt out per-rootfs by removing the
 * source line from their own ~/.bashrc (see [ShellDefaults]).
 */
internal object ShellDefaultsInstallProvider : TawcInstallProvider {
    override val name: String = "shell-defaults"

    override fun entries(context: Context): List<TawcInstall> {
        val src = File(context.filesDir, "shell-defaults/bashrc")
        src.parentFile?.mkdirs()
        src.writeText(ShellDefaults.GUEST_BASHRC_CONTENT)
        return listOf(
            TawcInstall(
                src = src.absolutePath,
                dest = ShellDefaults.GUEST_BASHRC_PATH,
                type = TawcInstall.Type.COPY,
            ),
        )
    }
}
