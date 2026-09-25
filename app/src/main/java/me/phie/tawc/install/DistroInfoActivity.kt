package me.phie.tawc.install

import android.graphics.Typeface
import android.content.ClipData
import android.content.ClipboardManager
import android.os.Bundle
import android.text.format.Formatter
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
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
import me.phie.tawc.ops.LogScreenActivity
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.destructiveButton
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.verticalLp

/**
 * Per-installation detail screen. Shows label/distro/arch/method/source
 * URL/installed-at/full rootfs path, kicks off an async `du -sk`-via-su
 * to fill in size, and exposes the (red, destructive) Delete button
 * (Are-You-Sure dialog → [InstallationService.startUninstall] +
 * [me.phie.tawc.ops.LogScreenActivity] for the live progress view).
 * Reached from the home ⋮ menu; size lives here (not on home) so
 * opening the home screen doesn't pay the multi-second su cost.
 * Editable per-install settings live in [me.phie.tawc.SettingsActivity].
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
        // from LogScreenActivity (which may have flipped the slot
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
        if (canProbeSize(installation)) {
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
        scaffold.toolbar.title = DistroRegistry.displayLabel(installation)

        val pad = (16 * resources.displayMetrics.density).toInt()
        val content = scaffold.content
        content.removeAllViews()

        content.addView(
            infoRow(getString(R.string.distro_info_row_label), DistroRegistry.displayLabel(installation)),
            rowLp(pad),
        )
        content.addView(
            infoRow(getString(R.string.distro_info_row_distro), resolvedDistro?.displayName ?: installation.distro),
            rowLp(pad),
        )
        content.addView(
            infoRow(getString(R.string.distro_info_row_architecture), resolvedDistro?.linuxArch ?: installation.arch),
            rowLp(pad),
        )
        content.addView(infoRow(getString(R.string.distro_info_row_method), installation.method), rowLp(pad))
        content.addView(
            infoRow(getString(R.string.distro_info_row_bootstrap), installation.bootstrapFlavor),
            rowLp(pad),
        )
        content.addView(infoRow(getString(R.string.distro_info_row_state), stateLabel(installation.state)), rowLp(pad))
        if (installation.failure != null) {
            content.addView(infoRow(getString(R.string.distro_info_row_failure), installation.failure), rowLp(pad))
        }
        content.addView(infoRow(getString(R.string.distro_info_row_source), installation.sourceUrl), rowLp(pad))
        content.addView(
            infoRow(
                getString(R.string.distro_info_row_installed),
                // 0 means "never recorded" (legacy record or corrupt-
                // metadata marker), not Jan 1 1970.
                if (installation.installedAtMillis > 0) {
                    DateFormat.getDateTimeInstance().format(Date(installation.installedAtMillis))
                } else getString(R.string.distro_info_unknown),
            ),
            rowLp(pad),
        )
        content.addView(
            infoRow(
                getString(R.string.distro_info_row_app_version_at_install),
                if (installation.installedAtAppVersionCode > 0) {
                    installation.installedAtAppVersionCode.toString()
                } else getString(R.string.distro_info_unknown),
            ),
            rowLp(pad),
        )
        val rootfsPath = store.rootfsDir(installation.id).absolutePath
        val rootfsRow = infoRow(getString(R.string.distro_info_row_rootfs_path), rootfsPath)
        rootfsRow.gravity = android.view.Gravity.CENTER_VERTICAL
        rootfsRow.addView(
            copyButton(getString(R.string.action_copy_rootfs_path), rootfsPath),
            LinearLayout.LayoutParams(tawcButtonSizePx(), tawcButtonSizePx()),
        )
        content.addView(rootfsRow, rowLp(pad))
        if (installation.method == TawcrootMethod.KEY && AllFilesAccess.declared(this)) {
            content.addView(
                infoRow(
                    getString(R.string.distro_info_row_external_binds),
                    installation.externalBinds.size.toString(),
                ),
                rowLp(pad),
            )
        }
        sizeValue = TextView(this).apply {
            text = if (canProbeSize(installation)) {
                getString(R.string.distro_info_computing)
            } else {
                getString(R.string.distro_info_size_unavailable)
            }
            textSize = 14f
            typeface = Typeface.MONOSPACE
        }
        content.addView(infoRowWithValue(getString(R.string.distro_info_row_size), sizeValue), rowLp(pad))

        // Push the action buttons to the bottom with a flexible spacer
        // so they don't crowd the info rows.
        content.addView(
            android.view.View(this),
            LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f),
        )
        content.addView(
            destructiveButton(getString(R.string.action_delete)) { confirmUninstall(installation) },
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )
    }

    private fun confirmUninstall(installation: Installation) {
        val name = renderDistroLabel(installation)
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.distro_info_delete_title, name))
            .setMessage(
                getString(R.string.distro_info_delete_message, store.rootfsDir(installation.id).absolutePath)
            )
            .setNegativeButton(getString(R.string.action_cancel), null)
            .setPositiveButton(getString(R.string.action_delete)) { _, _ ->
                // The dialog "Delete" press is the user's confirmation;
                // start the uninstall directly via the service helper
                // and open LogScreenActivity to view it. No intent-
                // extras contract — the service is the single mutation
                // surface.
                InstallationService.startUninstall(this, installation.id)
                startActivity(LogScreenActivity.intentFor(this, "uninstall:${installation.id}"))
            }
            .show()
        // Tint the destructive action red so it pops, and the Cancel
        // neutral so it doesn't compete with it. Default Material3 uses
        // colorPrimary for both, which made Cancel look like the
        // recommended path next to a red Delete.
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
        sizeValue.text = getString(R.string.distro_info_computing)
        cs.launch {
            // `runInterruptible` maps coroutine cancellation onto thread
            // interrupt; Su.run catches that and `destroyForcibly`s the
            // child `su` so a backgrounded `du -sk` doesn't keep
            // pounding storage after the user leaves this screen.
            val bytes = runInterruptible(Dispatchers.IO) { store.computeSizeBytes(targetId) }
            sizeValue.text = when {
                bytes < 0 -> getString(R.string.distro_info_unknown)
                else -> Formatter.formatFileSize(this@DistroInfoActivity, bytes)
            }
        }
    }

    private fun renderDistroLabel(installation: Installation): String =
        DistroRegistry.displayLabel(installation)

    /**
     * Borderless clipboard icon button. Skips the "copied" toast on
     * T+ where the system already shows its own clipboard overlay.
     */
    private fun copyButton(description: String, text: String): View =
        plainIconButton(R.drawable.ic_content_copy, description) {
            val clipboard = getSystemService(ClipboardManager::class.java)
            clipboard.setPrimaryClip(ClipData.newPlainText(description, text))
            if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
                Toast.makeText(this, R.string.copied_to_clipboard, Toast.LENGTH_SHORT).show()
            }
        }

    private fun stateLabel(state: Installation.State): String =
        when (state) {
            Installation.State.READY -> getString(R.string.install_state_ready)
            Installation.State.INSTALLING -> getString(R.string.install_state_installing)
            Installation.State.UNINSTALLING -> getString(R.string.install_state_uninstalling)
            Installation.State.FAILED -> getString(R.string.install_state_failed)
            Installation.State.CORRUPT -> getString(R.string.install_state_corrupt)
        }

    /**
     * READY, FAILED, and CORRUPT slots all have stable on-disk content
     * worth measuring — the broken states in particular are exactly
     * when the user wants to know how much space the slot is sitting
     * on. INSTALLING / UNINSTALLING are skipped because `du -sk` would
     * fight the installer for IO and the number changes faster than we
     * can render it.
     */
    private fun canProbeSize(installation: Installation): Boolean =
        installation.state == Installation.State.READY ||
            installation.state == Installation.State.FAILED ||
            installation.state == Installation.State.CORRUPT

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
