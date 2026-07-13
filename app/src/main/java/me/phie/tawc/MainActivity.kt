package me.phie.tawc

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.content.res.ColorStateList
import android.os.Build
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import com.google.android.material.button.MaterialButton
import me.phie.tawc.install.DistroInfoActivity
import me.phie.tawc.install.InstallActivity
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.TawcrootMethod
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.launcher.LauncherActivity
import me.phie.tawc.terminal.TerminalActivity
import me.phie.tawc.tasks.TaskManagerActivity
import me.phie.tawc.ui.buildHomeScreen
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.tonalButton
import me.phie.tawc.ui.tawcButtonSizePx
import me.phie.tawc.ui.tonalIconButton
import me.phie.tawc.ui.verticalLp

/**
 * Home screen for the tawc app. Renders a card for each currently-installed
 * Linux environment with two actions: Info (opens [DistroInfoActivity]) and
 * Run (opens [LauncherActivity] to pick an app). The compositor starts lazily
 * when a user launches a rootfs command, so a broken graphics backend doesn't
 * keep the home screen or Settings from opening.
 */
class MainActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private val cardMargin by lazy { (8 * resources.displayMetrics.density).toInt() }
    private val cardPad by lazy { (16 * resources.displayMetrics.density).toInt() }

    private lateinit var listContainer: LinearLayout
    private lateinit var installButton: MaterialButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        requestNotificationPermissionIfNeeded()

        val scaffold = buildHomeScreen(getString(R.string.app_name))

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        scaffold.content.addView(listContainer, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))

        scaffold.content.addView(
            tonalButton(getString(R.string.title_task_manager)) {
                startActivity(Intent(this@MainActivity, TaskManagerActivity::class.java))
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
        )

        scaffold.content.addView(
            tonalButton(getString(R.string.title_settings)) {
                startActivity(Intent(this@MainActivity, SettingsActivity::class.java))
            },
            verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
        )

        installButton = tonalButton(getString(R.string.action_install_new_distro)) {
            startActivity(Intent(this@MainActivity, InstallActivity::class.java))
        }
        scaffold.content.addView(installButton, verticalLp(MATCH_PARENT, WRAP_CONTENT))

        setContentView(scaffold.root)
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    // On API 33+ foreground-service notifications (install progress, the
    // running compositor) are suppressed unless POST_NOTIFICATIONS is granted.
    // Best-effort: we don't act on the result, the install still runs either way.
    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        val perm = Manifest.permission.POST_NOTIFICATIONS
        if (checkSelfPermission(perm) == PackageManager.PERMISSION_GRANTED) return
        ActivityCompat.requestPermissions(this, arrayOf(perm), REQUEST_NOTIFICATIONS)
    }

    private fun refresh() {
        listContainer.removeAllViews()
        val installations = store.list()
        styleInstallButton(isPrimary = installations.isEmpty())
        if (installations.isEmpty()) {
            listContainer.addView(TextView(this).apply {
                text = getString(R.string.home_empty_no_distros)
                textSize = 16f
                alpha = 0.75f
                gravity = Gravity.CENTER_HORIZONTAL
            }, verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))
            return
        }
        for (inst in installations) {
            listContainer.addView(buildCard(inst), verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin))
        }
    }

    private fun styleInstallButton(isPrimary: Boolean) {
        val background = if (isPrimary) R.color.tawc_accent else R.color.tawc_tonal_bg
        val foreground = getColor(R.color.tawc_on_tonal)
        installButton.backgroundTintList = ColorStateList.valueOf(getColor(background))
        installButton.setTextColor(foreground)
        installButton.iconTint = ColorStateList.valueOf(foreground)
    }

    private fun buildCard(inst: Installation): View {
        val card = tawcCard()
        // Bottom padding is set at the end of this function: when the
        // bottom row is shown, the terminal button's bottom margin
        // (which drops it onto the search underline) already provides
        // part of the gap, so the column's own bottom padding shrinks
        // by the same amount to keep the button's distance from the
        // card's bottom edge equal to its distance from the left edge.
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }

        // Header: title/subtitle on the left, Manage button on the top-right.
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val textCol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val title = DistroRegistry.displayLabel(inst)
        textCol.addView(TextView(this).apply {
            text = title
            textSize = 18f
        })
        // Distro line under the title — dropped when the title already
        // equals the display name (legacy records without a label).
        val displayName = DistroRegistry.forInstallation(inst)?.displayName
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"
        if (title != displayName) {
            textCol.addView(TextView(this).apply {
                text = displayName
                textSize = 14f
                alpha = 0.7f
            })
        }
        // Non-READY state marker, in red so a stuck/failed install pops.
        stateLine(inst.state)?.let { state ->
            textCol.addView(TextView(this).apply {
                text = state
                textSize = 14f
                setTextColor(getColor(R.color.tawc_danger))
            })
        }
        val gap = (8 * resources.displayMetrics.density).toInt()
        header.addView(
            textCol,
            LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f).also { it.marginEnd = gap },
        )
        val btnSize = tawcButtonSizePx()
        header.addView(
            tonalIconButton(
                R.drawable.ic_settings_gear,
                getString(R.string.action_manage),
                iconSizeDp = 24,
            ) {
                val i = Intent(this@MainActivity, DistroInfoActivity::class.java)
                    .putExtra(DistroInfoActivity.EXTRA_ID, inst.id)
                startActivity(i)
            },
            LinearLayout.LayoutParams(btnSize, btnSize).also { it.gravity = Gravity.TOP },
        )
        column.addView(header, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

        // Bottom row: Terminal icon button + search-apps stub. The stub
        // looks like a search field but never holds focus — tapping it
        // forwards into LauncherActivity, which is the real search UI.
        // Hidden on FAILED/CORRUPT (no usable launcher) and disabled while
        // installing/uninstalling so it returns once ready.
        val topMargin = (8 * resources.displayMetrics.density).toInt()
        // BOTTOM, not CENTER_VERTICAL: the search box's underline sits
        // at its bottom edge, so that's the line the terminal button
        // should sit on. baselineAligned must go — with it on (the
        // horizontal-LinearLayout default) the row aligns the button's
        // empty-text baseline to the EditText's instead of honoring
        // BOTTOM, pushing the button up out of the row's clip bounds.
        val bottomRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.BOTTOM
            isBaselineAligned = false
        }
        val searchBox = EditText(this).apply {
            hint = getString(R.string.hint_search_apps)
            isSingleLine = true
            isFocusable = false
            isClickable = true
            setTextColor(getColor(R.color.tawc_on_tonal))
            isEnabled = inst.state == Installation.State.READY
            setOnClickListener {
                val i = Intent(this@MainActivity, LauncherActivity::class.java)
                    .putExtra(LauncherActivity.EXTRA_ID, inst.id)
                startActivity(i)
            }
        }
        // The search box's underline draws a few px above the view's
        // bottom inside its bottom padding; this margin drops the
        // terminal button so its bottom edge lands a hair below the
        // line rather than above it (the extra +1px is deliberate —
        // dead-even read as the button floating half a pixel high).
        val underlineNudge = (3 * resources.displayMetrics.density).toInt() + 1
        val buttonDrop = (searchBox.paddingBottom - underlineNudge).coerceAtLeast(0)
        // Terminal needs a runnable rootfs and the tawcroot spawn path
        // (chroot needs su, proot is dev-only — see TerminalActivity).
        if (inst.state == Installation.State.READY && inst.method == TawcrootMethod.KEY) {
            // 28dp icon (over the 24dp default) so the ">_" reads at a
            // glance instead of floating in button padding.
            bottomRow.addView(
                tonalIconButton(
                    R.drawable.ic_terminal,
                    getString(R.string.action_terminal),
                    iconSizeDp = 28,
                ) {
                    val i = Intent(this@MainActivity, TerminalActivity::class.java)
                        .putExtra(TerminalActivity.EXTRA_ID, inst.id)
                        // Unique per-distro document URI — see the
                        // manifest comment on TerminalActivity.
                        .setData(Uri.parse("tawc://terminal/${inst.id}"))
                    startActivity(i)
                },
                LinearLayout.LayoutParams(btnSize, btnSize).also {
                    it.marginEnd = gap
                    it.bottomMargin = buttonDrop
                },
            )
        }
        bottomRow.addView(searchBox, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val rowVisible = inst.state != Installation.State.FAILED &&
            inst.state != Installation.State.CORRUPT
        bottomRow.visibility = if (rowVisible) View.VISIBLE else View.GONE
        column.addView(
            bottomRow,
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.topMargin = topMargin },
        )
        // See the comment at the top of this function: the button's
        // bottom margin counts toward the visual gap to the card edge.
        if (rowVisible) {
            column.setPadding(cardPad, cardPad, cardPad, (cardPad - buttonDrop).coerceAtLeast(0))
        }

        card.addView(column)
        return card
    }

    /** Card state marker; null for READY (no marker). */
    private fun stateLine(state: Installation.State): String? =
        when (state) {
            Installation.State.READY -> null
            Installation.State.INSTALLING -> getString(R.string.install_state_installing)
            Installation.State.UNINSTALLING -> getString(R.string.install_state_uninstalling)
            Installation.State.FAILED -> getString(R.string.install_state_failed)
            Installation.State.CORRUPT -> getString(R.string.install_state_corrupt)
        }

    private companion object {
        const val REQUEST_NOTIFICATIONS = 1
    }
}
