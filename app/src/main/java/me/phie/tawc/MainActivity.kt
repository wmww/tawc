package me.phie.tawc

import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.launcher.LauncherActivity
import me.phie.tawc.tasks.TaskManagerActivity
import me.phie.tawc.ui.buildHomeScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp

/**
 * Home screen for the tawc app. Starts the [CompositorService] (which
 * spawns the Rust compositor thread + Wayland socket), then renders a
 * card for each currently-installed Linux environment with two actions:
 * Info (opens [DistroInfoActivity]) and Run (opens [LauncherActivity]
 * to pick an app). The home screen deliberately doesn't compute rootfs
 * size — `du -sk` over a multi-GB tree costs seconds via su, and DistroInfo
 * is the place that needs it.
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
        startForegroundService(Intent(this, CompositorService::class.java))

        val scaffold = buildHomeScreen("TAWC")

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        scaffold.content.addView(listContainer, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))

        scaffold.content.addView(
            tonalButton("Task manager") {
                startActivity(Intent(this@MainActivity, TaskManagerActivity::class.java))
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
        )

        scaffold.content.addView(
            tonalButton("Settings") {
                startActivity(Intent(this@MainActivity, SettingsActivity::class.java))
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
        )

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
        val installations = store.list()
        if (installations.isEmpty()) {
            listContainer.addView(TextView(this).apply {
                text = "You have no installed Linux distros, install one to get started."
                textSize = 16f
                alpha = 0.75f
                gravity = Gravity.CENTER_HORIZONTAL
            }, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))
            return
        }
        for (inst in installations) {
            listContainer.addView(buildCard(inst), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))
        }
    }

    private fun buildCard(inst: Installation): View {
        val card = tawcCard()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }

        // Header: title/subtitle on the left, Manage button on the top-right.
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val textCol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val (title, subtitle) = displayParts(inst)
        textCol.addView(TextView(this).apply {
            text = title
            textSize = 18f
        })
        if (subtitle.isNotEmpty()) {
            textCol.addView(TextView(this).apply {
                text = subtitle
                textSize = 14f
                alpha = 0.7f
            })
        }
        val gap = (8 * resources.displayMetrics.density).toInt()
        header.addView(
            textCol,
            LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).also { it.marginEnd = gap },
        )
        header.addView(
            tonalButton("Manage") {
                val i = Intent(this@MainActivity, DistroInfoActivity::class.java)
                    .putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
                startActivity(i)
            },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).also { it.gravity = Gravity.TOP },
        )
        column.addView(header, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Search-apps stub: looks like a search field but never holds focus
        // — tapping it forwards into LauncherActivity, which is the real
        // search UI. Hidden on FAILED (no usable launcher) and disabled
        // while installing/uninstalling so it returns once ready.
        val topMargin = (4 * resources.displayMetrics.density).toInt()
        val searchBox = EditText(this).apply {
            hint = "Search apps"
            isSingleLine = true
            isFocusable = false
            isClickable = true
            setOnClickListener {
                val i = Intent(this@MainActivity, LauncherActivity::class.java)
                    .putExtra(LauncherActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }
        searchBox.visibility = if (inst.state == Installation.State.FAILED) View.GONE else View.VISIBLE
        searchBox.isEnabled = inst.state == Installation.State.READY
        column.addView(
            searchBox,
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = topMargin },
        )

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
