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
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.flatMapLatest
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
 *   - A new launchIntent arrives via [onNewIntent] (singleTop) for a
 *     different op id → the binding swaps to the new op, the frozen
 *     state from the previous op is replaced. Used by the broker's
 *     OP_TITLE path so back-to-back commands all surface here.
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

    /**
     * Currently-displayed op id. Updates when [onNewIntent] arrives so a
     * SINGLE_TOP relaunch for a different op rebinds the panel instead
     * of being silently dropped.
     */
    private val currentOpId = MutableStateFlow<String?>(null)

    /**
     * Sticky once the registry's lookup for the current op id returns
     * a non-null Operation. Used to distinguish "op terminated mid-
     * view" (keep the frozen panel) from "cold open, op never existed"
     * (show the empty-state placeholder). Reset when [currentOpId]
     * changes so a new op gets a clean cold-open state.
     */
    private var everBound = false

    @OptIn(ExperimentalCoroutinesApi::class)
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val initialId = intent?.getStringExtra(EXTRA_OPERATION_ID)
        if (initialId.isNullOrEmpty()) {
            android.util.Log.w(TAG, "LogScreenActivity launched without an operationId — finishing")
            finish()
            return
        }
        currentOpId.value = initialId

        scaffold = buildChildScreen(initialId)
        panel = OperationLogPanel(this)
        emptyView = TextView(this).apply {
            setTextColor(
                MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant)
            )
        }
        scaffold.content.addView(panel.view, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        scaffold.content.addView(emptyView, LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        // Both views start invisible; the registry collector below sets
        // exactly one of them visible based on whether the current op
        // id resolves.
        panel.view.visibility = View.GONE
        emptyView.visibility = View.GONE
        setContentView(scaffold.root)

        panel.onCancelClicked = { dispatchCancel() }

        lifecycleScope.launch {
            currentOpId
                .flatMapLatest { id ->
                    if (id.isNullOrEmpty()) kotlinx.coroutines.flow.flowOf(null)
                    else OperationsRegistry.ops.map { it[id] }
                }
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
                            emptyView.text = "No operation in flight for '${currentOpId.value}'."
                            panel.view.visibility = View.GONE
                            emptyView.visibility = View.VISIBLE
                        }
                    }
                }
        }
    }

    /**
     * SINGLE_TOP relaunch for a different op id (typical: broker-driven
     * back-to-back rootfs commands, each opening their own log screen).
     * Update [currentOpId] so the registry collector swaps bindings;
     * reset the frozen-on-terminal sticky so the new op gets the empty
     * placeholder if it's already gone.
     */
    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        val newId = intent?.getStringExtra(EXTRA_OPERATION_ID)
        if (newId.isNullOrEmpty() || newId == currentOpId.value) return
        everBound = false
        // Clear the panel's previously-rendered text so the user
        // doesn't briefly see the old op's frozen log lines under the
        // new op's toolbar title before the registry collector has
        // applied the binding.
        panel.unbind()
        panel.reset()
        currentOpId.value = newId
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
