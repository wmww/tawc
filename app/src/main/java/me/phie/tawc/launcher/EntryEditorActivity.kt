package me.phie.tawc.launcher

import android.content.DialogInterface
import android.content.Intent
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.content.res.AppCompatResources
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.PointerEmulation
import me.phie.tawc.R
import me.phie.tawc.Settings
import me.phie.tawc.compositor.NativeBridge
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.ui.Scaffold
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.graphicsBackendGroup
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.pointerEmulationGroup
import me.phie.tawc.ui.primaryButton
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.verticalLp
import java.io.File
import java.io.IOException

/**
 * Edit a launcher entry's TAWC-side fields, or make a shortcut. Writes
 * only ever go to the [LauncherStore]; no `.desktop` file is touched.
 *
 * - A scanned entry shows each field's effective value. A field that
 *   differs from the scanned (packaged) one becomes an override, with a
 *   reset control beside it; a blank field falls back to the packaged
 *   value, shown as its hint. Toolbar Reset drops every override but
 *   hide state.
 * - A shortcut (the store's own entry) uses the same form, with
 *   Delete; Add entry makes a new one.
 *
 * Form: Command (required) + Name (blank = the command), environment
 * variables (exported at launch), Icon (a theme name or in-rootfs path
 * with a live preview, picked by [IconPickerActivity], or an image
 * imported by [IconImport] into the store), Terminal, and graphics
 * backend / pointer emulation overrides. Launched for result by
 * [AppsPane]; RESULT_OK = rescan.
 */
class EntryEditorActivity : AppCompatActivity() {

    private val launcher by lazy { LauncherStore(this) }

    private lateinit var nameField: EditText
    private lateinit var execField: EditText
    private lateinit var iconField: EditText
    private lateinit var terminalCheckbox: CheckBox
    private lateinit var saveButton: MaterialButton
    private lateinit var iconPreview: ImageView
    private lateinit var rootfs: File
    private lateinit var scaffold: Scaffold
    private var installId = ""

    /** The entry being edited; null for a new shortcut. */
    private var entry: LauncherEntry? = null
    private val scanned get() = entry?.source == LauncherEntry.Source.DESKTOP

    /** Per-field reset (scanned) / clear (shortcut icon) controls. */
    private val resets = HashMap<String, View>()

    /** Store icon file in effect (unchanged from load), or null. */
    private var iconFile: String? = null
    /** A Load not saved yet, and its preview copy. */
    private var pendingIcon: IconImport.Result? = null
    private var pendingPreview: File? = null
    /** Set while the code (not the user) changes [iconField]. */
    private var settingIcon = false

    private val iconLoader by lazy {
        IconLoader(lifecycleScope, (PREVIEW_DP * resources.displayMetrics.density).toInt())
    }
    private var previewJob: Job? = null

