package me.phie.tawc.install

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import me.phie.tawc.R
import me.phie.tawc.install.distro.CHINA_MIRROR_REGION_ID
import me.phie.tawc.install.distro.Distro
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.install.distro.cjkFontCommand
import java.util.concurrent.Executor

/**
 * Per-install package-mirror row for the Settings distro card: the
 * label, a dropdown showing the selected region
 * ([Installation.mirrorRegion]) and a detail line. The dropdown lists
 * the distro's built-in presets (`Distro.mirrorRegions`) plus a
 * "Default" entry that clears the choice.
 *
 * A dropdown rather than a tappable row: the row used to read
 * "Package mirror: China" with no affordance at all, so nothing said
 * the value could be changed (or what the alternatives are).
 *
 * Picking a region rewrites the installed rootfs's mirror config as well
 * as the metadata. `Distro.configure` only ever runs during an install
 * (notes/installation.md "Upgrade policy"), so without that second step
 * the setting would look applied while every `pacman -S` kept using the
 * old list. The rewrite happens first and the choice is persisted only
 * on success — a region recorded but not applied is the misleading
 * state, so on failure the dropdown snaps back to what the rootfs
 * actually has.
 *
 * Rows only make sense for distros with a non-empty
 * [Distro.mirrorRegions]; the caller mounts nothing otherwise. Pacman's
 * mirrorlist is the only config a preset rewrites today — distros whose
 * repository path carries release policy (Manjaro ARM) or is already a
 * geo-routed CDN (Debian, Void) declare no regions.
 *
 * [executor] must be single-threaded so rapid picks apply in click order.
 */
internal fun buildMirrorRegionRow(
    activity: Activity,
    store: InstallationStore,
    installation: Installation,
    executor: Executor,
): View {
    val distro = DistroRegistry.forInstallation(installation)
    val regions = distro?.mirrorRegions.orEmpty()

    // Index 0 is "Default" (no region); the rest are the distro's
    // presets, in declaration order.
    val ids: List<String?> = listOf(null) + regions.map { it.id }
    fun labelFor(id: String?): String =
        regions.firstOrNull { it.id == id }?.label
            ?: activity.getString(R.string.settings_mirror_region_default)

    val container = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
    val row = LinearLayout(activity).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }
    row.addView(
        TextView(activity).apply {
            text = activity.getString(R.string.settings_mirror_region_label)
            textSize = 16f
            setTypeface(typeface, Typeface.BOLD)
        },
        LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f),
    )
    container.addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
    container.addView(
        TextView(activity).apply {
            text = activity.getString(R.string.settings_mirror_region_detail)
            textSize = 12f
            alpha = 0.7f
        },
        LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
    )
    if (distro == null || regions.isEmpty()) return container

    // Re-entrancy guard for the two programmatic selection changes: the
    // initial one below (suppressed anyway) and the revert on failure,
    // which must not look like a fresh user pick.
    var suppress = false

    // What the rootfs actually has. `installation` is a snapshot from when
    // the screen was built, so it goes stale on the first successful pick —
    // and with it both the "already selected" short-circuit and the revert
    // target: a picked-but-not-applied value would otherwise leave the
    // dropdown claiming a region the mirrorlist does not contain.
    var applied = installation.mirrorRegion

    val spinner = Spinner(activity).apply {
        adapter = ArrayAdapter(
            activity,
            android.R.layout.simple_spinner_item,
            ids.map { labelFor(it) },
        ).apply { setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
        contentDescription = activity.getString(R.string.settings_mirror_region_label)
        setSelection(ids.indexOf(installation.mirrorRegion).coerceAtLeast(0), false)
        onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (suppress) return
                val picked = ids[position]
                if (picked == applied) return
                executor.execute {
                    val error = applyMirrorRegion(activity, store, installation, distro, picked)
                    activity.runOnUiThread {
                        if (error == null) {
                            applied = picked
                            if (picked == CHINA_MIRROR_REGION_ID) {
                                showCjkFontHint(activity, distro, labelFor(picked))
                            }
                        } else {
                            Toast.makeText(
                                activity,
                                activity.getString(R.string.settings_mirror_region_failed, error),
                                Toast.LENGTH_LONG,
                            ).show()
                            // Back to what the rootfs really has.
                            suppress = true
                            setSelection(ids.indexOf(applied).coerceAtLeast(0), false)
                            suppress = false
                        }
                    }
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }
    row.addView(spinner, LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
    return container
}

/**
 * Mirror regions are about download speed; CJK glyphs are a font
 * package. Users who pick the China preset are exactly the ones who hit
 * the missing glyphs, so say it once, right after the switch, with the
 * distro's own command when we know it.
 */
private fun showCjkFontHint(activity: Activity, distro: Distro, regionLabel: String) {
    val command = cjkFontCommand(distro)
    val message = buildString {
        append(activity.getString(R.string.settings_mirror_region_cjk_message, regionLabel))
        append("\n\n")
        append(command ?: activity.getString(R.string.settings_mirror_region_cjk_unknown))
    }
    val builder = AlertDialog.Builder(activity)
        .setTitle(R.string.settings_mirror_region_cjk_title)
        .setMessage(message)
        .setPositiveButton(android.R.string.ok, null)
    if (command != null) {
        builder.setNeutralButton(R.string.settings_mirror_region_cjk_copy) { _, _ ->
            activity.getSystemService(ClipboardManager::class.java)
                ?.setPrimaryClip(ClipData.newPlainText("tawc-cjk", command))
            Toast.makeText(
                activity,
                R.string.settings_mirror_region_cjk_copied,
                Toast.LENGTH_SHORT,
            ).show()
        }
    }
    builder.show()
}

/**
 * Rewrite [installation]'s mirror config for [regionId] and persist the
 * choice. Returns null on success, else a message for the user.
 *
 * The state gate is re-checked inside the store's update (same rule as
 * [buildAndoCommitRow]): a slot that slipped into INSTALLING or
 * UNINSTALLING under us must not have its rootfs rewritten.
 */
private fun applyMirrorRegion(
    activity: Activity,
    store: InstallationStore,
    installation: Installation,
    distro: Distro,
    regionId: String?,
): String? {
    val method = InstallationMethod.forKey(activity, installation.method)
        ?: return activity.getString(R.string.settings_mirror_region_no_method, installation.method)
    val rootfs = store.rootfsDir(installation.id).absolutePath
    try {
        distro.configureMirrors(method, rootfs, regionId) { }
    } catch (e: Exception) {
        return e.message ?: e.javaClass.simpleName
    }
    val saved = store.update(installation.id) { current ->
        if (current.state == Installation.State.READY || current.state == Installation.State.FAILED) {
            current.copy(mirrorRegion = regionId)
        } else {
            null
        }
    }
    return if (saved == null) activity.getString(R.string.settings_mirror_region_stale) else null
}
