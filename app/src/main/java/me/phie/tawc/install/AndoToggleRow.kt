package me.phie.tawc.install

import android.app.Activity
import android.content.Context
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import me.phie.tawc.AndoBrokers
import me.phie.tawc.R
import java.util.concurrent.Executor

/**
 * The shared ando toggle row (notes/ando.md): checkbox + the one-line
 * description, used by both the install form ([InstallActivity]) and
 * the Settings distro card ([buildAndoCommitRow]) — the two differ only in what
 * [onChange] does. The checkbox is passed back so a commit-path caller
 * can revert it on failure.
 */
internal fun buildAndoToggleRow(
    context: Context,
    checked: Boolean,
    onChange: (checkbox: CheckBox, checked: Boolean) -> Unit,
): LinearLayout {
    val container = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    container.addView(
        CheckBox(context).apply {
            text = context.getString(R.string.ando_toggle_label)
            isChecked = checked
            setOnCheckedChangeListener { _, c -> onChange(this, c) }
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    container.addView(
        TextView(context).apply {
            text = context.getString(R.string.ando_toggle_description)
            textSize = 12f
            alpha = 0.7f
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    return container
}

/**
 * ando toggle row for an existing install. Toggling read-modify-writes
 * [Installation.andoEnabled] through [store] on [executor] — which must
 * be single-threaded so rapid taps commit in click order — re-checking
 * the state gate at write time (mirrors [ManageBindsActivity.commit]) so
 * a slot that slipped into INSTALLING/UNINSTALLING under us is left
 * alone, then reconciles the broker via [AndoBrokers.refresh].
 * Take-effect: disable is immediate (listener down, in-flight ando
 * children killed); enable applies to the next rootfs spawn.
 */
internal fun buildAndoCommitRow(
    activity: Activity,
    store: InstallationStore,
    installation: Installation,
    executor: Executor,
): LinearLayout {
    var reverting = false
    return buildAndoToggleRow(activity, installation.andoEnabled) { checkbox, checked ->
        if (reverting) return@buildAndoToggleRow
        executor.execute {
            // Gate + write atomically under the store's per-id lock.
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
                AndoBrokers.refresh(activity)
            } else {
                activity.runOnUiThread {
                    Toast.makeText(
                        activity,
                        activity.getString(R.string.ando_toggle_enable_failed),
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
