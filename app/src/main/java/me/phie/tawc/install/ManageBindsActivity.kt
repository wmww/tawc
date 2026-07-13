package me.phie.tawc.install

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import me.phie.tawc.R
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.tonalIconButton
import me.phie.tawc.ui.verticalLp
import org.json.JSONArray

/**
 * Add/edit/remove screen for an install's [ExternalBind] list
 * (notes/external-binds.md). Two callers, two modes:
 *
 *   - [EXTRA_INSTALL_ID] (from [DistroInfoActivity]): edits the
 *     persisted metadata directly — every mutation saves through
 *     [InstallationStore].
 *   - [EXTRA_BINDS] (from [InstallActivity], pre-install): edits an
 *     in-memory list and publishes every mutation via
 *     `setResult(RESULT_OK, [EXTRA_BINDS] = json)` so backing out at
 *     any point hands the caller the latest list.
 *
 * Guest paths are typed; host paths come from [DirectoryPickerActivity]
 * (or are typed — the picker can't reach what it can't read, e.g. a
 * path that needs the not-yet-granted all-files access).
 */
class ManageBindsActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private var installId: String? = null
    private val binds = mutableListOf<ExternalBind>()

    private lateinit var scaffold: me.phie.tawc.ui.Scaffold
    private lateinit var listColumn: LinearLayout
    private lateinit var grantBanner: LinearLayout

    /** Host-path field of the currently open add/edit dialog, where the
     * directory picker's result lands. Null when no dialog is open. */
    private var pickerTargetField: EditText? = null
    private val pickHostDir = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        val path = result.data?.getStringExtra(DirectoryPickerActivity.EXTRA_PATH)
        if (result.resultCode == Activity.RESULT_OK && path != null) {
            pickerTargetField?.setText(path)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installId = intent?.getStringExtra(EXTRA_INSTALL_ID)

        binds.clear()
        binds.addAll(loadInitialBinds(savedInstanceState?.getString(KEY_BINDS)))
        // Recreation (e.g. rotation) clears any result set by the old
        // instance; re-arm it or edits made before the config change
        // would vanish if the user backs out without further edits.
        if (installId == null) publishResult()

        scaffold = buildChildScreen(getString(R.string.title_manage_binds))
        val pad = (16 * resources.displayMetrics.density).toInt()

        scaffold.content.addView(
            TextView(this).apply {
                text = getString(R.string.manage_binds_intro)
                textSize = 14f
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )

        grantBanner = buildGrantBanner(pad)
        scaffold.content.addView(grantBanner, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        listColumn = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val scroll = ScrollView(this).apply {
            isFillViewport = true
            addView(listColumn, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        }
        scaffold.content.addView(scroll, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        scaffold.content.addView(
            primaryButton(getString(R.string.manage_binds_add)) { showEditDialog(null) },
            verticalLp(MATCH_PARENT, WRAP_CONTENT),
        )

        setContentView(scaffold.root)
        renderList()
    }

    override fun onResume() {
        super.onResume()
        // Grant state may have flipped during a settings round-trip.
        updateGrantBanner()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_BINDS, ExternalBind.toJsonArray(binds).toString())
    }

    private fun loadInitialBinds(savedJson: String?): List<ExternalBind> {
        if (savedJson != null) {
            return runCatching { ExternalBind.fromJsonArray(JSONArray(savedJson)) }
                .getOrDefault(emptyList())
        }
        installId?.let { id -> return store.load(id)?.externalBinds ?: emptyList() }
        val json = intent?.getStringExtra(EXTRA_BINDS) ?: return emptyList()
        return runCatching { ExternalBind.fromJsonArray(JSONArray(json)) }
            .getOrDefault(emptyList())
    }

    /** Persist/publish [binds] after a mutation, then re-render. */
    private fun commit() {
        val id = installId
        if (id != null) {
            // Re-check state at write time, not just when DistroInfo
            // rendered the button: a service job (broker-initiated
            // install/uninstall) may have started while this screen was
            // open, and its metadata writes must not interleave with
            // ours. [InstallationStore.update] re-reads and applies the
            // edit under a per-id lock, so the gate and the write are
            // atomic against other in-process writers.
            val saved = store.update(id) { current ->
                if (current.state == Installation.State.READY ||
                    current.state == Installation.State.FAILED
                ) {
                    current.copy(externalBinds = binds.toList())
                } else {
                    null
                }
            }
            if (saved == null) {
                // Uninstalled or mid-mutation underneath us — bail
                // rather than fight the service over the file.
                finish()
                return
            }
        } else {
            publishResult()
        }
        renderList()
        updateGrantBanner()
    }

    /** Result mode: hand the caller the current list. */
    private fun publishResult() {
        setResult(
            Activity.RESULT_OK,
            Intent().putExtra(EXTRA_BINDS, ExternalBind.toJsonArray(binds).toString()),
        )
    }

    private fun buildGrantBanner(pad: Int): LinearLayout {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        row.addView(
            TextView(this).apply {
                text = getString(R.string.manage_binds_grant_banner)
                textSize = 14f
                setTextColor(getColor(R.color.tawc_danger))
            },
            LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f),
        )
        row.addView(
            tonalButton(getString(R.string.manage_binds_grant_button)) {
                AllFilesAccess.openSettings(this)
            },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginStart = pad / 2 },
        )
        return row
    }

    private fun updateGrantBanner() {
        val show = !AllFilesAccess.granted()
        grantBanner.visibility = if (show) android.view.View.VISIBLE else android.view.View.GONE
    }

    private fun renderList() {
        val pad = (16 * resources.displayMetrics.density).toInt()
        listColumn.removeAllViews()
        if (binds.isEmpty()) {
            listColumn.addView(
                TextView(this).apply {
                    text = getString(R.string.manage_binds_empty)
                    textSize = 14f
                },
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
            )
        }
        for ((index, bind) in binds.withIndex()) {
            listColumn.addView(bindCard(bind, index, pad), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        }
        // Unbound common dirs (matched by guest path) trail the active
        // list under a "Suggested binds" header as one-tap suggestions.
        // Host dirs that verifiably don't exist are left out; without
        // the grant they can't be stat'd, so all suggestions show.
        val suggestions = AllFilesAccess.commonDirBinds().filter { common ->
            binds.none { it.guestPath == common.guestPath } &&
                !AllFilesAccess.hostDirVerifiablyMissing(common.hostPath)
        }
        if (suggestions.isNotEmpty()) {
            listColumn.addView(
                TextView(this).apply {
                    text = getString(R.string.manage_binds_suggested)
                    textSize = 14f
                    setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant))
                },
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2).also { it.topMargin = pad / 2 },
            )
        }
        for (common in suggestions) {
            listColumn.addView(suggestionCard(common, pad), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        }
    }

    /** Monospace path text with a small OS logo (Android host / Linux
     * guest) on its left. */
    private fun pathRow(
        iconRes: Int,
        path: String,
        sizeSp: Float,
        dim: Boolean,
        selectable: Boolean,
    ): LinearLayout {
        val density = resources.displayMetrics.density
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        row.addView(
            ImageView(this).apply {
                setImageResource(iconRes)
                imageTintList = ColorStateList.valueOf(
                    MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
                )
            },
            LinearLayout.LayoutParams((16 * density).toInt(), (16 * density).toInt()).apply {
                marginEnd = (8 * density).toInt()
            },
        )
        row.addView(
            TextView(this).apply {
                text = path
                textSize = sizeSp
                typeface = Typeface.MONOSPACE
                if (dim) {
                    setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant))
                }
                setTextIsSelectable(selectable)
            },
            LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f),
        )
        return row
    }

    private fun readOnlyLabel(): TextView = TextView(this).apply {
        text = getString(R.string.manage_binds_read_only)
        textSize = 12f
        val color = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
        setTextColor(color)
        // Lock occupies the same 16dp icon slot as the pathRow logos so
        // the icon column and text column both line up.
        val density = resources.displayMetrics.density
        val lock = androidx.appcompat.content.res.AppCompatResources
            .getDrawable(context, R.drawable.ic_lock)?.mutate()?.apply {
                val size = (16 * density).toInt()
                setBounds(0, 0, size, size)
                setTint(color)
            }
        setCompoundDrawablesRelative(lock, null, null, null)
        compoundDrawablePadding = (8 * density).toInt()
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun bindCard(bind: ExternalBind, index: Int, pad: Int): android.view.View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(pad, pad / 2, pad, pad / 2)
        }
        // Guest (Linux) path over the Android path it's linked to,
        // each tagged with its OS logo.
        val paths = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        paths.addView(
            pathRow(R.drawable.ic_linux_logo, bind.guestPath, 16f, dim = false, selectable = true),
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 4),
        )
        paths.addView(
            pathRow(R.drawable.ic_android_logo, bind.hostPath, 14f, dim = true, selectable = true),
        )
        if (bind.readOnly) {
            paths.addView(readOnlyLabel())
        }
        row.addView(paths, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val btnSize = tawcButtonSizePx()
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.addView(
            tonalIconButton(R.drawable.ic_settings_gear, getString(R.string.action_edit)) {
                showEditDialog(index)
            },
            LinearLayout.LayoutParams(btnSize, btnSize).apply { marginEnd = pad / 4 },
        )
        buttons.addView(
            tonalIconButton(R.drawable.ic_delete, getString(R.string.action_remove)) {
                binds.removeAt(index)
                commit()
            },
            LinearLayout.LayoutParams(btnSize, btnSize),
        )
        row.addView(
            buttons,
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply { marginStart = pad / 2 },
        )
        return tawcCard().apply { addView(row) }
    }

    /** One-tap suggestion for an unbound common dir: guest path plus
     * an accent Add button. Adding carries the suggestion's default
     * read-only-ness (flagged on the card). */
    private fun suggestionCard(bind: ExternalBind, pad: Int): android.view.View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(pad, pad / 2, pad, pad / 2)
        }
        // No OS logo here: suggestions show only the guest path, and the
        // bare text keeps them visually distinct from active binds.
        val labels = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        labels.addView(TextView(this).apply {
            text = bind.guestPath
            textSize = 16f
            typeface = Typeface.MONOSPACE
            setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant))
        })
        if (bind.readOnly) {
            labels.addView(readOnlyLabel())
        }
        row.addView(labels, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val btnSize = tawcButtonSizePx()
        row.addView(
            tonalIconButton(
                R.drawable.ic_add,
                getString(R.string.action_add),
                backgroundColor = R.color.tawc_accent,
            ) {
                val problem = validate(bind, null)
                if (problem != null) {
                    Toast.makeText(this, problem, Toast.LENGTH_LONG).show()
                    return@tonalIconButton
                }
                binds.add(bind)
                commit()
            },
            LinearLayout.LayoutParams(btnSize, btnSize).apply { marginStart = pad / 2 },
        )
        return tawcCard().apply { addView(row) }
    }

    /** Add (`editIndex == null`) or edit (`editIndex` set) one bind. */
    private fun showEditDialog(editIndex: Int?) {
        val pad = (16 * resources.displayMetrics.density).toInt()
        val existing = editIndex?.let { binds[it] }

        fun pathField(initial: String?): EditText = EditText(this).apply {
            setText(initial ?: "")
            isSingleLine = true
            typeface = Typeface.MONOSPACE
            textSize = 14f
            // Same no-autocorrect trick as the run dialog: Gboard only
            // honours it via VISIBLE_PASSWORD.
            inputType = InputType.TYPE_CLASS_TEXT or
                InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }

        val guestField = pathField(existing?.guestPath)
        val hostField = pathField(existing?.hostPath)

        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad / 2, pad, 0)
        }
        column.addView(TextView(this).apply {
            text = getString(R.string.manage_binds_guest_label)
            textSize = 14f
        })
        column.addView(guestField, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        column.addView(TextView(this).apply {
            text = getString(R.string.manage_binds_host_label)
            textSize = 14f
        })
        column.addView(hostField, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        column.addView(
            tonalButton(getString(R.string.manage_binds_browse)) {
                pickerTargetField = hostField
                pickHostDir.launch(
                    DirectoryPickerActivity.intentFor(this, hostField.text.toString().trim().ifEmpty { null })
                )
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT),
        )
        val readOnlyBox = CheckBox(this).apply {
            text = getString(R.string.manage_binds_read_only)
            isChecked = existing?.readOnly ?: false
        }
        column.addView(readOnlyBox, verticalLp(WRAP_CONTENT, WRAP_CONTENT))

        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(getString(
                if (existing == null) R.string.manage_binds_add_title else R.string.manage_binds_edit_title
            ))
            .setView(column)
            .setNegativeButton(getString(R.string.action_cancel), null)
            .setPositiveButton(getString(R.string.action_save), null)
            .show()
        // Validation needs to keep the dialog open on failure, so the
        // positive button is wired manually instead of via the builder
        // (whose listener always dismisses).
        dialog.getButton(android.content.DialogInterface.BUTTON_POSITIVE)?.setOnClickListener {
            val candidate = ExternalBind(
                hostPath = hostField.text.toString().trim().let {
                    if (it.length > 1) it.trimEnd('/') else it
                },
                guestPath = expandGuestHome(guestField.text.toString().trim()).let {
                    if (it.length > 1) it.trimEnd('/') else it
                },
                readOnly = readOnlyBox.isChecked,
            )
            val problem = validate(candidate, editIndex)
            if (problem != null) {
                Toast.makeText(this, problem, Toast.LENGTH_LONG).show()
                return@setOnClickListener
            }
            if (editIndex == null) binds.add(candidate) else binds[editIndex] = candidate
            commit()
            dialog.dismiss()
        }
    }

    /** Expand a leading `~` in a typed guest path to the guest home.
     * Saved binds stay absolute so downstream consumers (persistence,
     * tawcroot `-b` specs) never see a tilde. Other forms (`~user`)
     * pass through and fail the must-be-absolute check. Host paths get
     * no expansion — Android has no meaningful `~`. */
    private fun expandGuestHome(path: String): String = when {
        path == "~" -> RootfsEnv.GUEST_HOME
        path.startsWith("~/") -> RootfsEnv.GUEST_HOME + path.substring(1)
        else -> path
    }

    private fun validate(candidate: ExternalBind, editIndex: Int?): String? {
        candidate.validationError()?.let { return it }
        if (editIndex == null && binds.size >= ExternalBind.MAX_BINDS) {
            return getString(R.string.manage_binds_too_many, ExternalBind.MAX_BINDS)
        }
        if (binds.withIndex().any { (i, b) -> i != editIndex && b.guestPath == candidate.guestPath }) {
            return getString(R.string.manage_binds_duplicate_guest, candidate.guestPath)
        }
        // Sources are never auto-created, so a typo here would
        // otherwise surface only at launch time.
        if (AllFilesAccess.hostDirVerifiablyMissing(candidate.hostPath)) {
            return getString(R.string.manage_binds_host_missing, candidate.hostPath)
        }
        return null
    }

    companion object {
        /** Edit the persisted bind list of an existing install. */
        const val EXTRA_INSTALL_ID = "installId"

        /** Edit a JSON bind list in memory; result carries the same key. */
        const val EXTRA_BINDS = "binds"

        private const val KEY_BINDS = "tawc.managebinds.binds"

        fun intentForInstall(context: Context, installId: String): Intent =
            Intent(context, ManageBindsActivity::class.java)
                .putExtra(EXTRA_INSTALL_ID, installId)

        fun intentForResult(context: Context, bindsJson: String): Intent =
            Intent(context, ManageBindsActivity::class.java)
                .putExtra(EXTRA_BINDS, bindsJson)
    }
}
