package me.phie.tawc.ui

import android.content.Context
import android.content.res.ColorStateList
import android.view.ContextThemeWrapper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.content.res.AppCompatResources
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.drawerlayout.widget.DrawerLayout
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.floatingactionbutton.FloatingActionButton
import com.google.android.material.navigation.NavigationView
import com.google.android.material.shape.RelativeCornerSize
import com.google.android.material.shape.ShapeAppearanceModel
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
    buildScreenInternal(title, withUp = false).also {
        it.toolbar.setTitleCentered(true)
        it.toolbar.setTitleTextAppearance(this, R.style.TextAppearance_Tawc_HomeTitle)
    }

/**
 * [buildHomeScreen] inside a [DrawerLayout] with a start-edge
 * [NavigationView]: hamburger in the toolbar opens it, Back closes it.
 * [DrawerScreen.body] overlays the content column so a FAB can sit
 * bottom-right. Set `setContentView(drawerScreen.drawer)`.
 */
class DrawerScreen(
    val drawer: DrawerLayout,
    val nav: NavigationView,
    val scaffold: Scaffold,
    val body: FrameLayout,
)

fun AppCompatActivity.buildDrawerScreen(title: CharSequence): DrawerScreen {
    // System-bar insets stay on the main column (buildScreenInternal):
    // the drawer runs under the status bar and pads its own contents.
    val home = buildScreenInternal(title, withUp = false).also {
        it.toolbar.setTitleCentered(true)
        it.toolbar.setTitleTextAppearance(this, R.style.TextAppearance_Tawc_HomeTitle)
    }
    // Re-parent the content column into a FrameLayout so overlays
    // (the FAB) can float over it.
    home.root.removeView(home.content)
    val body = FrameLayout(this)
    body.addView(home.content, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    home.root.addView(body, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

    val drawer = DrawerLayout(this)
    drawer.addView(home.root, DrawerLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
    val surfaces = ContextThemeWrapper(this, R.style.ThemeOverlay_Tawc_Surfaces)
    val nav = NavigationView(surfaces).apply {
        fitsSystemWindows = true
        // The inset scrims paint grey bands over the drawer's white.
        isTopInsetScrimEnabled = false
        isBottomInsetScrimEnabled = false
    }
    drawer.addView(
        nav,
        DrawerLayout.LayoutParams(WRAP_CONTENT, MATCH_PARENT).also { it.gravity = Gravity.START },
    )
    home.toolbar.popupTheme = R.style.ThemeOverlay_Tawc_Surfaces

    home.toolbar.setNavigationIcon(R.drawable.ic_menu)
    home.toolbar.setNavigationContentDescription(R.string.action_open_drawer)
    home.toolbar.setNavigationOnClickListener { drawer.openDrawer(nav) }

    val backCloses = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = drawer.closeDrawer(nav)
    }
    onBackPressedDispatcher.addCallback(this, backCloses)
    drawer.addDrawerListener(object : DrawerLayout.SimpleDrawerListener() {
        override fun onDrawerOpened(drawerView: View) { backCloses.isEnabled = true }
        override fun onDrawerClosed(drawerView: View) { backCloses.isEnabled = false }
    })

    return DrawerScreen(drawer, nav, home, body)
}

private fun AppCompatActivity.buildScreenInternal(title: CharSequence, withUp: Boolean): Scaffold {
    val root = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT)
    }
    root.applySystemBarPadding()

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

private fun View.applySystemBarPadding() {
    ViewCompat.setOnApplyWindowInsetsListener(this) { view, insets ->
        val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
        insets
    }
    ViewCompat.requestApplyInsets(this)
}

// MaterialButton's default fully-rounded "pill" looks slick but reads
// as a chip more than an action; the app prefers near-square buttons
// with just enough corner softening to not feel sharp. 6dp on the
// 48dp button height lands at "barely rounded".
private const val BUTTON_CORNER_DP = 6f

// Uniform visible height for every app button (text and icon). The
// Material default is ~48dp of layout with 6dp transparent insets top
// and bottom, which makes mixed text/icon rows look misaligned; zero
// the insets and pin minHeight instead so the painted area is the
// same everywhere. 44dp: 48 read slightly chunky next to the cards'
// text rows.
private const val BUTTON_HEIGHT_DP = 44

// Glyph edge for [plainIconButton]: what a toolbar's own up arrow
// draws at, not MaterialButton's 18dp default (which is sized to sit
// beside a label, and reads small on its own).
private const val PLAIN_ICON_SIZE_DP = 24

/**
 * The uniform button height in pixels — also the exact width/height
 * callers should give [tonalIconButton]s' layout params so the squares
 * can't be squeezed by their row (LinearLayout only guarantees
 * minHeight for exact-size children).
 */
fun Context.tawcButtonSizePx(): Int = (BUTTON_HEIGHT_DP * resources.displayMetrics.density).toInt()

private fun MaterialButton.applyTawcButtonShape() {
    val density = resources.displayMetrics.density
    insetTop = 0
    insetBottom = 0
    minHeight = (BUTTON_HEIGHT_DP * density).toInt()
    minimumHeight = minHeight
    cornerRadius = (BUTTON_CORNER_DP * density).toInt()
}

/**
 * Filled accent-colored button for primary actions (Install, Open).
 * Inherits `colorPrimary` from the theme.
 */
fun AppCompatActivity.primaryButton(label: CharSequence, onClick: () -> Unit): MaterialButton =
    MaterialButton(this).apply {
        text = label
        applyTawcButtonShape()
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
        applyTawcButtonShape()
        setOnClickListener { onClick() }
    }

/**
 * Tonal Material button for secondary / subdued actions (Manage,
 * Cancel, Task manager). Muted fill, no border, same
 * near-square corners as [primaryButton] — reads clearly as "a button,
 * but not the headline action." Context-bound so non-Activity UI
 * surfaces (e.g. `OperationLogPanel`) can use it too.
 */
fun Context.tonalButton(label: CharSequence, onClick: () -> Unit): MaterialButton =
    MaterialButton(this).apply {
        text = label
        backgroundTintList = ColorStateList.valueOf(getColor(R.color.tawc_tonal_bg))
        setTextColor(getColor(R.color.tawc_on_tonal))
        applyTawcButtonShape()
        setOnClickListener { onClick() }
    }

/**
 * Square icon-only variant of [tonalButton]; base of
 * [plainIconButton]. Fixed
 * [BUTTON_HEIGHT_DP]-square so every icon button matches the text
 * buttons' height regardless of icon size. MaterialButton centers a
 * TEXT_START icon when there's no text and iconPadding is 0.
 * Colors default to the tonal pair; pass e.g. [R.color.tawc_accent]
 * for an accent-filled button (same pairing as the primary-styled
 * install button).
 */
fun Context.tonalIconButton(
    iconRes: Int,
    description: CharSequence,
    backgroundColor: Int = R.color.tawc_tonal_bg,
    foregroundColor: Int = R.color.tawc_on_tonal,
    iconSizeDp: Int? = null,
    onClick: () -> Unit,
): MaterialButton =
    MaterialButton(this).apply {
        icon = AppCompatResources.getDrawable(context, iconRes)
        if (iconSizeDp != null) iconSize = (iconSizeDp * resources.displayMetrics.density).toInt()
        iconPadding = 0
        iconGravity = MaterialButton.ICON_GRAVITY_TEXT_START
        applyTawcButtonShape()
        val size = (BUTTON_HEIGHT_DP * resources.displayMetrics.density).toInt()
        minWidth = size
        minimumWidth = size
        setPadding(0, 0, 0, 0)
        contentDescription = description
        backgroundTintList = ColorStateList.valueOf(getColor(backgroundColor))
        iconTint = ColorStateList.valueOf(getColor(foregroundColor))
        setOnClickListener { onClick() }
    }

/**
 * Background-less [tonalIconButton] for chrome that sits inside
 * another surface (the launcher's back / ⋮ buttons beside the search
 * field), where a filled square would read as a second control rather
 * than part of the row. Same square footprint, no fill — only the
 * press ripple shows, so the shape is a circle (a rounded square
 * looks like a stray button once the fill is gone).
 *
 * The glyph is [PLAIN_ICON_SIZE_DP] at `?attr/colorControlNormal`,
 * i.e. what a toolbar's own up arrow draws: these buttons are the same
 * kind of chrome, so a screen's back arrow and a row's must be one
 * mark. Pass [foregroundColor] only where the colour carries meaning,
 * and [iconSizeDp] only for a glyph that reads heavier than the rest
 * at the shared size.
 */
fun Context.plainIconButton(
    iconRes: Int,
    description: CharSequence,
    foregroundColor: Int? = null,
    iconSizeDp: Int = PLAIN_ICON_SIZE_DP,
    onClick: () -> Unit,
): MaterialButton =
    tonalIconButton(
        iconRes,
        description,
        android.R.color.transparent,
        foregroundColor ?: R.color.tawc_on_tonal,
        iconSizeDp,
        onClick,
    ).apply {
        cornerRadius = tawcButtonSizePx() / 2
        if (foregroundColor == null) iconTint = controlTint()
    }

/** `?attr/colorControlNormal`, the tint the platform's own chrome uses. */
private fun Context.controlTint(): ColorStateList {
    val value = android.util.TypedValue()
    theme.resolveAttribute(androidx.appcompat.R.attr.colorControlNormal, value, true)
    return if (value.resourceId != 0) {
        AppCompatResources.getColorStateList(this, value.resourceId)
    } else {
        ColorStateList.valueOf(value.data)
    }
}

/**
 * Card / panel surface used for distro rows on the home screen, the
 * task manager's per-install group cards, the launcher's app rows, and
 * the operation log panel. Filled with [R.color.tawc_card_bg] (a
 * slight contrast against the window surface) and no stroke — the
 * fill alone is what separates the card from the background.
 */
fun Context.tawcCard(): MaterialCardView =
    MaterialCardView(this).apply {
        strokeWidth = 0
        cardElevation = 0f
        setCardBackgroundColor(getColor(R.color.tawc_card_bg))
    }

/**
 * Round accent floating action button for a screen's one headline
 * action (the home screen's Terminal). Same accent/on-tonal pairing as
 * the primary-styled install button. Add it to a [DrawerScreen.body]
 * with [fabLp].
 */
fun Context.tawcFab(iconRes: Int, description: CharSequence, onClick: () -> Unit): FloatingActionButton =
    FloatingActionButton(this).apply {
        setImageResource(iconRes)
        contentDescription = description
        backgroundTintList = ColorStateList.valueOf(getColor(R.color.tawc_accent))
        imageTintList = ColorStateList.valueOf(getColor(R.color.tawc_on_tonal))
        shapeAppearanceModel = ShapeAppearanceModel.builder()
            .setAllCornerSizes(RelativeCornerSize(0.5f))
            .build()
        setOnClickListener { onClick() }
    }

/** Bottom-end placement for [tawcFab] inside a FrameLayout. */
fun Context.fabLp(): FrameLayout.LayoutParams {
    val margin = (16 * resources.displayMetrics.density).toInt()
    return FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).also {
        it.gravity = Gravity.BOTTOM or Gravity.END
        it.setMargins(margin, margin, margin, margin)
    }
}

/** Convenience: vertical [LinearLayout.LayoutParams] with a bottom margin. */
fun verticalLp(width: Int, height: Int, bottomMargin: Int = 0): LinearLayout.LayoutParams =
    LinearLayout.LayoutParams(width, height).also { it.bottomMargin = bottomMargin }
