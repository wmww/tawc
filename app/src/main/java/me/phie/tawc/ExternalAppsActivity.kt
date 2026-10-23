package me.phie.tawc

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.compositor.ExternalWaylandAccess
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.plainIconButton
import me.phie.tawc.ui.searchPill

/**
 * Picker for the external-Wayland allowlist
 * ([Settings.externalAllowedPackages]).
 *
 * Lists launchable apps plus anything already allowed, and writes each
 * toggle straight through to [Settings], so the service's next decision
 * sees it with no save step. Reachable from Settings → External apps →
 * "Manage allowed apps"; the mode radio lives in Settings.
 */
class ExternalAppsActivity : AppCompatActivity() {

    private class App(
        val packageName: String,
        val label: String,
        val icon: android.graphics.drawable.Drawable?,
    )

    private lateinit var searchField: EditText
    private lateinit var statusView: TextView
    private val adapter = AppAdapter()

    private var all: List<App> = emptyList()
    private var loaded = false

    private val density by lazy { resources.displayMetrics.density }
    private val iconSizePx by lazy { (40 * density).toInt() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen(getString(R.string.title_external_apps))
        val pad = (16 * density).toInt()

        scaffold.content.addView(
            TextView(this).apply {
                text = getString(R.string.external_apps_intro)
                textSize = 14f
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.bottomMargin = pad },
        )

        val clearButton = plainIconButton(R.drawable.ic_close, getString(R.string.action_clear_search)) {
            searchField.text.clear()
        }.apply { visibility = View.INVISIBLE }
        searchField = EditText(this).apply {
            hint = getString(R.string.external_apps_search_hint)
            textSize = 16f
            isSingleLine = true
            background = null
            doAfterTextChanged {
                clearButton.visibility = if (it.isNullOrEmpty()) View.INVISIBLE else View.VISIBLE
                applyFilter()
            }
        }
        scaffold.content.addView(
            searchPill(searchField, clearButton),
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also {
                it.setMargins(0, 0, 0, pad / 2)
            },
        )

        statusView = TextView(this).apply {
            textSize = 14f
            alpha = 0.7f
        }
        scaffold.content.addView(
            statusView,
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).also { it.bottomMargin = pad / 2 },
        )

        val list = RecyclerView(this).apply {
            layoutManager = LinearLayoutManager(this@ExternalAppsActivity)
            adapter = this@ExternalAppsActivity.adapter
            clipToPadding = false
        }
        scaffold.content.addView(list, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        setContentView(scaffold.root)

        lifecycleScope.launch {
            all = withContext(Dispatchers.IO) { loadApps() }
            loaded = true
            applyFilter()
        }
    }

    /**
     * Launchable packages plus anything already allowlisted (a
     * service-only helper has no launcher entry but must stay
     * manageable). Labels come from the package manager; an unresolvable
     * package falls back to its id.
     */
    private fun loadApps(): List<App> {
        val pm = packageManager
        val launchable = pm.queryIntentActivities(
            Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER),
            0,
        ).mapNotNull { it.activityInfo?.packageName }

        val names = ExternalWaylandAccess.allowlistCandidates(
            launchable = launchable,
            alreadyAllowed = Settings.externalAllowedPackages,
        )

        return names.map { pkg ->
            val label = runCatching {
                val info = pm.getApplicationInfo(pkg, 0)
                pm.getApplicationLabel(info).toString()
            }.getOrDefault(pkg)
            val icon = runCatching { pm.getApplicationIcon(pkg) }.getOrNull()
            App(pkg, label, icon)
        }.sortedBy { it.label.lowercase() }
    }

    private fun applyFilter() {
        val query = searchField.text.toString().trim()
        val shown = if (query.isEmpty()) {
            all
        } else {
            all.filter {
                it.label.contains(query, ignoreCase = true) ||
                    it.packageName.contains(query, ignoreCase = true)
            }
        }
        adapter.submit(shown)
        if (!loaded) return
        statusView.visibility = if (shown.isEmpty()) View.VISIBLE else View.GONE
        statusView.text = getString(
            if (all.isEmpty()) R.string.external_apps_no_launcher else R.string.external_apps_empty
        )
    }

    private fun toggle(packageName: String, allowed: Boolean) {
        val next = Settings.externalAllowedPackages.toMutableSet()
        if (allowed) next.add(packageName) else next.remove(packageName)
        Settings.externalAllowedPackages = next
    }

    private inner class AppAdapter : RecyclerView.Adapter<AppAdapter.Row>() {

        private var apps: List<App> = emptyList()

        inner class Row(val root: LinearLayout, val check: CheckBox, val icon: ImageView) :
            RecyclerView.ViewHolder(root)

        fun submit(apps: List<App>) {
            this.apps = apps
            notifyDataSetChanged()
        }

        override fun getItemCount() = apps.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Row {
            val rowPad = (12 * density).toInt()
            val root = LinearLayout(this@ExternalAppsActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                setPadding(rowPad, rowPad / 2, rowPad, rowPad / 2)
                isClickable = true
            }
            val icon = ImageView(this@ExternalAppsActivity).apply {
                scaleType = ImageView.ScaleType.FIT_CENTER
            }
            root.addView(
                icon,
                LinearLayout.LayoutParams(iconSizePx, iconSizePx).also { it.marginEnd = rowPad },
            )
            val check = CheckBox(this@ExternalAppsActivity).apply {
                textSize = 16f
                setPadding(0, 0, 0, 0)
                // The row handles the click so a tap anywhere toggles once;
                // a clickable box would toggle itself and skip the write.
                isClickable = false
                isFocusable = false
            }
            root.addView(check, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            return Row(root, check, icon)
        }

        override fun onBindViewHolder(holder: Row, position: Int) {
            val app = apps[position]
            holder.check.text = app.label
            holder.check.isChecked = app.packageName in Settings.externalAllowedPackages
            // Tapping anywhere on the row toggles, not just the box.
            holder.root.setOnClickListener {
                val next = !holder.check.isChecked
                holder.check.isChecked = next
                toggle(app.packageName, next)
            }
            if (app.icon != null) {
                holder.icon.setImageDrawable(app.icon)
            } else {
                holder.icon.setImageResource(R.drawable.ic_app_fallback)
            }
        }
    }
}
