package me.phie.tawc.install

import android.content.Context
import android.content.DialogInterface
import android.graphics.Typeface
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import me.phie.tawc.R
import me.phie.tawc.install.distro.DistroRegistry

/**
 * "Run command in <distro>" prompt; Run hands the line to
 * [RunCommandOp.start]. Opened from the home screen's ⋮ menu.
 */
fun AppCompatActivity.showRunCommandDialog(installation: Installation) {
    val pad = (16 * resources.displayMetrics.density).toInt()
    val input = EditText(this).apply {
        hint = getString(R.string.hint_run_command)
        // URI variation kills Gboard autocorrect (which ignores
        // TYPE_TEXT_FLAG_NO_SUGGESTIONS) like the old VISIBLE_PASSWORD
        // trick, minus the password semantics: on a password-type field
        // Firefox's autofill still threw an "Unlock Firefox" chip even
        // with importantForAutofill=NO (verified on device 2026-08-09).
        // The opt-out stays as a second layer.
        inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_VARIATION_URI
        importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
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
        .setTitle(getString(R.string.distro_info_run_command_title, DistroRegistry.displayLabel(installation)))
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
