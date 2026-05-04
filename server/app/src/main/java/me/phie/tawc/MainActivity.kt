package me.phie.tawc

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.card.MaterialCardView
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
 * tappable card for each currently-installed Linux environment and a
 * button to install a new one. Each card opens [DistroInfoActivity] for
 * the full path / size / Uninstall UI — the home screen deliberately
 * doesn't compute size (`du -sk` over a multi-GB rootfs costs seconds
 * via su) so opening the launcher stays snappy.
 */
class MainActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private val cardMargin by lazy { (8 * resources.displayMetrics.density).toInt() }
    private val cardPad by lazy { (16 * resources.displayMetrics.density).toInt() }

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
        for (inst in store.list()) {
            listContainer.addView(buildCard(inst), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))
        }
    }

    private fun buildCard(inst: Installation): View {
        val card = MaterialCardView(this).apply {
            isClickable = true
            isFocusable = true
            setOnClickListener {
                val i = Intent(this@MainActivity, DistroInfoActivity::class.java)
                    .putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }
        val (title, subtitle) = displayParts(inst)
        column.addView(TextView(this).apply {
            text = title
            textSize = 18f
        })
        if (subtitle.isNotEmpty()) {
            column.addView(TextView(this).apply {
                text = subtitle
                textSize = 14f
                alpha = 0.7f
            })
        }
        card.addView(column)
        return card
    }

    /**
     * Split an installation into (card title, card subtitle).
     *
     * Title is the user-set label (which defaults to `distro.defaultLabel`
     * at install time, e.g. "Arch"). Subtitle is the registry-resolved
     * display name (e.g. "Arch Linux (x86)") plus any non-READY state
     * marker. If the title already equals the display name (legacy
     * records without a label), drop the redundant distro subtitle and
     * keep only the state marker (if any).
     */
    private fun displayParts(inst: Installation): Pair<String, String> {
        val resolved = DistroRegistry.forInstallation(inst)
        val displayName = resolved?.displayName
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"
        val title = inst.label ?: displayName
        val distroLine = if (title == displayName) "" else displayName
        val stateLine = when (inst.state) {
            Installation.State.READY -> ""
            Installation.State.INSTALLING -> "installing…"
            Installation.State.UNINSTALLING -> "uninstalling…"
            Installation.State.FAILED -> "failed"
        }
        val subtitle = listOf(distroLine, stateLine).filter { it.isNotEmpty() }.joinToString(" — ")
        return title to subtitle
    }
}