    private val pickIcon = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        result.data?.getStringExtra(IconPickerActivity.EXTRA_NAME)
            ?.takeIf { result.resultCode == RESULT_OK }
            ?.let { setIcon(it) }
    }

    private val loadIcon = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { importIcon(it) }
    }

    private lateinit var envRows: LinearLayout
    private val envFields = ArrayList<Pair<EditText, EditText>>()

    /** The form's graphics / pointer emulation pick (null = global). */
    private var graphicsPick: GraphicsBackend? = null
    private var pointerPick: PointerEmulation? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installId = intent?.getStringExtra(EXTRA_ID) ?: ""
        val inst = InstallationStore(this).load(installId) ?: return finish()
        rootfs = launcher.rootfs(installId)
        val entryId = intent?.getStringExtra(EXTRA_ENTRY)
        // The screen goes up at once (a later setContentView misses the
        // system-bar insets); the form fills in once the entry loads.
        scaffold = buildChildScreen(getString(if (entryId == null) R.string.editor_title_new else R.string.editor_title_edit))
        // ✕, not ←: leaving discards the form, so say so (Material's
        // full-screen dialog pattern). Unsaved changes confirm first.
        scaffold.toolbar.apply {
            navigationIcon = AppCompatResources.getDrawable(this@EntryEditorActivity, R.drawable.ic_close)
                ?.mutate()
                ?.apply { setTintList(controlTint()) }
            navigationContentDescription = getString(R.string.editor_close)
            setNavigationOnClickListener { requestClose() }
        }
        onBackPressedDispatcher.addCallback(this, discardGuard)
        setContentView(scaffold.root)
        if (entryId == null) {
            buildForm()
            return
        }
        lifecycleScope.launch {
            val found = withContext(Dispatchers.IO) { LauncherEntry.find(applicationContext, inst, entryId) }
            if (found == null) {
                Toast.makeText(this@EntryEditorActivity, R.string.shortcut_entry_gone, Toast.LENGTH_LONG).show()
                finish()
                return@launch
            }
            entry = found
            buildForm()
        }
    }

    override fun onDestroy() {
        pendingPreview?.delete()
        super.onDestroy()
    }

    private fun buildForm() {
        val e = entry
        graphicsPick = GraphicsBackend.fromKeyOrNull(e?.graphics)
        pointerPick = PointerEmulation.fromKeyOrNull(e?.pointer)
        iconFile = e?.iconFile?.ifEmpty { null }
        val pad = (16 * resources.displayMetrics.density).toInt()

        val form = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        execField = addField(form, "exec", R.string.editor_field_exec, e?.exec.orEmpty(), pad)
        nameField = addField(form, "name", R.string.editor_field_name, e?.name.orEmpty(), pad)
        addEnvSection(form, e?.env.orEmpty().toList(), pad)
        iconField = addIconRow(form, if (iconFile != null) "" else e?.icon.orEmpty(), pad)
        // New shortcuts default to Terminal: hand-made entries are
        // usually CLI programs.
        val terminalRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        terminalCheckbox = CheckBox(this).apply {
            text = getString(R.string.editor_field_terminal)
            isChecked = e?.terminal ?: true
            // The fallback glyph follows Terminal.
            setOnCheckedChangeListener { _, _ -> refreshPreview(debounce = false); revalidate() }
        }
        terminalRow.addView(terminalCheckbox, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(terminalRow, "terminal") {
            terminalCheckbox.isChecked = e?.packagedValue("terminal") == "true"
        }
        form.addView(terminalRow, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        // Unchecked = the global setting; checking starts from it.
        var picked = graphicsPick ?: Settings.graphicsBackend
        val graphicsGroup = graphicsBackendGroup(picked) { b ->
            picked = b; graphicsPick = b; revalidate()
        }
        form.addView(
            overrideBlock(R.string.editor_override_graphics, graphicsPick != null, graphicsGroup) { checked ->
                graphicsPick = if (checked) picked else null
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2),
        )
        var pointerPicked = pointerPick ?: Settings.pointerEmulation
        val pointerGroup = pointerEmulationGroup(pointerPicked) { m ->
            pointerPicked = m; pointerPick = m; revalidate()
        }
        form.addView(
            overrideBlock(R.string.editor_override_pointer_emulation, pointerPick != null, pointerGroup) { checked ->
                pointerPick = if (checked) pointerPicked else null
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad),
        )

        saveButton = primaryButton(getString(R.string.editor_save)) { save() }
        form.addView(saveButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        // Delete/Reset live in the toolbar, not as a big destructive
        // button next to Save; both confirm. Tinted manually —
        // MaterialToolbar doesn't tint menu-item icons, and the vectors'
        // own fill is white.
        val reset = scanned && e!!.overridden.isNotEmpty()
        if (e?.source == LauncherEntry.Source.SHORTCUT || reset) {
            scaffold.toolbar.menu.add(getString(if (reset) R.string.editor_reset else R.string.editor_delete)).apply {
                icon = AppCompatResources
                    .getDrawable(this@EntryEditorActivity, if (reset) R.drawable.ic_reset else R.drawable.ic_delete)
                    ?.mutate()
                    ?.apply { setTint(getColor(R.color.tawc_danger)) }
                setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS)
                setOnMenuItemClickListener { confirmRemove(reset); true }
            }
        }

        scaffold.content.addView(
            ScrollView(this).apply { addView(form) },
            LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT),
        )
        initialState = formState()
        revalidate()
        refreshPreview(debounce = false)
    }

    /** Checkbox + [group], the group shown only while checked. One
     *  block, so its bottom margin holds with the group hidden. */
    private fun overrideBlock(labelRes: Int, checked: Boolean, group: View, onCheck: (Boolean) -> Unit): View {
        group.visibility = if (checked) View.VISIBLE else View.GONE
        val block = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        block.addView(
            CheckBox(this).apply {
                text = getString(labelRes)
                isChecked = checked
                setOnCheckedChangeListener { _, on ->
                    onCheck(on)
                    revalidate()
                    group.visibility = if (on) View.VISIBLE else View.GONE
                }
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT),
        )
        block.addView(group, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        return block
    }

    /** A reset control for [field] at the end of [row], shown by
     *  [revalidate] while the field differs from the packaged value. */
    private fun addReset(row: LinearLayout, field: String, onClick: () -> Unit) {
        val shortcutIcon = field == "icon" && !scanned
        if (!scanned && !shortcutIcon) return
        val btn = plainIconButton(
            if (shortcutIcon) R.drawable.ic_close else R.drawable.ic_reset,
            getString(if (shortcutIcon) R.string.editor_icon_clear else R.string.editor_field_reset),
        ) { onClick(); revalidate() }
        btn.visibility = View.GONE
        resets[field] = btn
        row.addView(btn, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
    }

    private fun label(form: LinearLayout, labelRes: Int) {
        form.addView(
            TextView(this).apply { text = getString(labelRes); textSize = 14f },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
    }

    /**
     * ```
     * Icon
     * [preview]  [ field ............ ✕ ] ↺
     *            [ Select ]  [ Load ]
     * ```
     */
    private fun addIconRow(form: LinearLayout, value: String, pad: Int): EditText {
        label(form, R.string.editor_field_icon)
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.TOP
        }
        val previewPx = (PREVIEW_DP * resources.displayMetrics.density).toInt()
        iconPreview = ImageView(this).apply { scaleType = ImageView.ScaleType.FIT_CENTER }
        row.addView(iconPreview, LinearLayout.LayoutParams(previewPx, previewPx).also {
            it.marginEnd = pad
            it.topMargin = pad / 4
        })

        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val field = TextInputEditText(this).apply {
            setText(value)
            isSingleLine = true
            // Icon names aren't words; no spellcheck squiggles.
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            // As tall as the ✕, so the row doesn't jump when it shows.
            minHeight = (48 * resources.displayMetrics.density).toInt()
            doAfterTextChanged {
                // Typing a name replaces an imported image.
                if (!settingIcon && !it.isNullOrEmpty()) clearIconImport()
                revalidate()
                refreshPreview(debounce = true)
            }
        }
        // Box-less, so it reads like the plain fields above; the layout
        // is only here for the clear (✕) end icon.
        val layout = TextInputLayout(this).apply {
            boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_NONE
            isHintEnabled = false
            endIconMode = TextInputLayout.END_ICON_CLEAR_TEXT
            addView(field)
        }
        val fieldRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        fieldRow.addView(layout, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(fieldRow, "icon") {
            clearIconImport()
            setIcon(if (scanned) entry!!.packagedValue("icon") else "")
        }
        column.addView(fieldRow, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        val buttons = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.addView(tonalButton(getString(R.string.editor_icon_select)) {
            pickIcon.launch(Intent(this, IconPickerActivity::class.java).putExtra(IconPickerActivity.EXTRA_ID, installId))
        }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).also { it.marginEnd = pad / 2 })
        buttons.addView(tonalButton(getString(R.string.editor_icon_load)) {
            loadIcon.launch(arrayOf("image/*"))
        }, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        column.addView(buttons, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        row.addView(column, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        form.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        return field
    }

    private fun hasIconImage() = iconFile != null || pendingIcon != null

    private fun clearIconImport() {
        iconFile = null
        pendingIcon = null
    }

    /** Show what the launcher grid would draw: the imported image, else
     *  the resolved Icon value (blank on a scanned entry = packaged),
     *  else the same fallback glyph. */
    private fun refreshPreview(debounce: Boolean) {
        if (!::iconPreview.isInitialized || !::terminalCheckbox.isInitialized) return
        val fallback = if (terminalCheckbox.isChecked) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback
        val image = when {
            pendingIcon != null -> pendingPreview?.path
            iconFile != null -> File(launcher.iconsDir(installId), iconFile!!).path
            else -> null
        }
        val value = iconField.text.toString().ifBlank { if (scanned) entry!!.packagedValue("icon") else "" }
        val rootfsPath = rootfs.absolutePath
        previewJob?.cancel()
        previewJob = lifecycleScope.launch {
            if (debounce) delay(PREVIEW_DEBOUNCE_MS)
            val path = image ?: if (value.isBlank()) "" else withContext(Dispatchers.IO) {
                runCatching { NativeBridge.nativeResolveIcon(rootfsPath, value) }.getOrDefault("")
            }
            iconLoader.load(path, iconPreview, fallback)
        }
    }

    private fun setIcon(value: String) {
        settingIcon = true
        iconField.setText(value)
        iconField.setSelection(value.length)
        settingIcon = false
        if (value.isNotEmpty()) clearIconImport()
        refreshPreview(debounce = false)
    }

    /** Read a picked image as PNG ([IconImport]); it lands in the store
     *  on Save. Failure leaves the icon as it was. */
    private fun importIcon(uri: Uri) {
        lifecycleScope.launch {
            val scratch = File(cacheDir, "icon-import")
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val r = IconImport.import(this@EntryEditorActivity, uri, scratch)
                    val preview = File.createTempFile("preview", ".png", scratch).apply { writeBytes(r.bytes) }
                    r to preview
                }
            }
            result.onSuccess { (r, preview) ->
                pendingPreview?.delete()
                pendingIcon = r
                pendingPreview = preview
                iconFile = null
                settingIcon = true
                iconField.setText("")
                settingIcon = false
                revalidate()
                refreshPreview(debounce = false)
            }.onFailure {
                Toast.makeText(
                    this@EntryEditorActivity,
                    getString(R.string.editor_icon_import_failed, it.message),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    /** Label + single-line EditText with a reset control beside it. A
     *  scanned entry's packaged value is the hint. */
    private fun addField(form: LinearLayout, field: String, labelRes: Int, value: String, pad: Int): EditText {
        label(form, labelRes)
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val edit = EditText(this).apply {
            setText(value)
            isSingleLine = true
            if (scanned) hint = entry!!.packagedValue(field)
            doAfterTextChanged { revalidate() }
        }
        row.addView(edit, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        addReset(row, field) { edit.setText(entry?.packagedValue(field).orEmpty()) }
        form.addView(row, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        return edit
    }

    /**
     * ```
     * Environment variables
     * [ NAME      ] [ value          ] ✕
     * [ + Add ]
     * ```
     */
    private fun addEnvSection(form: LinearLayout, env: List<Pair<String, String>>, pad: Int) {
        form.addView(
            TextView(this).apply { text = getString(R.string.editor_field_env); textSize = 14f },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
        envRows = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        form.addView(envRows, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        for ((k, v) in env) addEnvRow(k, v)
        form.addView(
            tonalButton(getString(R.string.editor_env_add)) { addEnvRow("", "").first.requestFocus() }.apply {
                icon = AppCompatResources.getDrawable(this@EntryEditorActivity, R.drawable.ic_add)
                iconTint = ColorStateList.valueOf(getColor(R.color.tawc_on_tonal))
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT, bottomMargin = pad),
        )
    }

    /** One `[name] [value] ✕` row; returns its two fields. */
    private fun addEnvRow(name: String, value: String): Pair<EditText, EditText> {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        fun field(text: String, hintRes: Int) = EditText(this).apply {
            setText(text)
            hint = getString(hintRes)
            isSingleLine = true
            // Variable names and values aren't words.
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            doAfterTextChanged { revalidate() }
        }
        val key = field(name, R.string.editor_env_name_hint)
        val value = field(value, R.string.editor_env_value_hint)
        row.addView(key, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        row.addView(value, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        row.addView(plainIconButton(R.drawable.ic_close, getString(R.string.editor_env_remove)) {
            envRows.removeView(row)
            envFields.removeAll { it.first === key }
            revalidate()
        })
        envRows.addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
        return (key to value).also { envFields += it }
    }

    /** Rows with a name; fully blank rows are ignored. */
    private fun envVars(): List<Pair<String, String>> =
        envFields.map { (k, v) -> k.text.toString().trim() to v.text.toString() }
            .filter { (k, v) -> k.isNotEmpty() || v.isNotEmpty() }

    /** Flag invalid names on their fields; true when all are valid. */
    private fun validateEnv(): Boolean {
        var ok = true
        for ((k, v) in envFields) {
            val name = k.text.toString().trim()
            val bad = (name.isNotEmpty() || v.text.isNotEmpty()) && !DesktopEntryFile.isValidEnvName(name)
            k.error = if (bad) getString(R.string.editor_env_invalid) else null
            if (bad) ok = false
        }
        return ok
    }

    /** What [save] would persist, as typed; compared against [initialState]. */
    private data class FormState(
        val name: String,
        val exec: String,
        val icon: String,
        val iconFile: String?,
        val pendingIcon: IconImport.Result?,
        val terminal: Boolean,
        val env: List<Pair<String, String>>,
        val graphics: GraphicsBackend?,
        val pointer: PointerEmulation?,
    )

    private fun formState() = FormState(
        nameField.text.toString(), execField.text.toString(), iconField.text.toString(), iconFile, pendingIcon,
        terminalCheckbox.isChecked, envVars(), graphicsPick, pointerPick,
    )

    private var initialState: FormState? = null

    private fun isDirty() = initialState.let { it != null && it != formState() }

    /** Back/gesture interception, enabled only while there are unsaved
     *  changes so a clean form keeps the system's predictive back. */
    private val discardGuard = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = requestClose()
    }

    /** Close, confirming first when that would discard changes. */
    private fun requestClose() {
        if (!isDirty()) {
            finish()
            return
        }
        val dialog = AlertDialog.Builder(this)
            .setMessage(R.string.editor_discard_confirm)
            .setPositiveButton(R.string.editor_discard) { _, _ -> finish() }
            .setNegativeButton(R.string.editor_keep_editing, null)
            .show()
        // Neutral Keep editing, as on the app's other confirms.
        dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
            btn.setTextColor(MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant))
        }
    }

    private fun controlTint(): ColorStateList? = TypedValue().let {
        theme.resolveAttribute(androidx.appcompat.R.attr.colorControlNormal, it, true)
        if (it.resourceId != 0) getColorStateList(it.resourceId) else ColorStateList.valueOf(it.data)
    }

    /** A scanned entry's text override for [field]: the typed value
     *  unless blank or the packaged one. */
    private fun textOverride(field: String, value: String): String? =
        value.trim().takeIf { it.isNotEmpty() && it != entry!!.packagedValue(field) }

    private fun revalidate() {
        if (!::saveButton.isInitialized) return
        val exec = execField.text.toString().trim()
        nameField.hint = if (scanned) entry!!.packagedValue("name") else exec
        if (scanned) {
            fun show(field: String, on: Boolean) {
                resets[field]?.visibility = if (on) View.VISIBLE else View.GONE
            }
            show("name", textOverride("name", nameField.text.toString()) != null)
            show("exec", textOverride("exec", execField.text.toString()) != null)
            show("icon", hasIconImage() || textOverride("icon", iconField.text.toString()) != null)
            show("terminal", terminalCheckbox.isChecked.toString() != entry!!.packagedValue("terminal"))
        } else {
            resets["icon"]?.visibility = if (hasIconImage()) View.VISIBLE else View.GONE
        }
        iconField.hint = when {
            hasIconImage() -> getString(R.string.editor_icon_imported)
            scanned -> entry!!.packagedValue("icon")
            else -> null
        }
        val envOk = validateEnv()
        saveButton.isEnabled = (exec.isNotEmpty() || scanned) && envOk
        discardGuard.isEnabled = isDirty()
    }

    /** The form as store fields over [old] (kept: comment, hidden and
     *  fields this build doesn't know). [newIcon] is the file a pending
     *  Load was stored as. */
    private fun fields(old: EntryFields, newIcon: String?): EntryFields {
        val image = newIcon ?: iconFile
        val iconText = iconField.text.toString().trim()
        val env = envVars().toMap().takeIf { it.isNotEmpty() }
        return if (scanned) {
            val e = entry!!
            old.copy(
                name = textOverride("name", nameField.text.toString()),
                exec = textOverride("exec", execField.text.toString()),
                terminal = terminalCheckbox.isChecked.takeIf { it.toString() != e.packagedValue("terminal") },
                icon = if (image != null) null else textOverride("icon", iconText),
                iconFile = image,
                env = env,
                graphics = graphicsPick?.key,
                pointer = pointerPick?.key,
            )
        } else {
            old.copy(
                name = nameField.text.toString().trim().ifEmpty { null },
                exec = execField.text.toString().trim(),
                terminal = terminalCheckbox.isChecked,
                icon = if (image != null) null else iconText.ifEmpty { null },
                iconFile = image,
                env = env,
                graphics = graphicsPick?.key,
                pointer = pointerPick?.key,
            )
        }
    }

    /** Write the form to the store: an override of a scanned entry, the
     *  shortcut being edited, or a new `tawc:app:` shortcut. */
    private fun save() {
        val e = entry
        val pending = pendingIcon
        val name = nameField.text.toString().trim().ifEmpty { execField.text.toString().trim() }
        val written = try {
            launcher.update(installId, pending?.let { it.bytes to it.slug }) { store, newIcon ->
                when {
                    e == null -> store.withShortcut(store.newShortcutId(name), fields(EntryFields(), newIcon))
                    scanned -> store.withOverride(e.id, fields(store.overrides[e.id] ?: EntryFields(), newIcon))
                    else -> store.withShortcut(e.id, fields(store.shortcuts[e.id] ?: EntryFields(), newIcon))
                }
            }
        } catch (ex: IOException) {
            Toast.makeText(this, getString(R.string.editor_save_failed, ex.message), Toast.LENGTH_LONG).show()
            return
        }
        if (written == null) {
            Toast.makeText(this, getString(R.string.editor_save_failed, installId), Toast.LENGTH_LONG).show()
            return
        }
        setResult(RESULT_OK)
        finish()
    }

    /** Reset (drop a scanned entry's overrides, keeping hide state) or
     *  Delete (a shortcut). */
    private fun confirmRemove(reset: Boolean) {
        val e = entry ?: return
        val label = nameField.text.toString().ifBlank { e.name.ifBlank { e.id } }
        AlertDialog.Builder(this)
            .setMessage(getString(if (reset) R.string.editor_reset_confirm else R.string.editor_delete_confirm, label))
            .setPositiveButton(if (reset) R.string.editor_reset else R.string.editor_delete) { _, _ ->
                val written = runCatching {
                    launcher.update(installId) { store, _ ->
                        if (reset) store.withoutOverrides(e.id) else store.withoutShortcut(e.id)
                    }
                }.getOrNull()
                if (written == null) {
                    Toast.makeText(this, R.string.editor_delete_failed, Toast.LENGTH_LONG).show()
                    return@setPositiveButton
                }
                setResult(RESULT_OK)
                finish()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    companion object {
        /** Installation id. */
        const val EXTRA_ID = "id"

        /** [LauncherEntry.id] to edit; absent = new shortcut. */
        const val EXTRA_ENTRY = "entry"

        /** Icon preview edge, close to the launcher grid's icon. */
        private const val PREVIEW_DP = 48f

        private const val PREVIEW_DEBOUNCE_MS = 250L
    }
}
