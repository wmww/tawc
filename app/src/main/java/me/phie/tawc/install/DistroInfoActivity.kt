package me.phie.tawc.install

import android.graphics.Typeface
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.text.InputType
import android.text.format.Formatter
import android.view.KeyEvent
import android.view.WindowManager
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import android.content.DialogInterface
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import me.phie.tawc.AndoBrokers
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
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp

/**
 * Per-installation detail screen. Shows label/distro/arch/method/source
 * URL/installed-at/full rootfs path, kicks off an async `du -sk`-via-su
 * to fill in size, and exposes the (red, destructive) Delete button
 * (Are-You-Sure dialog → [InstallationService.startUninstall] +
 * [me.phie.tawc.ops.LogScreenActivity] for the live progress view).
 * Reached by tapping a row on the home screen; size lives here (not
 * on the home list) so opening the launcher doesn't pay the multi-
 * second su cost per row.
 */
class DistroInfoActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var targetId: String = Installation.DISTRO_ARCH

    private lateinit var sizeValue: TextView
    private var sizeScope: CoroutineScope? = null

    /** Serializes ando toggle commits so rapid taps land in click
     *  order (see [buildAndoRow]). */
    private val andoCommitExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()

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

    override fun onDestroy() {
        super.onDestroy()
        // Queued commits still run; shutdown() only stops new ones and
        // lets the worker thread exit, so the executor doesn't outlive
        // the activity.
        andoCommitExecutor.shutdown()
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
        content.addView(infoRow(getString(R.string.distro_info_row_state), stateLabel(installation.state)), rowLp(pad))
        if (installation.failure != null) {
            content.addView(infoRow(getString(R.string.distro_info_row_failure), installation.failure), rowLp(pad))
        }
        content.addView(infoRow(getString(R.string.distro_info_row_source), installation.sourceUrl), rowLp(pad))
        content.addView(
            infoRow(
                getString(R.string.distro_info_row_installed),
                DateFormat.getDateTimeInstance().format(Date(installation.installedAtMillis)),
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
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
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
        // READY and FAILED slots both have stable on-disk content
        // worth measuring — FAILED in particular is exactly when the
        // user wants to know how much space the half-installed
        // rootfs is sitting on. INSTALLING / UNINSTALLING are skipped
        // because `du -sk` would fight the installer for IO and the
        // number changes faster than we can render it.
        val canProbeSize = installation.state == Installation.State.READY ||
            installation.state == Installation.State.FAILED
        sizeValue = TextView(this).apply {
            text = if (canProbeSize) {
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
        // ando toggle (notes/ando.md). Applies to ALL install methods
        // (unlike binds), state-gated to READY/FAILED like manage binds
        // so a mid-mutation edit can't race the service's writes. Above
        // the manage-binds button.
        if (installation.state == Installation.State.READY ||
            installation.state == Installation.State.FAILED
        ) {
            content.addView(
                buildAndoRow(installation),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
            )
        }
        // Manage binds is gated like Run plus FAILED: editing binds is
        // exactly how the user recovers a slot that failed closed on a
        // bad bind, but a mid-INSTALLING/UNINSTALLING edit would race
        // the service's own metadata writes. Tawcroot-only — the other
        // methods don't consume the list — and hidden when this build
        // doesn't ship all-files access.
        if (installation.method == TawcrootMethod.KEY && AllFilesAccess.declared(this) &&
            (installation.state == Installation.State.READY ||
                installation.state == Installation.State.FAILED)
        ) {
            content.addView(
                tonalButton(getString(R.string.distro_info_manage_binds)) {
                    startActivity(ManageBindsActivity.intentForInstall(this, installation.id))
                },
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
            )
        }
        // Run is gated on READY — the other states either have no
        // rootfs to enter (no dir, INSTALLING) or are mid-mutation
        // (UNINSTALLING) or are likely broken (FAILED).
        if (installation.state == Installation.State.READY) {
            content.addView(
                tonalButton(getString(R.string.action_run)) { showRunDialog(installation) },
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
            )
        }
        content.addView(
            destructiveButton(getString(R.string.action_delete)) { confirmUninstall(installation) },
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )
    }

    /**
     * ando toggle row ([buildAndoToggleRow], notes/ando.md). Toggling
     * read-modify-writes [Installation.andoEnabled] through [store] on
     * [andoCommitExecutor] — single-threaded so rapid taps commit in
     * click order instead of racing each other — re-checking the state
     * gate at write time (mirrors [ManageBindsActivity.commit]) so a
     * slot that slipped into INSTALLING/UNINSTALLING under us is left
     * alone, then reconciles the broker via [AndoBrokers.refresh].
     * Take-effect: disable is immediate (listener down, in-flight ando
     * children killed); enable applies to the next rootfs spawn.
     */
    private fun buildAndoRow(installation: Installation): LinearLayout {
        var reverting = false
        return buildAndoToggleRow(this, installation.andoEnabled) { checkbox, checked ->
            if (reverting) return@buildAndoToggleRow
            andoCommitExecutor.execute {
                // Gate + write atomically under the store's per-id lock:
                // re-read inside [update] so a slot that slipped into
                // INSTALLING/UNINSTALLING (or was uninstalled) under us
                // is left alone rather than racing the service's writes.
                val saved = store.update(installation.id) { current ->
                    if (current.state == Installation.State.READY ||
                        current.state == Installation.State.FAILED
                    ) {
                        current.copy(andoEnabled = checked)
                    } else {
                        null
                    }
                }
                if (saved != null) {
                    AndoBrokers.refresh(this)
                } else {
                    runOnUiThread {
                        Toast.makeText(
                            this,
                            getString(R.string.ando_toggle_enable_failed),
                            Toast.LENGTH_SHORT,
                        ).show()
                        reverting = true
                        checkbox.isChecked = !checked
                        reverting = false
                    }
                }
            }
        }
    }

    private fun showRunDialog(installation: Installation) {
        val pad = (16 * resources.displayMetrics.density).toInt()
        val input = EditText(this).apply {
            hint = getString(R.string.hint_run_command)
            // VISIBLE_PASSWORD is the load-bearing flag for "no autocorrect" —
            // Gboard ignores TYPE_TEXT_FLAG_NO_SUGGESTIONS on a plain CLASS_TEXT
            // field and still autocorrects.
            inputType = InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            isSingleLine = true
            imeOptions = EditorInfo.IME_ACTION_GO
            typeface = Typeface.MONOSPACE
            textSize = 14f
        }
        // Wrap so the EditText gets dialog-edge padding without
        // touching MaterialAlertDialog's own content insets.
        val wrap = FrameLayout(this).apply { setPadding(pad, pad / 2, pad, 0) }
        wrap.addView(input, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(getString(R.string.distro_info_run_command_title, renderDistroLabel(installation)))
            .setView(wrap)
            .setNegativeButton(getString(R.string.action_cancel), null)
            .setPositiveButton(getString(R.string.action_run)) { _, _ ->
                val cmd = input.text.toString().trim()
                if (cmd.isNotEmpty()) RunCommandOp.start(this, installation, cmd)
            }
            .show()
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        // Default Material3 paints both buttons in colorPrimary, which
        // makes Cancel look like a recommended path. Tone it down to
        // colorOnSurfaceVariant so Run reads as the action.
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
            btn.setTextColor(
                MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
        }
        input.setOnEditorActionListener { _, actionId, event ->
            val isEnter = actionId == EditorInfo.IME_ACTION_GO ||
                actionId == EditorInfo.IME_ACTION_DONE ||
                (event?.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
            if (isEnter) {
                dialog.getButton(DialogInterface.BUTTON_POSITIVE)?.performClick()
                true
            } else {
                false
            }
        }
        input.requestFocus()
        input.post {
            input.requestFocus()
            val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.showSoftInput(input, InputMethodManager.SHOW_IMPLICIT)
        }
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
    private fun copyButton(description: String, text: String): ImageButton =
        ImageButton(this).apply {
            setImageResource(R.drawable.ic_content_copy)
            imageTintList = android.content.res.ColorStateList.valueOf(
                MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
            val outValue = android.util.TypedValue()
            context.theme.resolveAttribute(android.R.attr.selectableItemBackgroundBorderless, outValue, true)
            setBackgroundResource(outValue.resourceId)
            contentDescription = description
            setOnClickListener {
                val clipboard = getSystemService(ClipboardManager::class.java)
                clipboard.setPrimaryClip(ClipData.newPlainText(description, text))
                if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.TIRAMISU) {
                    Toast.makeText(context, R.string.copied_to_clipboard, Toast.LENGTH_SHORT).show()
                }
            }
        }

    private fun stateLabel(state: Installation.State): String =
        when (state) {
            Installation.State.READY -> getString(R.string.install_state_ready)
            Installation.State.INSTALLING -> getString(R.string.install_state_installing)
            Installation.State.UNINSTALLING -> getString(R.string.install_state_uninstalling)
            Installation.State.FAILED -> getString(R.string.install_state_failed)
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
