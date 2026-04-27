package me.phie.tawc.ui

import android.content.res.ColorStateList
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import me.phie.tawc.R

/**
 * Helpers for building screens that share the app's chrome — a top bar
 * with the platform-standard back/up affordance plus a vertically
 * stacked content area — without duplicating boilerplate across
 * activities.
 *
 * Layouts are still built imperatively in Kotlin; these helpers just
 * install the toolbar and hand back the inner column so the caller can
 * keep using `addView(...)` like before.
 */

data class Scaffold(
    val root: LinearLayout,
    val toolbar: MaterialToolbar,
    val content: LinearLayout,
)

/**
 * Build a child screen (Install, Uninstall, Distro info, …): toolbar
 * with the up arrow + a content column. Tapping the up arrow calls
 * [AppCompatActivity.finish], i.e. the screen pops back to its parent.
 */
fun AppCompatActivity.buildChildScreen(title: CharSequence): Scaffold =
    buildScreenInternal(title, withUp = true)

/** Top-level screen (Home): toolbar with title only, no up arrow. */
fun AppCompatActivity.buildHomeScreen(title: CharSequence): Scaffold =
    buildScreenInternal(title, withUp = false)

private fun AppCompatActivity.buildScreenInternal(title: CharSequence, withUp: Boolean): Scaffold {
    val root = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
    }

    val toolbar = MaterialToolbar(this).apply {
        this.title = title
        if (withUp) {
            setNavigationIcon(R.drawable.ic_arrow_back)
            // Use the platform's "Navigate up" string so TalkBack reads
            // the same affordance users already know from other apps.
            setNavigationContentDescription(androidx.appcompat.R.string.abc_action_bar_up_description)
            setNavigationOnClickListener { finish() }
        }
    }
    root.addView(toolbar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))

    val pad = (16 * resources.displayMetrics.density).toInt()
    val content = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(pad, pad, pad, pad)
    }
    root.addView(content, LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))

    return Scaffold(root, toolbar, content)
}

// MaterialButton's default fully-rounded "pill" looks slick but reads
// as a chip more than an action; the app prefers near-square buttons
// with just enough corner softening to not feel sharp. 6dp on the
// usual ~52dp button height lands at "barely rounded".
private const val BUTTON_CORNER_DP = 6f

private fun AppCompatActivity.applyNearSquareCorners(btn: MaterialButton) {
    btn.cornerRadius = (BUTTON_CORNER_DP * resources.displayMetrics.density).toInt()
}

/**
 * Filled accent-colored button for primary actions (Install, Open).
 * Inherits `colorPrimary` (yellow-orange) from the theme.
 */
fun AppCompatActivity.primaryButton(label: CharSequence, onClick: () -> Unit): MaterialButton =
    MaterialButton(this).apply {
        text = label
        applyNearSquareCorners(this)
        setOnClickListener { onClick() }
    }

/**
 * Filled red button for destructive actions (Uninstall). Tinted
 * programmatically — no XML style indirection — so it's resilient
 * against future Material widget churn.
 */
fun AppCompatActivity.destructiveButton(label: CharSequence, onClick: () -> Unit): MaterialButton =
    MaterialButton(this).apply {
        text = label
        backgroundTintList = ColorStateList.valueOf(getColor(R.color.tawc_danger))
        setTextColor(getColor(R.color.tawc_on_danger))
        iconTint = ColorStateList.valueOf(getColor(R.color.tawc_on_danger))
        applyNearSquareCorners(this)
        setOnClickListener { onClick() }
    }

/** Convenience: vertical [LinearLayout.LayoutParams] with a bottom margin. */
fun verticalLp(width: Int, height: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
    LinearLayout.LayoutParams(width, height).also { it.bottomMargin = bottomMargin }
