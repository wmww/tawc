package me.phie.tawc

import android.content.Intent
import android.os.Bundle
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.buildHomeScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.verticalLp

/**
 * Home screen for the tawc app. Starts the [CompositorService] (which
 * spawns the Rust compositor thread + Wayland socket), then renders a
 * tappable list of currently-installed Linux environments and a button
 * to install a new one. Each row opens [DistroInfoActivity] for the
 * full path / size / Uninstall UI — the home screen deliberately doesn't
 * compute size (`du -sk` over a multi-GB rootfs costs seconds via su)
 * so opening the launcher stays snappy.
 */
class MainActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private val rowMargin by lazy { (8 * resources.displayMetrics.density).toInt() }
    private val rowPadV by lazy { (12 * resources.displayMetrics.density).toInt() }
    private val rowSelectableBg by lazy {
        val attrs = intArrayOf(android.R.attr.selectableItemBackground)
        val ta = obtainStyledAttributes(attrs)
        try { ta.getResourceId(0, 0) } finally { ta.recycle() }
    }

    private lateinit var listContainer: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Compositor is foreground/sticky and outlives this Activity; the
        // launcher tap is the natural place to ensure it's running.
        // (enter.sh refresh on APK reinstall is handled by
        // TawcApplication.onCreate, which fires on every cold-process
        // start and rewrites every READY install's enter.sh against the
        // new nativeLibraryDir before MainActivity can use it.)
        startForegroundService(Intent(this, CompositorService::class.java))

        val scaffold = buildHomeScreen("tawc")
        val pad = (16 * resources.displayMetrics.density).toInt()

        TextView(this).apply {
            text = "Installations"
            textSize = 18f
        }.also { scaffold.content.addView(it, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2)) }

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        scaffold.content.addView(listContainer, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        scaffold.content.addView(
            primaryButton("Install new distro") {
                startActivity(Intent(this@MainActivity, InstallActivity::class.java))
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )

        setContentView(scaffold.root)
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        listContainer.removeAllViews()
        val installs = store.list()
        if (installs.isEmpty()) {
            TextView(this).apply { text = "(none)" }
                .also { listContainer.addView(it, verticalLp(MATCH_PARENT, WRAP_CONTENT)) }
            return
        }
        for (inst in installs) {
            listContainer.addView(buildRow(inst), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = rowMargin))
        }
    }

    private fun buildRow(inst: Installation): TextView =
        // Whole row is tappable — opens DistroInfoActivity for full
        // details (path, size, uninstall). selectableItemBackground gives
        // the platform-standard tap ripple.
        TextView(this).apply {
            text = displayName(inst)
            textSize = 16f
            setPadding(0, rowPadV, 0, rowPadV)
            if (rowSelectableBg != 0) setBackgroundResource(rowSelectableBg)
            isClickable = true
            isFocusable = true
            setOnClickListener {
                val i = Intent(this@MainActivity, DistroInfoActivity::class.java)
                    .putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }

    private fun displayName(inst: Installation): String {
        // Prefer the registry's canonical display name + Linux arch
        // ("Arch Linux ARM (aarch64)") and fall back to the raw
        // on-disk strings for unknown distros.
        val resolved = DistroRegistry.forInstallation(inst)
        val name = resolved?.displayName ?: inst.distro.replaceFirstChar { it.titlecase() }
        val arch = resolved?.linuxArch ?: inst.arch
        val suffix = when (inst.state) {
            Installation.State.READY -> ""
            Installation.State.INSTALLING -> " — installing…"
            Installation.State.UNINSTALLING -> " — uninstalling…"
            Installation.State.FAILED -> " — failed"
        }
        return "$name ($arch)$suffix ›"
    }
}
