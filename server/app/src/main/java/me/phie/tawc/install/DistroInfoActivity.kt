package me.phie.tawc.install

import android.app.Activity
import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.text.format.Formatter
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Per-installation detail screen. Shows id/distro/arch/method/source
 * URL/installed-at/full rootfs path, kicks off an async `du -sk`-via-su
 * to fill in size, and exposes the Delete button that opens
 * [UninstallActivity]. Reached by tapping a row on the home screen;
 * size lives here (not on the home list) so opening the launcher
 * doesn't pay the multi-second su cost per row.
 */
class DistroInfoActivity : Activity() {

    private val store by lazy { InstallationStore(this) }
    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var sizeValue: TextView
    private var sizeScope: CoroutineScope? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH

        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
        }

        val installation = store.load(targetId)
        val titleText = installation?.let {
            "${it.distro.replaceFirstChar { c -> c.titlecase() }} (${it.arch})"
        } ?: targetId

        TextView(this).apply {
            text = titleText
            textSize = 22f
            gravity = Gravity.START
        }.also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad)) }

        if (installation == null) {
            TextView(this).apply { text = "Installation '$targetId' not found." }
                .also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT)) }
            setContentView(root)
            return
        }

        root.addView(infoRow("ID:", installation.id), rowLp(pad))
        root.addView(infoRow("Distro:", installation.distro), rowLp(pad))
        root.addView(infoRow("Architecture:", installation.arch), rowLp(pad))
        root.addView(infoRow("Method:", installation.method), rowLp(pad))
        root.addView(infoRow("Source:", installation.sourceUrl), rowLp(pad))
        root.addView(
            infoRow("Installed:", DateFormat.getDateTimeInstance().format(Date(installation.installedAtMillis))),
            rowLp(pad),
        )
        root.addView(
            infoRow("Rootfs path:", store.rootfsDir(installation.id).absolutePath),
            rowLp(pad),
        )
        sizeValue = TextView(this).apply {
            text = "computing…"
            textSize = 14f
            typeface = Typeface.MONOSPACE
        }
        root.addView(infoRowWithValue("Size:", sizeValue), rowLp(pad))

        Button(this).apply {
            text = "Delete"
            setOnClickListener {
                val i = Intent(this@DistroInfoActivity, UninstallActivity::class.java)
                    .putExtra(UninstallActivity.EXTRA_ID, installation.id)
                startActivity(i)
            }
        }.also { root.addView(it, lp(MATCH_PARENT, WRAP_CONTENT)) }

        setContentView(root)
    }

    override fun onResume() {
        super.onResume()
        if (store.load(targetId) == null) {
            // Uninstall happened in a child activity while we were paused;
            // there's nothing to show so back out to the home screen.
            finish()
            return
        }
        startSizeProbe()
    }

    override fun onPause() {
        super.onPause()
        sizeScope?.cancel()
        sizeScope = null
    }

    private fun startSizeProbe() {
        sizeScope?.cancel()
        val cs = CoroutineScope(Dispatchers.Main)
        sizeScope = cs
        sizeValue.text = "computing…"
        cs.launch {
            val bytes = withContext(Dispatchers.IO) { store.computeSizeBytes(targetId) }
            sizeValue.text = when {
                bytes < 0 -> "?"
                else -> Formatter.formatFileSize(this@DistroInfoActivity, bytes)
            }
        }
    }

    private fun infoRow(label: String, value: String): LinearLayout =
        infoRowWithValue(label, TextView(this).apply {
            text = value
            textSize = 14f
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        })

    private fun infoRowWithValue(label: String, valueView: TextView): LinearLayout {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val l = TextView(this).apply { text = label; textSize = 14f }
        row.addView(l, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginEnd = 16 })
        row.addView(valueView, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        return row
    }

    private fun rowLp(pad: Int): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.bottomMargin = pad / 2 }

    private fun lp(w: Int, h: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
        LinearLayout.LayoutParams(w, h).also { it.bottomMargin = bottomMargin }

    companion object {
        const val EXTRA_ID = "id"
    }
}
