package me.phie.tawc.install

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.text.format.Formatter
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import android.content.DialogInterface
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import me.phie.tawc.R
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.destructiveButton
import me.phie.tawc.ui.verticalLp

/**
 * Per-installation detail screen. Shows id/distro/arch/method/source
 * URL/installed-at/full rootfs path, kicks off an async `du -sk`-via-su
 * to fill in size, and exposes the (red, destructive) Uninstall button
 * that opens [UninstallActivity]. Reached by tapping a row on the home
 * screen; size lives here (not on the home list) so opening the
 * launcher doesn't pay the multi-second su cost per row.
 */
class DistroInfoActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var sizeValue: TextView
    private var sizeScope: CoroutineScope? = null

    private lateinit var scaffold: me.phie.tawc.ui.Scaffold

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        targetId = intent?.getStringExtra(EXTRA_ID) ?: Installation.DISTRO_ARCH
        scaffold = buildChildScreen(targetId)
        setContentView(scaffold.root)
        // Defer all view population to onResume so a returning trip
        // from UninstallActivity (which may have flipped the slot
        // INSTALLING/READY → FAILED on cancel) re-reads metadata and
        // re-renders the right state row, button, and size probe.
    }

    override fun onResume() {
        super.onResume()
        val installation = store.load(targetId)
        if (installation == null) {
            // Uninstall happened in a child activity while we were paused;
            // there's nothing to show so back out to the home screen.
            finish()
            return
        }
        renderContent(installation)
        if (installation.state == Installation.State.READY ||
            installation.state == Installation.State.FAILED) {
            startSizeProbe()
        }
    }

    override fun onPause() {
        super.onPause()
        sizeScope?.cancel()
        sizeScope = null
    }

    private fun renderContent(installation: Installation) {
        val resolvedDistro: Distro? = DistroRegistry.forInstallation(installation)
        val titleText = installation.label
            ?: resolvedDistro?.displayName
            ?: targetId
        scaffold.toolbar.title = titleText

        val pad = (16 * resources.displayMetrics.density).toInt()
        val content = scaffold.content
        content.removeAllViews()

        content.addView(infoRow("ID:", installation.id), rowLp(pad))
        if (installation.label != null) {
            content.addView(infoRow("Label:", installation.label), rowLp(pad))
        }
        content.addView(
            infoRow("Distro:", resolvedDistro?.displayName ?: installation.distro),
            rowLp(pad),
        )
        content.addView(
            infoRow("Architecture:", resolvedDistro?.linuxArch ?: installation.arch),
            rowLp(pad),
        )
        content.addView(infoRow("Method:", installation.method), rowLp(pad))
        content.addView(infoRow("State:", installation.state.name.lowercase()), rowLp(pad))
        if (installation.failure != null) {
            content.addView(infoRow("Failure:", installation.failure), rowLp(pad))
        }
        content.addView(infoRow("Source:", installation.sourceUrl), rowLp(pad))
        content.addView(
            infoRow("Installed:", DateFormat.getDateTimeInstance().format(Date(installation.installedAtMillis))),
            rowLp(pad),
        )
        content.addView(
            infoRow(
                "App version at install:",
                if (installation.installedAtAppVersionCode > 0) {
                    installation.installedAtAppVersionCode.toString()
                } else "unknown",
            ),
            rowLp(pad),
        )
        content.addView(
            infoRow("Rootfs path:", store.rootfsDir(installation.id).absolutePath),
            rowLp(pad),
        )
        // READY and FAILED slots both have stable on-disk content
        // worth measuring — FAILED in particular is exactly when the
        // user wants to know how much space the half-installed
        // rootfs is sitting on. INSTALLING / UNINSTALLING are skipped
        // because `du -sk` would fight the installer for IO and the
        // number changes faster than we can render it.
        val canProbeSize = installation.state == Installation.State.READY ||
            installation.state == Installation.State.FAILED
        sizeValue = TextView(this).apply {
            text = if (canProbeSize) "computing…" else "—"
            textSize = 14f
            typeface = Typeface.MONOSPACE
        }
        content.addView(infoRowWithValue("Size:", sizeValue), rowLp(pad))

        // Push Uninstall to the bottom with a flexible spacer so it
        // doesn't crowd the info rows.
        content.addView(
            android.view.View(this),
            LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f),
        )
        content.addView(
            destructiveButton("Delete") { confirmUninstall(installation) },
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )
    }

    private fun confirmUninstall(installation: Installation) {
        val name = renderDistroLabel(installation)
        val message = "This permanently deletes the rootfs at\n" +
            "${store.rootfsDir(installation.id).absolutePath},\n" +
            "including all files in your Linux home directory."
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle("Delete $name?")
            .setMessage(message)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Delete") { _, _ ->
                // autoStart=true is the explicit trigger contract —
                // UninstallActivity won't kick off the operation on a
                // bare launch. The dialog "Delete" press is the user's
                // confirmation, so we promote it to an autoStart intent.
                val i = Intent(this, UninstallActivity::class.java)
                    .putExtra(UninstallActivity.EXTRA_ID, installation.id)
                    .putExtra(EXTRA_AUTO_START, true)
                startActivity(i)
            }
            .show()
        // Tint the destructive action red so it pops, and the Cancel
        // neutral so it doesn't compete with it. Default Material3 uses
        // colorPrimary (yellow-orange) for both, which made Cancel look
        // like the recommended path next to a red Delete.
        dialog.getButton(DialogInterface.BUTTON_POSITIVE)?.setTextColor(getColor(R.color.tawc_danger))
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
            btn.setTextColor(
                MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
        }
    }

    private fun startSizeProbe() {
        sizeScope?.cancel()
        val cs = CoroutineScope(Dispatchers.Main)
        sizeScope = cs
        sizeValue.text = "computing…"
        cs.launch {
            // `runInterruptible` maps coroutine cancellation onto thread
            // interrupt; Su.run catches that and `destroyForcibly`s the
            // child `su` so a backgrounded `du -sk` doesn't keep
            // pounding storage after the user leaves this screen.
            val bytes = runInterruptible(Dispatchers.IO) { store.computeSizeBytes(targetId) }
            sizeValue.text = when {
                bytes < 0 -> "?"
                else -> Formatter.formatFileSize(this@DistroInfoActivity, bytes)
            }
        }
    }

    /**
     * Friendly name for the installation: the user's label if set,
     * else the registry-resolved display name (which already carries
     * the ARM/(x86) disambiguator), else a raw "<distro> (<arch>)"
     * fall-back for unknown-distro records.
     */
    private fun renderDistroLabel(installation: Installation): String {
        val resolved = DistroRegistry.forInstallation(installation)
        return installation.label
            ?: resolved?.displayName
            ?: "${installation.distro.replaceFirstChar { it.titlecase() }} (${installation.arch})"
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

    companion object {
        const val EXTRA_ID = "id"
    }
}
