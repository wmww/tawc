package me.phie.tawc.terminal

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Rect
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.LayerDrawable
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.HorizontalScrollView
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import me.phie.tawc.R

/**
 * Top row of the home screen's [DistroHome]:
 * `[≡][⊞][ tabs… ][+] …… [⋮]`, or `[≡] Title …… [⋮]` while there are
 * no terminal tabs. ⊞ is the apps tab: first, an icon instead of a
 * label, no `×`, shown only alongside terminal tabs. The terminal tab strip scrolls
 * horizontally; `+` sits outside it, right after the last tab, shows
 * only while there are terminal tabs, and stays in place once the tabs
 * overflow. `≡` and `⋮` are pinned at the edges. Each terminal tab is
 * an ellipsized label plus a small `×` close button.
 *
 * Imperative custom view, no XML layout (app style). Terminal tab
 * indices map 1:1 to `TerminalSessions.list(distroId)` — the bar never
 * reorders; [DistroHome] drives all mutations and supplies the
 * callbacks. Click handlers resolve the index at click time
 * (`indexOfChild`) so removals don't stale captured positions.
 *
 * The bar always blends into the window background (theme-following),
 * so it keeps its color when a terminal tab is selected.
 */
internal class TerminalTabBar(context: Context, title: CharSequence) : LinearLayout(context) {

    var onAppsSelected: () -> Unit = {}
    var onTabSelected: (Int) -> Unit = {}
    var onTabCloseClicked: (Int) -> Unit = {}
    var onNewTabClicked: () -> Unit = {}
    var onDrawerClicked: () -> Unit = {}
    var onMenuClicked: (View) -> Unit = {}

    private val scroller: HorizontalScrollView
    private val strip: LinearLayout
    private val appsTab: ImageButton
    private val titleView: TextView
    private val newTab: View
    private val tabsRow: LinearLayout
    private val buttons = mutableListOf<ImageButton>()

    private val palette = Palette(
        bg = context.getColor(R.color.tawc_window_bg),
        tabSelected = context.getColor(R.color.tawc_nav_selected),
        fgSelected = themeColor(com.google.android.material.R.attr.colorOnSurface),
        fgUnselected = themeColor(com.google.android.material.R.attr.colorOnSurfaceVariant),
    )
    private var selectedIndex = APPS

    /** Fill; MainActivity continues it into the status band. */
    val barColor: Int get() = palette.bg

