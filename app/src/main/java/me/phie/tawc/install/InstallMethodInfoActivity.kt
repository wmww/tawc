package me.phie.tawc.install

import android.graphics.Typeface
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.verticalLp

/**
 * Read-only reference page describing the install methods this APK
 * ships ([EnabledMethods]). Linked from the install form when more
 * than one method is enabled — the single-method case skips the link
 * since there's nothing to compare. Content lives here (in code, not
 * `notes/installation.md`) so it ships in the APK and is reachable
 * offline.
 */
class InstallMethodInfoActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen("Install methods")
        val pad = (16 * resources.displayMetrics.density).toInt()
        val content = scaffold.content

        if (EnabledMethods.tawcroot) {
            section(
                content, pad,
                "TAWCroot (recommended)",
                "Custom systrap-based syscall emulator written for TAWC. Like " +
                    "proot it needs no root, but uses a more efficient " +
                    "ptrace alternative and is built specifically to handle " +
                    "the Wayland / GPU paths the compositor needs. Default " +
                    "for new installs and the only officially supported " +
                    "method.",
            )
        }
        if (EnabledMethods.proot) {
            section(
                content, pad,
                "proot (dev only)",
                "Userspace chroot via the Termux fork of proot. Rootless and " +
                    "broadly compatible — pacman, package builds and most " +
                    "desktop apps work. Slower than TAWCroot because every " +
                    "syscall is intercepted via ptrace. Available in debug " +
                    "builds for comparison; not shipped to release users.",
            )
        }
        if (EnabledMethods.chroot) {
            section(
                content, pad,
                "chroot (requires root, dev only)",
                "Real Linux chroot via su. Fastest path with no syscall " +
                    "translation overhead, but only works on rooted devices " +
                    "(Magisk / Phh-style su). The chroot mounts /apex, " +
                    "/vendor, /system and /linkerconfig from Android so " +
                    "libhybris GPU drivers can resolve their dependencies. " +
                    "Available in debug builds; not shipped to release users.",
            )
        }

        setContentView(scaffold.root)
    }

    private fun section(parent: LinearLayout, pad: Int, title: String, body: String) {
        TextView(this).apply {
            text = title
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
        }.also { parent.addView(it, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 4)) }

        TextView(this).apply {
            text = body
            textSize = 14f
        }.also { parent.addView(it, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad)) }
    }
}
