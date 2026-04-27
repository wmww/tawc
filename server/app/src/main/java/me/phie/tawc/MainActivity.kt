package me.phie.tawc

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore

/**
 * Home screen for the tawc app. Starts the [CompositorService] (which
 * spawns the Rust compositor thread + Wayland socket), then renders a
 * tappable list of currently-installed Linux environments and a button
 * to install a new one. Each row opens [DistroInfoActivity] for the
 * full path / size / Delete UI — the home screen deliberately doesn't
 * compute size (`du -sk` over a multi-GB rootfs costs seconds via su)
 * so opening the launcher stays snappy.
 */
class MainActivity : Activity() {

    private val store by lazy { InstallationStore(this) }
    private val rowMargin by lazy { (8 * resources.displayMetrics.density).toInt() }

    private lateinit var listContainer: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Compositor is foreground/sticky and outlives this Activity; the
        // launcher tap is the natural place to ensure it's running.
        startForegroundService(Intent(this, CompositorService::class.java))

        val pad = (16 * resources.displayMetrics.density).toInt()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
        }

        TextView(this).apply {
            text = "tawc"
            textSize = 32f
            gravity = Gravity.START
        }.also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad)) }

        TextView(this).apply {
            text = "Installations"
            textSize = 18f
        }.also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2)) }

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(listContainer, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        Button(this).apply {
            text = "Install new distro"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, InstallActivity::class.java))
            }
        }.also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT)) }

        setContentView(root)
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
                .also { listContainer.addView(it, lp(MATCH_PARENT, WRAP_CONTENT)) }
            return
        }
        for (inst in installs) {
            listContainer.addView(buildRow(inst), lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = rowMargin))
        }
    }

    private fun buildRow(inst: Installation): TextView {
        // Whole row is tappable — opens DistroInfoActivity for full
        // details (path, size, delete). Use the platform's selectable
        // background so the tap target gets visible feedback.
        val attrs = intArrayOf(android.R.attr.selectableItemBackground)
        val ta = obtainStyledAttributes(attrs)
        val bg = ta.getResourceId(0, 0)
        ta.recycle()

        return TextView(this).apply {
            text = displayName(inst)
            textSize = 16f
            val vpad = (12 * resources.displayMetrics.density).toInt()
            setPadding(0, vpad, 0, vpad)
            if (bg != 0) setBackgroundResource(bg)
            isClickable = true
            isFocusable = true
            setOnClickListener {
                val i = Intent(this@MainActivity, DistroInfoActivity::class.java)
                    .putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }
    }

    private fun displayName(inst: Installation): String {
        val distro = inst.distro.replaceFirstChar { it.titlecase() }
        return "$distro (${inst.arch}) ›"
    }

    private fun lp(w: Int, h: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(w, h).also { it.bottomMargin = bottomMargin }
}