    init {
        orientation = HORIZONTAL

        addView(
            barButton(R.drawable.ic_menu, R.string.action_open_drawer) { onDrawerClicked() },
            LayoutParams(dp(NEW_TAB_WIDTH_DP), MATCH_PARENT),
        )
        titleView = TextView(context).apply {
            text = title
            setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleLarge)
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(TITLE_PAD_DP), 0, 0, 0)
        }
        addView(titleView, LayoutParams(0, MATCH_PARENT, 1f))
        appsTab = barButton(R.drawable.ic_apps, R.string.action_apps) { onAppsSelected() }.apply {
            // Same glyph size as the other bar buttons in a wider cell.
            val h = dp(APPS_TAB_WIDTH_DP - NEW_TAB_WIDTH_DP) / 2 + dp(ICON_PAD_DP)
            setPadding(h, dp(ICON_PAD_DP) + dp(2), h, dp(ICON_PAD_DP) + dp(2))
            visibility = GONE
        }
        addView(appsTab, LayoutParams(dp(APPS_TAB_WIDTH_DP), MATCH_PARENT))

        strip = LinearLayout(context).apply { orientation = HORIZONTAL }
        scroller = HorizontalScrollView(context).apply {
            isHorizontalScrollBarEnabled = false
            // A tab scrolled half out reads as cut off, not as scrolled.
            isHorizontalFadingEdgeEnabled = true
            setFadingEdgeLength(dp(FADE_DP))
            addView(strip, LayoutParams(WRAP_CONTENT, MATCH_PARENT))
        }
        newTab = barButton(R.drawable.ic_add, R.string.terminal_new_tab) { onNewTabClicked() }
            .apply { visibility = GONE }
        tabsRow = object : LinearLayout(context) {
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                // Measure contents as wrap-content so `+` hugs the tabs; the
                // weighted scroller gives back any overflow. Still claim the
                // full width so `⋮` stays pinned right.
                val width = MeasureSpec.getSize(widthMeasureSpec)
                super.onMeasure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST), heightMeasureSpec)
                setMeasuredDimension(width, measuredHeight)
            }
        }.apply {
            orientation = HORIZONTAL
            addView(scroller, LayoutParams(WRAP_CONTENT, MATCH_PARENT, 1f))
            addView(newTab, LayoutParams(dp(NEW_TAB_WIDTH_DP), MATCH_PARENT))
        }
        tabsRow.visibility = GONE
        addView(tabsRow, LayoutParams(0, MATCH_PARENT, 1f))

        lateinit var menu: View
        menu = barButton(R.drawable.ic_more_vert, R.string.home_menu_description) { onMenuClicked(menu) }
        addView(menu, LayoutParams(dp(NEW_TAB_WIDTH_DP), MATCH_PARENT))
        setSelected(APPS)
    }

    private fun themeColor(attr: Int): Int {
        val value = TypedValue()
        context.theme.resolveAttribute(attr, value, true)
        return value.data
    }

    private fun barButton(icon: Int, description: Int, onClick: () -> Unit): ImageButton =
        ImageButton(context).apply {
            setImageResource(icon)
            setBackgroundColor(Color.TRANSPARENT)
            // ImageView's FIT_CENTER upscales the icon to the button
            // bounds; pad it back down to a small glyph.
            setPadding(dp(ICON_PAD_DP), dp(ICON_PAD_DP), dp(ICON_PAD_DP), dp(ICON_PAD_DP))
            contentDescription = context.getString(description)
            setOnClickListener { onClick() }
            buttons += this
        }

    /** Append a terminal tab and scroll it into view. */
    fun addTab(label: CharSequence) {
        val tab = buildTab(label)
        strip.addView(tab, LayoutParams(WRAP_CONTENT, MATCH_PARENT))
        updateTabsShown()
        scrollIntoView(tab)
    }

    fun removeTab(index: Int) {
        strip.removeViewAt(index)
        updateTabsShown()
    }

    /** Title alone with no terminal tabs; ⊞, tabs and `+` otherwise. */
    private fun updateTabsShown() {
        val tabs = tabCount() > 0
        titleView.visibility = if (tabs) GONE else VISIBLE
        appsTab.visibility = if (tabs) VISIBLE else GONE
        tabsRow.visibility = if (tabs) VISIBLE else GONE
        newTab.visibility = if (tabs) VISIBLE else GONE
    }

    private fun tabCount(): Int = strip.childCount

    /** Highlight terminal tab [index], or the apps tab for [APPS]. */
    fun setSelected(index: Int) {
        selectedIndex = index
        val apps = index == APPS
        setBackgroundColor(palette.bg)
        titleView.setTextColor(palette.fgSelected)
        for (b in buttons) b.imageTintList = ColorStateList.valueOf(palette.fgUnselected)
        appsTab.background = if (apps) selectedBackground() else null
        appsTab.imageTintList = ColorStateList.valueOf(if (apps) palette.fgSelected else palette.fgUnselected)
        for (i in 0 until tabCount()) styleTab(i)
        if (index in 0 until tabCount()) scrollIntoView(strip.getChildAt(index))
    }

    private fun styleTab(i: Int) {
        val tab = strip.getChildAt(i) as LinearLayout
        val selected = i == selectedIndex
        val fg = if (selected) palette.fgSelected else palette.fgUnselected
        tab.background = if (selected) selectedBackground() else null
        (tab.getChildAt(0) as TextView).setTextColor(fg)
        (tab.getChildAt(1) as ImageView).imageTintList = ColorStateList.valueOf(fg)
    }

    fun setLabel(index: Int, label: CharSequence) {
        if (index !in 0 until tabCount()) return
        val tab = strip.getChildAt(index) as LinearLayout
        (tab.getChildAt(0) as TextView).text = label
    }

    /** Faint fill with an accent strip along the top. */
    private fun selectedBackground(): LayerDrawable =
        LayerDrawable(arrayOf(ColorDrawable(palette.tabSelected), ColorDrawable(context.getColor(R.color.tawc_accent))))
            .apply {
                setLayerGravity(1, Gravity.TOP)
                setLayerHeight(1, dp(SELECTED_BORDER_DP))
            }

    private fun buildTab(label: CharSequence): View {
        val tab = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(TAB_PAD_H_DP), 0, 0, 0)
        }
        val text = TextView(context).apply {
            this.text = label
            isSingleLine = true
            ellipsize = TextUtils.TruncateAt.END
            maxWidth = dp(TAB_MAX_LABEL_DP)
            textSize = TAB_TEXT_SP
            setTextColor(palette.fgUnselected)
        }
        tab.addView(text, LayoutParams(WRAP_CONTENT, WRAP_CONTENT))
        val close = ImageButton(context).apply {
            setImageResource(R.drawable.ic_close)
            imageTintList = ColorStateList.valueOf(palette.fgUnselected)
            setBackgroundColor(Color.TRANSPARENT)
            setPadding(dp(ICON_PAD_DP), dp(ICON_PAD_DP), dp(ICON_PAD_DP), dp(ICON_PAD_DP))
            contentDescription = context.getString(R.string.terminal_close_tab)
            setOnClickListener { onTabCloseClicked(strip.indexOfChild(tab)) }
        }
        tab.addView(close, LayoutParams(dp(CLOSE_WIDTH_DP), MATCH_PARENT))
        tab.setOnClickListener { onTabSelected(strip.indexOfChild(tab)) }
        return tab
    }

    private fun scrollIntoView(tab: View) {
        // Post: a freshly added/restyled tab has no geometry until the
        // next layout pass.
        scroller.post {
            tab.requestRectangleOnScreen(Rect(0, 0, tab.width, tab.height), false)
        }
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()

    companion object {
        /** [setSelected] index of the apps tab. */
        const val APPS = -1

        private const val TITLE_PAD_DP = 8
        private const val TAB_TEXT_SP = 13f
        private const val SELECTED_BORDER_DP = 2
        private const val TAB_MAX_LABEL_DP = 180
        private const val TAB_PAD_H_DP = 12
        private const val CLOSE_WIDTH_DP = 36
        private const val NEW_TAB_WIDTH_DP = 44
        // About a short terminal tab (`Term 1 ×`), so it reads as one.
        private const val APPS_TAB_WIDTH_DP = 64
        private const val ICON_PAD_DP = 10
        private const val FADE_DP = 16
    }
}

private class Palette(val bg: Int, val tabSelected: Int, val fgSelected: Int, val fgUnselected: Int)
