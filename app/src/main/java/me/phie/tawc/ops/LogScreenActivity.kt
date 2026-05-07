package me.phie.tawc.ops

import android.content.Context
import android.content.DialogInterface
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import me.phie.tawc.R
import me.phie.tawc.ui.Scaffold
import me.phie.tawc.ui.buildChildScreen

/**
 * Generic viewer for any [Operation] in [OperationsRegistry]. Looks up
 * the op by id from its launch intent, renders an [OperationLogPanel]
 * bound to it, and follows the registry as the op's lifetime evolves.
 *
 * **Pure viewer.** Opening this activity never side-effects: no install
 * runs, no uninstall fires. Triggers live elsewhere (the Install form's
 * button, the Delete dialog, the dev exec broker).
 *
 * Lifetime relative to the op:
 *   - Op currently in the registry → panel binds, status / log /
 *     Cancel update live.
 *   - Op leaves the registry while we're bound (terminal-then-
 *     unregister) → panel unbinds, last-rendered status and log
 *     remain frozen on screen so the user can read what happened.
 *   - Cold open against an id that's not in the registry → "no such
 *     operation" placeholder. (Per the project's "unregister
 *     immediately on terminal" choice, the registry holds no history.)
 *
 * Launch:
 *   `LogScreenActivity.intentFor(ctx, "install:arch")` from in-app
 *   callers, or `am start … --es operationId <id>` from adb to attach
 *   to a running op.
 *
 * **Threat model.** `exported="true"` is intentional so adb workflows
 * can attach. That means another installed app can launch this
 * activity for any [Operation.id] (it's just an intent extra) and
 * read the live log stream while the op is in flight. The op log is
 * therefore treated as "user-visible content," not a place to dump
 * sensitive material — install/uninstall log lines are limited to
 * filenames, package versions, and error messages, all of which are
 * already on logcat (`adb logcat -s tawc-install`). Mutating the op
 * still requires going through [Operation.cancel], which the on-screen
 * Cancel button calls but no intent extra triggers.
 */
class LogScreenActivity : AppCompatActivity() {

    private lateinit var scaffold: Scaffold
    private lateinit var panel: OperationLogPanel
    private lateinit var emptyView: TextView

    private var opId: String? = null

    /**
     * Sticky once the registry's lookup for [opId] returns a non-null
     * Operation. Used to distinguish "op terminated mid-view" (keep
     * the frozen panel) from "cold open, op never existed" (show the
     * empty-state placeholder).
     */
    private var everBound = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        opId = intent?.getStringExtra(EXTRA_OPERATION_ID)
        if (opId.isNullOrEmpty()) {
            android.util.Log.w(TAG, "LogScreenActivity launched without an operationId — finishing")
            finish()
            return
        }

        scaffold = buildChildScreen(opId!!)
        panel = OperationLogPanel(this)
        emptyView = TextView(this).apply {
            text = "No operation in flight for '$opId'."
            setTextColor(
                MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
        }
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        scaffold.content.addView(emptyView, LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        // Both views start invisible; the registry collector below sets
        // exactly one of them visible based on whether [opId] resolves.
        panel.view.visibility = View.GONE
        emptyView.visibility = View.GONE
        setContentView(scaffold.root)

        panel.onCancelClicked = { dispatchCancel() }

        lifecycleScope.launch {
            OperationsRegistry.ops
                .map { it[opId] }
                .distinctUntilChanged()
                .collect { op ->
                    if (op != null) {
                        everBound = true
                        scaffold.toolbar.title = op.title
                        emptyView.visibility = View.GONE
                        panel.view.visibility = View.VISIBLE
                        panel.bind(op)
                    } else {
                        panel.unbind()
                        if (everBound) {
                            // The op terminated mid-view; keep the panel
                            // up showing its frozen final state.
                            panel.view.visibility = View.VISIBLE
                            emptyView.visibility = View.GONE
                        } else {
                            panel.view.visibility = View.GONE
                            emptyView.visibility = View.VISIBLE
                        }
                    }
                }
        }
    }

    private fun dispatchCancel() {
        val op = panel.boundOperation ?: return
        confirmAndCancel(op)
    }

    companion object {
        private const val TAG = "tawc-ops"
        const val EXTRA_OPERATION_ID = "operationId"

        fun intentFor(context: Context, operationId: String): Intent =
            Intent(context, LogScreenActivity::class.java)
                .putExtra(EXTRA_OPERATION_ID, operationId)
    }
}

/**
 * Activity-side cancel handler shared between [LogScreenActivity] and
 * any other view that hosts an [OperationLogPanel] (e.g.
 * [me.phie.tawc.install.InstallActivity]). Reads
 * [Operation.cancelConfirmation]; if non-null, shows a destructive
 * confirm dialog before invoking [Operation.cancel].
 */
internal fun AppCompatActivity.confirmAndCancel(op: Operation) {
    val confirm = op.cancelConfirmation
    if (confirm == null) {
        op.cancel()
        return
    }
    val dialog = MaterialAlertDialogBuilder(this)
        .setTitle(confirm.title)
        .setMessage(confirm.message)
        .setNegativeButton(confirm.keepLabel, null)
        .setPositiveButton(confirm.confirmLabel) { _, _ -> op.cancel() }
        .show()
    dialog.getButton(DialogInterface.BUTTON_POSITIVE)?.setTextColor(getColor(R.color.tawc_danger))
    dialog.getButton(DialogInterface.BUTTON_NEGATIVE)?.let { btn ->
        btn.setTextColor(
            MaterialColors.getColor(btn, com.google.android.material.R.attr.colorOnSurfaceVariant)
        )
    }
}
