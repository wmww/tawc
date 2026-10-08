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
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
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
import me.phie.tawc.install.util.atomicWriteText
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
 * Create or edit a launcher entry. Writes only ever land in the rootfs's
 * managed dir ([DesktopEntryFile.MANAGED_SUBDIR]): editing a packaged
 * entry saves an override copy there that shadows it, and the packaged
 * file is never touched. Form: Exec (required) + Name (defaults to
 * Exec), Icon (freeform `Icon=` value with a live preview, picked from
 * the distro's icons by [IconPickerActivity] or imported by
 * [IconImport]), Terminal, and per-entry graphics backend and pointer
 * emulation overrides ([Installation.entryGraphics],
 * [Installation.entryPointerEmulation], not part of the file). Existing files
 * are [DesktopEntryFile.patch]ed, so keys the form doesn't show survive.
 *
 * The toolbar action follows what is being edited: Delete for a
 * personal entry, Reset for an override or a packaged entry with a
 * graphics or pointer override, nothing otherwise.
 *
 * Launched by [AppsPane] (via MainActivity) for result (RESULT_OK = the rootfs
 * changed, rescan). Writes are plain app-uid file I/O, so entry points
 * are hidden for chroot installs (root-owned rootfs — see
 * notes/launcher.md "Access model").
 */
class DesktopFileEditorActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }

    private lateinit var nameField: EditText
    private lateinit var execField: EditText
    private lateinit var iconField: EditText
    private lateinit var terminalCheckbox: CheckBox
    private lateinit var saveButton: MaterialButton
    private lateinit var iconPreview: ImageView
    private lateinit var rootfs: File
    private var installId = ""

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

    /** `Comment=` from the loaded file. Not exposed in the form, but
     *  carried in the draft so [DesktopEntryFile.patch] leaves it be. */
    private var existingComment = ""

    /** `Exec=` as loaded, and split into variables + command. */
    private var loadedExec = ""
    private var loadedLine = DesktopEntryFile.ExecLine(emptyList(), "")
    private lateinit var envRows: LinearLayout
    private val envFields = ArrayList<Pair<EditText, EditText>>()

    /** The file loaded into the form, or null when creating a new entry. */
    private var source: File? = null
    private var sourceText = ""
    /** Where a save goes ([DesktopEntryFile.targetFor]). */
    private var target: File? = null
    /** The packaged file a managed [source] overrides, if any. */
    private var shadows: File? = null
    private var entryId: String? = null
    /** [Installation.entryGraphics] for [entryId] as loaded, and the
     *  form's pick (null = no override, the global setting). */
    private var storedGraphics: String? = null
    private var graphicsPick: GraphicsBackend? = null
    /** Same for [Installation.entryPointerEmulation]. */
    private var storedPointer: String? = null
    private var pointerPick: PointerEmulation? = null
    private lateinit var managedDir: File

    private enum class ToolbarAction { NONE, DELETE, RESET }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installId = intent?.getStringExtra(EXTRA_ID) ?: ""
        val inst = store.load(installId) ?: return finish()
        rootfs = store.rootfsDir(installId)
        managedDir = DesktopEntryFile.managedDir(rootfs)

        val path = intent?.getStringExtra(EXTRA_PATH)
        // New entries default to Terminal=true: hand-made entries are
        // usually CLI scripts. Editing keeps the file's value.
        var loaded = DesktopEntryFile.Draft(terminal = true)
        if (path != null) {
            // Any scanned entry file, but nothing outside this rootfs
            // (belt-and-braces against a stale/forged intent).
            val f = DesktopEntryFile.fileInRootfs(path, rootfs) ?: return finish()
            // A failed read must not fall through to the empty default
            // draft: Save would then write an entry built from nothing.
            val bytes = try {
                f.readBytes()
            } catch (e: IOException) {
                Toast.makeText(this, getString(R.string.editor_load_failed, e.message), Toast.LENGTH_LONG).show()
                finish()
                return
            }
            val text = String(bytes)
            // String() decodes malformed UTF-8 to U+FFFD without
            // throwing; patching that would write mojibake back.
            if (!text.toByteArray().contentEquals(bytes)) {
                Toast.makeText(this, R.string.editor_not_utf8, Toast.LENGTH_LONG).show()
                finish()
                return
            }
            source = f
            sourceText = text
            target = DesktopEntryFile.targetFor(f, rootfs)
            shadows = intent?.getStringExtra(EXTRA_SHADOWS)
                ?.takeIf { it.isNotEmpty() && DesktopEntryFile.isManaged(f.path, rootfs) }
                ?.let { DesktopEntryFile.fileInRootfs(it, rootfs) }
            entryId = DesktopEntryFile.idFor(target!!.path)
            loaded = DesktopEntryFile.parse(text)
        }
        storedGraphics = entryId?.let { inst.entryGraphics[it] }
        graphicsPick = GraphicsBackend.fromKeyOrNull(storedGraphics)
        storedPointer = entryId?.let { inst.entryPointerEmulation[it] }
        pointerPick = PointerEmulation.fromKeyOrNull(storedPointer)

        val title = getString(
            if (source == null) R.string.editor_title_new else R.string.editor_title_edit,
        )
        val scaffold = buildChildScreen(title)
        // ✕, not ←: leaving discards the form, so say so (Material's
        // full-screen dialog pattern). Unsaved changes confirm first.
        scaffold.toolbar.apply {
            navigationIcon = AppCompatResources.getDrawable(this@DesktopFileEditorActivity, R.drawable.ic_close)
                ?.mutate()
                ?.apply { setTintList(controlTint()) }
            navigationContentDescription = getString(R.string.editor_close)
            setNavigationOnClickListener { requestClose() }
        }
        onBackPressedDispatcher.addCallback(this, discardGuard)
        val pad = (16 * resources.displayMetrics.density).toInt()

        val form = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        existingComment = loaded.comment
        loadedExec = loaded.exec
        loadedLine = DesktopEntryFile.splitExec(loaded.exec)
        execField = addField(form, R.string.editor_field_exec, loadedLine.command, pad)
        nameField = addField(form, R.string.editor_field_name, loaded.name, pad)
        addEnvSection(form, loadedLine.env, pad)
        iconField = addIconRow(form, loaded.icon, pad)
        terminalCheckbox = CheckBox(this).apply {
            text = getString(R.string.editor_field_terminal)
            isChecked = loaded.terminal
            // The fallback glyph follows Terminal.
            setOnCheckedChangeListener { _, _ -> refreshPreview(debounce = false); revalidate() }
        }
        form.addView(terminalCheckbox, verticalLp(WRAP_CONTENT, WRAP_CONTENT, bottomMargin = pad / 2))
        // Unchecked = the global setting; checking starts from it.
        var picked = graphicsPick ?: Settings.graphicsBackend
        val graphicsGroup = graphicsBackendGroup(picked) { b ->
            picked = b; graphicsPick = b; revalidate()
        }.apply { visibility = if (graphicsPick != null) View.VISIBLE else View.GONE }
        // One block, so its bottom margin holds with the group hidden.
        val graphicsBlock = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        graphicsBlock.addView(
            CheckBox(this).apply {
                text = getString(R.string.editor_override_graphics)
                isChecked = graphicsPick != null
                setOnCheckedChangeListener { _, checked ->
                    graphicsPick = if (checked) picked else null
                    revalidate()
                    graphicsGroup.visibility = if (checked) View.VISIBLE else View.GONE
                }
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT),
        )
        graphicsBlock.addView(graphicsGroup, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        form.addView(graphicsBlock, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        // Same shape for pointer emulation.
        var pointerPicked = pointerPick ?: Settings.pointerEmulation
        val pointerGroup = pointerEmulationGroup(pointerPicked) { m ->
            pointerPicked = m; pointerPick = m; revalidate()
        }.apply { visibility = if (pointerPick != null) View.VISIBLE else View.GONE }
        val pointerBlock = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        pointerBlock.addView(
            CheckBox(this).apply {
                text = getString(R.string.editor_override_pointer_emulation)
                isChecked = pointerPick != null
                setOnCheckedChangeListener { _, checked ->
                    pointerPick = if (checked) pointerPicked else null
                    revalidate()
                    pointerGroup.visibility = if (checked) View.VISIBLE else View.GONE
                }
            },
            verticalLp(WRAP_CONTENT, WRAP_CONTENT),
        )
        pointerBlock.addView(pointerGroup, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        form.addView(pointerBlock, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad))

        saveButton = primaryButton(getString(R.string.editor_save)) { save() }
        form.addView(saveButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))
        // Delete/Reset live in the toolbar, not as a big destructive
        // button next to Save; both confirm. Tinted manually —
        // MaterialToolbar doesn't tint menu-item icons, and the vectors'
        // own fill is white.
        val action = toolbarAction()
        if (action != ToolbarAction.NONE) {
            val reset = action == ToolbarAction.RESET
            scaffold.toolbar.menu.add(getString(if (reset) R.string.editor_reset else R.string.editor_delete)).apply {
                icon = AppCompatResources
                    .getDrawable(this@DesktopFileEditorActivity, if (reset) R.drawable.ic_reset else R.drawable.ic_delete)
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
        setContentView(scaffold.root)
        initialState = formState()
        revalidate()
        refreshPreview(debounce = false)
    }

    /**
     * ```
     * Icon
     * [preview]  [ field ............ ✕ ]
     *            [ Select ]  [ Load ]
     * ```
     */
    private fun addIconRow(form: LinearLayout, value: String, pad: Int): EditText {
        form.addView(
            TextView(this).apply { text = getString(R.string.editor_field_icon); textSize = 14f },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
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
            doAfterTextChanged { revalidate(); refreshPreview(debounce = true) }
        }
        // Box-less, so it reads like the plain fields above; the layout
        // is only here for the clear (✕) end icon.
        val layout = TextInputLayout(this).apply {
            boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_NONE
            isHintEnabled = false
            endIconMode = TextInputLayout.END_ICON_CLEAR_TEXT
            addView(field)
        }
        column.addView(layout, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
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

    /** Show what the launcher grid would draw for the current Icon
     *  value: the resolved PNG, else the same fallback glyph. */
    private fun refreshPreview(debounce: Boolean) {
        if (!::iconPreview.isInitialized || !::terminalCheckbox.isInitialized) return
        val value = iconField.text.toString()
        val fallback = if (terminalCheckbox.isChecked) R.drawable.ic_terminal_fallback else R.drawable.ic_app_fallback
        val rootfsPath = rootfs.absolutePath
        previewJob?.cancel()
        previewJob = lifecycleScope.launch {
            if (debounce) delay(PREVIEW_DEBOUNCE_MS)
            val path = if (value.isBlank()) "" else withContext(Dispatchers.IO) {
                runCatching { NativeBridge.nativeResolveIcon(rootfsPath, value) }.getOrDefault("")
            }
            iconLoader.load(path, iconPreview, fallback)
        }
    }

    private fun setIcon(value: String) {
        iconField.setText(value)
        iconField.setSelection(value.length)
    }

    /** Copy a picked image into the distro ([IconImport]) and put its
     *  name in the field. Failure leaves the field untouched. */
    private fun importIcon(uri: Uri) {
        lifecycleScope.launch {
            val name = withContext(Dispatchers.IO) {
                runCatching { IconImport.import(this@DesktopFileEditorActivity, uri, rootfs) }
            }
            name.onSuccess { setIcon(it) }.onFailure {
                Toast.makeText(
                    this@DesktopFileEditorActivity,
                    getString(R.string.editor_icon_import_failed, it.message),
                    Toast.LENGTH_LONG,
                ).show()
            }
        }
    }

    /** Label + single-line EditText, InstallActivity's form idiom. */
    private fun addField(form: LinearLayout, labelRes: Int, value: String, pad: Int): EditText {
        form.addView(
            TextView(this).apply { text = getString(labelRes); textSize = 14f },
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT),
        )
        val field = EditText(this).apply {
            setText(value)
            isSingleLine = true
            doAfterTextChanged { revalidate() }
        }
        form.addView(field, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = pad / 2))
        return field
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
                icon = AppCompatResources.getDrawable(this@DesktopFileEditorActivity, R.drawable.ic_add)
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

    /**
     * The form's Exec: the loaded value verbatim while the command and
     * variables are as loaded (so an untouched entry patches to
     * identical text), else [DesktopEntryFile.joinExec].
     */
    private fun composedExec(): String {
        val line = DesktopEntryFile.ExecLine(envVars(), execField.text.toString())
        return if (line == loadedLine) loadedExec else DesktopEntryFile.joinExec(line)
    }

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

    /** Name defaults to the command when left blank; the field shows it
     *  as the hint. */
    private fun draft() = DesktopEntryFile.Draft(
        name = nameField.text.toString().ifBlank { execField.text.toString().trim() },
        exec = composedExec(),
        comment = existingComment,
        icon = iconField.text.toString(),
        terminal = terminalCheckbox.isChecked,
    )

    /** What [save] would persist; compared against [initialState]. */
    private fun formState() = Triple(draft(), graphicsPick, pointerPick)

    private var initialState: Triple<DesktopEntryFile.Draft, GraphicsBackend?, PointerEmulation?>? = null

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

    private fun revalidate() {
        if (!::saveButton.isInitialized) return
        nameField.hint = execField.text.toString().trim()
        val envOk = validateEnv()
        saveButton.isEnabled = execField.text.isNotBlank() && envOk
        discardGuard.isEnabled = isDirty()
    }

    /**
     * Delete for a personal entry (managed, shadows nothing); Reset for
     * an override, or a packaged entry with only a graphics or pointer
     * override.
     */
    private fun toolbarAction(): ToolbarAction {
        val src = source ?: return ToolbarAction.NONE
        return when {
            src == target && shadows == null -> ToolbarAction.DELETE
            src == target || storedGraphics != null || storedPointer != null -> ToolbarAction.RESET
            else -> ToolbarAction.NONE
        }
    }

    /**
     * New entries get a fresh slug filename ([DesktopEntryFile.newFile])
     * and [DesktopEntryFile.serialize]; edits patch the loaded text into
     * [target], keeping its name — the filename is the entry id, which
     * pins, hidden state and the graphics override reference. Nothing is
     * written when the text didn't change, so a graphics-only edit of a
     * packaged entry doesn't fork it; an override patched back to exactly
     * its packaged text is deleted instead.
     */
    private fun save() {
        val draft = draft()
        val id: String
        try {
            val src = source
            // Atomic: a truncated file from a mid-write kill would
            // quietly break the id's pins and hidden state.
            if (src == null) {
                managedDir.mkdirs()
                val file = DesktopEntryFile.newFile(managedDir, draft.name)
                atomicWriteText(file, DesktopEntryFile.serialize(draft))
                id = DesktopEntryFile.idFor(file.path)
            } else {
                val dest = target!!
                id = entryId!!
                val text = DesktopEntryFile.patch(sourceText, draft)
                val packaged = shadows?.let { runCatching { it.readText() }.getOrNull() }
                if (src == dest && text == packaged) {
                    if (!dest.delete() && dest.exists()) throw IOException(getString(R.string.editor_delete_failed))
                } else if (text != sourceText) {
                    dest.parentFile?.mkdirs()
                    atomicWriteText(dest, text)
                }
            }
        } catch (e: IOException) {
            Toast.makeText(this, getString(R.string.editor_save_failed, e.message), Toast.LENGTH_LONG).show()
            return
        }
        val key = graphicsPick?.key
        if (key != storedGraphics) store.update(installId) { it.withEntryGraphics(id, key) }
        val pointerKey = pointerPick?.key
        if (pointerKey != storedPointer) store.update(installId) { it.withEntryPointerEmulation(id, pointerKey) }
        setResult(RESULT_OK)
        finish()
    }

    /**
     * Delete (personal entry) or Reset (override): remove the managed
     * file, if [source] is one, and the per-entry overrides, so a later
     * entry reusing the slug doesn't inherit it.
     */
    private fun confirmRemove(reset: Boolean) {
        val src = source ?: return
        val id = entryId ?: return
        val label = nameField.text.toString().ifBlank { id }
        AlertDialog.Builder(this)
            .setMessage(getString(if (reset) R.string.editor_reset_confirm else R.string.editor_delete_confirm, label))
            .setPositiveButton(if (reset) R.string.editor_reset else R.string.editor_delete) { _, _ ->
                if (src == target && !src.delete() && src.exists()) {
                    Toast.makeText(this, R.string.editor_delete_failed, Toast.LENGTH_LONG).show()
                    return@setPositiveButton
                }
                if (storedGraphics != null) store.update(installId) { it.withEntryGraphics(id, null) }
                if (storedPointer != null) store.update(installId) { it.withEntryPointerEmulation(id, null) }
                setResult(RESULT_OK)
                finish()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    companion object {
        /** Installation id whose rootfs hosts the managed dir. */
        const val EXTRA_ID = "id"

        /** Absolute path of the `.desktop` file to edit (any scanned
         *  entry, [LauncherEntry.path]); absent = new entry. */
        const val EXTRA_PATH = "path"

        /** [LauncherEntry.shadows] of the entry at [EXTRA_PATH]. */
        const val EXTRA_SHADOWS = "shadows"

        /** Icon preview edge, close to the launcher grid's icon. */
        private const val PREVIEW_DP = 48f

        private const val PREVIEW_DEBOUNCE_MS = 250L
    }
}
