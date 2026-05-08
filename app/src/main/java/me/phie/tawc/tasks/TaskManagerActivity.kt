package me.phie.tawc.tasks

import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import kotlinx.coroutines.withContext
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore
import me.phie.tawc.install.distro.DistroRegistry
import me.phie.tawc.ui.buildChildScreen
import me.phie.tawc.ui.destructiveButton
import me.phie.tawc.ui.tawcCard
import me.phie.tawc.ui.verticalLp

/**
 * Lists every guest process running across every installed rootfs (plus
 * orphan processes from rootfs slots that have since been uninstalled
 * but didn't get fully cleaned up). One Stop button per process.
 *
 * MVP scope: scan + render + kill. The screen polls every
 * [REFRESH_INTERVAL_MS] while visible — discovery is one `/proc` walk
 * plus (only when chroot installs are present) one `su`-shell pass, so
 * the cost is millisecond-scale even with hundreds of processes.
 */
class TaskManagerActivity : AppCompatActivity() {

    private val store by lazy { InstallationStore(this) }
    private lateinit var listContainer: LinearLayout
    private lateinit var emptyView: TextView
    private val pad by lazy { (16 * resources.displayMetrics.density).toInt() }
    private val cardMargin by lazy { (8 * resources.displayMetrics.density).toInt() }
    private val cardPad by lazy { (12 * resources.displayMetrics.density).toInt() }

    private var scope: CoroutineScope? = null
    private var refreshJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen("Task manager")

        val scroll = ScrollView(this).apply { isFillViewport = true }
        listContainer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }
        scroll.addView(listContainer)
        scaffold.content.addView(scroll, verticalLp(MATCH_PARENT, MATCH_PARENT))

        // Empty state shown immediately so the screen isn't blank
        // during the first scan; flipped to GONE if results arrive.
        emptyView = TextView(this).apply {
            text = "No Linux processes running."
            alpha = 0.6f
            textSize = 16f
            gravity = Gravity.CENTER
            setPadding(pad, pad * 4, pad, pad)
        }
        listContainer.addView(emptyView, verticalLp(MATCH_PARENT, WRAP_CONTENT))

        setContentView(scaffold.root)
    }

    override fun onResume() {
        super.onResume()
        val s = CoroutineScope(Dispatchers.Main)
        scope = s
        s.launch {
            while (isActive) {
                startRefresh()
                delay(REFRESH_INTERVAL_MS)
            }
        }
    }

    override fun onPause() {
        super.onPause()
        scope?.cancel()
        scope = null
        refreshJob = null
    }

    /**
     * Drops the request if a previous scan is still in flight — the
     * next tick will pick up. Avoids stacking scans if a refresh ever
     * outruns the interval.
     */
    private fun startRefresh() {
        val s = scope ?: return
        if (refreshJob?.isActive == true) return
        refreshJob = s.launch {
            val (installs, result) = withContext(Dispatchers.IO) {
                runInterruptible {
                    val i = store.list()
                    i to ProcessScanner.scan(this@TaskManagerActivity, i)
                }
            }
            render(installs, result)
        }
    }

    private fun render(installs: List<Installation>, result: ProcessScanner.ScanResult) {
        listContainer.removeAllViews()

        val byInstall = result.groupedByInstall()
        val orphans = result.orphans()

        if (byInstall.isEmpty() && orphans.isEmpty()) {
            listContainer.addView(emptyView, verticalLp(MATCH_PARENT, WRAP_CONTENT))
            return
        }

        for (inst in installs) {
            val procs = byInstall[inst.id] ?: continue
            listContainer.addView(
                buildGroupCard(installLabel(inst), procs),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
            )
        }
        // Stable order across ticks — groupBy preserves insertion order
        // but the underlying scanner doesn't guarantee orphan ordering.
        val byOrphanId = orphans.groupBy { it.orphanRootfsId ?: "?" }.toSortedMap()
        for ((id, procs) in byOrphanId) {
            listContainer.addView(
                buildGroupCard("(uninstalled: $id)", procs),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
            )
        }
    }

    private fun installLabel(inst: Installation): String {
        val resolved = DistroRegistry.forInstallation(inst)
        val displayName = resolved?.displayName
            ?: "${inst.distro.replaceFirstChar { it.titlecase() }} (${inst.arch})"
        return inst.label ?: displayName
    }

    private fun buildGroupCard(title: String, procs: List<ProcessInfo>): View {
        val card = tawcCard()
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad, cardPad, cardPad, cardPad)
        }
        column.addView(TextView(this).apply {
            text = title
            textSize = 16f
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        })
        for (p in procs) {
            column.addView(
                buildProcessRow(p),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin / 2),
            )
        }
        card.addView(column)
        return card
    }

    private fun buildProcessRow(p: ProcessInfo): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, cardPad / 2, 0, cardPad / 2)
        }
        val labelColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        labelColumn.addView(
            TextView(this@TaskManagerActivity).apply {
                text = "${p.pid}  ${p.comm}"
                textSize = 15f
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
        // Skip the second line when cmdline adds nothing over comm,
        // otherwise Stop buttons render at uneven heights across rows.
        if (p.cmdline.isNotBlank() && p.cmdline != p.comm) {
            labelColumn.addView(
                TextView(this@TaskManagerActivity).apply {
                    text = p.cmdline
                    textSize = 12f
                    alpha = 0.7f
                    maxLines = 2
                    ellipsize = android.text.TextUtils.TruncateAt.END
                },
                LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
            )
        }
        row.addView(labelColumn, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))

        val stop = destructiveButton("Stop") { stopProcess(p) }
        row.addView(
            stop,
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                marginStart = cardPad
            },
        )
        return row
    }

    /**
     * Fire SIGTERM/SIGKILL off the main thread, then immediately
     * refresh so the row disappears (or stays, if the process
     * resisted) without waiting for the next tick.
     */
    private fun stopProcess(p: ProcessInfo) {
        val s = scope ?: return
        s.launch {
            withContext(Dispatchers.IO) {
                runInterruptible { ProcessScanner.stop(p) }
            }
            startRefresh()
        }
    }

    companion object {
        private const val REFRESH_INTERVAL_MS = 2000L
    }
}
