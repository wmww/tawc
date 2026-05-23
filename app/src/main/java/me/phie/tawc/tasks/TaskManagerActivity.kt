package me.phie.tawc.tasks

import android.app.Dialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.os.Bundle
import android.text.TextUtils
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.graphics.drawable.toDrawable
import com.google.android.material.button.MaterialButton
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runInterruptible
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.install.ChrootMethod
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
    private val treeIndent by lazy { (18 * resources.displayMetrics.density).toInt() }
    private val toggleSlotWidth by lazy { (32 * resources.displayMetrics.density).toInt() }
    private val toggleIconSize by lazy { (28 * resources.displayMetrics.density).toInt() }
    private val stopSlotWidth by lazy { (56 * resources.displayMetrics.density).toInt() }
    private val spinnerSize by lazy { (28 * resources.displayMetrics.density).toInt() }

    private var scope: CoroutineScope? = null
    private var refreshJob: Job? = null
    private val stoppingPids = mutableSetOf<Int>()
    private val stoppingInstallIds = mutableSetOf<String>()
    private val collapsedPids = mutableSetOf<Int>()
    private val openDetailButtons = mutableMapOf<Int, View>()
    private var lastInstalls: List<Installation> = emptyList()
    private var lastResult = ProcessScanner.ScanResult(emptyList())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scaffold = buildChildScreen(getString(R.string.title_task_manager))

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
            text = getString(R.string.task_manager_empty)
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
        stoppingPids.clear()
        stoppingInstallIds.clear()
        openDetailButtons.clear()
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
        lastInstalls = installs
        lastResult = result
        updateOpenDetailButtons()
        collapsedPids.retainAll(result.processes.mapTo(HashSet()) { it.pid })
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
                buildGroupCard(installLabel(inst), procs, inst),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, bottomMargin = cardMargin),
            )
        }
        // Stable order across ticks — groupBy preserves insertion order
        // but the underlying scanner doesn't guarantee orphan ordering.
        val byOrphanId = orphans.groupBy { it.orphanRootfsId ?: "?" }.toSortedMap()
        for ((id, procs) in byOrphanId) {
            listContainer.addView(
                buildGroupCard(getString(R.string.task_manager_uninstalled_group, id), procs),
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

    private fun buildGroupCard(
        title: String,
        procs: List<ProcessInfo>,
        install: Installation? = null,
    ): View {
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
        for (row in processTreeRows(procs)) {
            column.addView(
                buildProcessRow(row),
                verticalLp(MATCH_PARENT, WRAP_CONTENT),
            )
        }
        if (install != null) {
            column.addView(
                buildStopAllControl(install),
                LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                    gravity = Gravity.END
                    topMargin = cardPad
                },
            )
        }
        card.addView(column)
        return card
    }

    private fun buildProcessRow(treeRow: ProcessTreeRow): View {
        val p = treeRow.process
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, cardPad / 4, 0, cardPad / 4)
            isClickable = true
            isFocusable = true
            val attrs = intArrayOf(android.R.attr.selectableItemBackground)
            val ta = obtainStyledAttributes(attrs)
            try {
                background = ta.getDrawable(0)
            } finally {
                ta.recycle()
            }
            setOnClickListener { showProcessDetails(p) }
        }

        val toggle = ImageButton(this).apply {
            if (treeRow.hasChildren) {
                setImageResource(
                    if (treeRow.isCollapsed) me.phie.tawc.R.drawable.ic_tree_collapsed
                    else me.phie.tawc.R.drawable.ic_tree_expanded,
                )
                imageTintList = ColorStateList.valueOf(defaultTextColor())
                contentDescription = if (treeRow.isCollapsed) {
                    getString(R.string.action_expand)
                } else {
                    getString(R.string.action_collapse)
                }
            } else {
                imageTintList = null
                contentDescription = null
            }
            background = null
            scaleType = android.widget.ImageView.ScaleType.CENTER
            setPadding(0, 0, 0, 0)
            minimumWidth = 0
            minimumHeight = 0
            alpha = if (treeRow.hasChildren) 0.75f else 0f
            isClickable = treeRow.hasChildren
            isFocusable = treeRow.hasChildren
            if (treeRow.hasChildren) {
                setOnClickListener {
                    if (!collapsedPids.add(p.pid)) collapsedPids.remove(p.pid)
                    render(lastInstalls, lastResult)
                }
            }
        }
        row.addView(
            toggle,
            LinearLayout.LayoutParams(toggleSlotWidth, toggleIconSize).apply {
                marginStart = treeIndent * treeRow.depth
                marginEnd = cardPad / 2
            },
        )

        val labelColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        labelColumn.addView(
            TextView(this@TaskManagerActivity).apply {
                text = p.displayCommand.ifBlank { getString(R.string.task_manager_unknown_command) }
                textSize = 16.5f
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
            },
            LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT),
        )
        row.addView(labelColumn, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))

        val stop = buildStopControl(p)
        row.addView(
            stop,
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                marginStart = cardPad
            },
        )
        return row
    }

    private fun defaultTextColor(): Int =
        TextView(this).currentTextColor

    private fun showProcessDetails(p: ProcessInfo) {
        val details = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(cardPad * 2, cardPad * 2, cardPad * 2, cardPad)
        }
        details.addView(TextView(this).apply {
            text = p.displayCommand.ifBlank { getString(R.string.task_manager_process_title, p.pid) }
            textSize = 28f
        }, verticalLp(MATCH_PARENT, WRAP_CONTENT, cardPad))
        for ((label, value) in detailRows(p)) {
            details.addView(
                buildDetailRow(label, value),
                verticalLp(MATCH_PARENT, WRAP_CONTENT, cardPad / 2),
            )
        }

        val dialog = Dialog(this).apply {
            setCanceledOnTouchOutside(true)
        }
        val stop = dialogStopButton(p).apply {
            setOnClickListener {
                dialog.dismiss()
                stopProcess(p)
            }
        }
        openDetailButtons[p.pid] = stop
        details.addView(
            stop,
            LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT).apply {
                gravity = Gravity.END
                topMargin = cardPad
            },
        )

        val card = tawcCard().apply {
            radius = 28f * resources.displayMetrics.density
            addView(details)
        }
        dialog.setContentView(card)
        dialog.setOnDismissListener {
            if (openDetailButtons[p.pid] === stop) {
                openDetailButtons.remove(p.pid)
            }
        }
        dialog.window?.setBackgroundDrawable(Color.TRANSPARENT.toDrawable())
        dialog.show()
        dialog.window?.setLayout(
            resources.displayMetrics.widthPixels - (64 * resources.displayMetrics.density).toInt(),
            WRAP_CONTENT,
        )
    }

    private fun dialogStopButton(p: ProcessInfo): View {
        val label = if (isProcessStopping(p)) getString(R.string.action_stopping) else getString(R.string.action_stop)
        return destructiveButton(label) {}.apply {
            minWidth = stopSlotWidth
            setDetailStopButtonState(this, p.pid)
        }
    }

    private fun canStop(pid: Int): Boolean {
        val proc = lastResult.processes.firstOrNull { it.pid == pid } ?: return false
        return !isProcessStopping(proc)
    }

    private fun isProcessStopping(p: ProcessInfo): Boolean =
        p.pid in stoppingPids ||
            (p.ownerInstallId != null && p.ownerInstallId in stoppingInstallIds)

    private fun updateOpenDetailButtons() {
        for ((pid, button) in openDetailButtons) {
            if (button is MaterialButton) {
                setDetailStopButtonState(button, pid)
            } else {
                button.isEnabled = canStop(pid)
            }
        }
    }

    private fun setDetailStopButtonState(button: MaterialButton, pid: Int) {
        val enabled = canStop(pid)
        button.isEnabled = enabled
        button.backgroundTintList = ColorStateList.valueOf(
            getColor(
                if (enabled) me.phie.tawc.R.color.tawc_danger
                else me.phie.tawc.R.color.tawc_tonal_bg,
            ),
        )
        button.setTextColor(
            getColor(
                if (enabled) me.phie.tawc.R.color.tawc_on_danger
                else me.phie.tawc.R.color.tawc_on_tonal,
            ),
        )
    }

    private fun buildDetailRow(label: String, value: String): View {
        return LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(this@TaskManagerActivity).apply {
                text = label
                textSize = 12f
                alpha = 0.65f
                setTypeface(typeface, android.graphics.Typeface.BOLD)
            })
            addView(TextView(this@TaskManagerActivity).apply {
                text = value.ifBlank { getString(R.string.task_manager_unknown) }
                textSize = 14f
                setTextIsSelectable(true)
            })
        }
    }

    private fun detailRows(p: ProcessInfo): List<Pair<String, String>> {
        val owner = when {
            p.ownerInstallId != null -> installNameForId(p.ownerInstallId)
            p.orphanRootfsId != null -> getString(R.string.task_manager_uninstalled_rootfs, p.orphanRootfsId)
            else -> getString(R.string.task_manager_unknown_rootfs)
        }
        return listOf(
            getString(R.string.task_manager_detail_distro) to owner,
            getString(R.string.task_manager_detail_pid) to p.pid.toString(),
            getString(R.string.task_manager_detail_guest_command) to p.guestCommand,
            getString(R.string.task_manager_detail_cwd) to p.cwd,
            getString(R.string.task_manager_detail_android_command_line) to p.cmdline,
        )
    }

    private fun installNameForId(id: String): String {
        val inst = lastInstalls.firstOrNull { it.id == id }
            ?: return id
        return "${installLabel(inst)} ($id)"
    }

    private fun buildStopControl(p: ProcessInfo): View {
        if (!isProcessStopping(p)) {
            return destructiveButton(getString(R.string.action_stop)) { stopProcess(p) }.apply {
                minWidth = stopSlotWidth
                minimumWidth = stopSlotWidth
                minHeight = 0
                minimumHeight = 0
                insetTop = 0
                insetBottom = 0
                setPadding(cardPad / 2, cardPad / 3, cardPad / 2, cardPad / 3)
            }
        }
        return FrameLayout(this).apply {
            minimumWidth = stopSlotWidth
            addView(
                ProgressBar(
                    this@TaskManagerActivity,
                    null,
                    android.R.attr.progressBarStyleSmall,
                ).apply {
                    isIndeterminate = true
                },
                FrameLayout.LayoutParams(spinnerSize, spinnerSize, Gravity.CENTER),
            )
        }
    }

    private fun buildStopAllControl(inst: Installation): View {
        val active = inst.id in stoppingInstallIds
        return destructiveButton(if (active) getString(R.string.action_stopping) else getString(R.string.action_stop_all)) {
            stopAllInInstall(inst)
        }.apply {
            isEnabled = !active
            minHeight = 0
            minimumHeight = 0
            insetTop = 0
            insetBottom = 0
            setPadding(cardPad, cardPad / 3, cardPad, cardPad / 3)
            if (active) {
                backgroundTintList = ColorStateList.valueOf(getColor(me.phie.tawc.R.color.tawc_tonal_bg))
                setTextColor(getColor(me.phie.tawc.R.color.tawc_on_tonal))
            }
        }
    }

    private fun processTreeRows(procs: List<ProcessInfo>): List<ProcessTreeRow> {
        if (procs.isEmpty()) return emptyList()
        val byPid = procs.associateBy { it.pid }
        val childrenByParent = procs
            .filter { it.parentPid in byPid && it.parentPid != it.pid }
            .groupBy { it.parentPid }
            .mapValues { (_, children) -> children.sortedBy { it.pid } }
        val roots = procs
            .filter { it.parentPid !in byPid || it.parentPid == it.pid }
            .sortedBy { it.pid }
        val rows = ArrayList<ProcessTreeRow>(procs.size)
        val visited = HashSet<Int>(procs.size)

        fun append(process: ProcessInfo, depth: Int) {
            if (!visited.add(process.pid)) return
            val children = childrenByParent[process.pid].orEmpty()
            val isCollapsed = process.pid in collapsedPids
            rows += ProcessTreeRow(process, depth, children.isNotEmpty(), isCollapsed)
            if (!isCollapsed) {
                for (child in children) append(child, depth + 1)
            }
        }

        fun hiddenByCollapsedAncestor(process: ProcessInfo): Boolean {
            val seen = HashSet<Int>()
            var parentPid = process.parentPid
            while (seen.add(parentPid)) {
                if (parentPid in collapsedPids) return true
                val parent = byPid[parentPid] ?: return false
                if (parent.parentPid == parent.pid) return false
                parentPid = parent.parentPid
            }
            return false
        }

        for (root in roots) append(root, 0)
        for (p in procs.sortedBy { it.pid }) {
            if (!hiddenByCollapsedAncestor(p)) append(p, 0)
        }
        return rows
    }

    /**
     * Fire SIGTERM/SIGKILL off the main thread, then immediately
     * refresh so the row disappears (or stays, if the process
     * resisted) without waiting for the next tick.
     */
    private fun stopProcess(p: ProcessInfo) {
        val s = scope ?: return
        if (isProcessStopping(p)) return
        if (!stoppingPids.add(p.pid)) return
        render(lastInstalls, lastResult)
        s.launch {
            try {
                withContext(Dispatchers.IO) {
                    runInterruptible { ProcessScanner.stop(p) }
                }
            } finally {
                stoppingPids.remove(p.pid)
            }
            startRefresh()
        }
    }

    /**
     * Use the uninstall-time repeated scan/sweep path so a distro-wide
     * stop catches fork races and helpers that appear after the first
     * process list was rendered.
     */
    private fun stopAllInInstall(inst: Installation) {
        val s = scope ?: return
        if (!stoppingInstallIds.add(inst.id)) return
        render(lastInstalls, lastResult)
        s.launch {
            try {
                withContext(Dispatchers.IO) {
                    runInterruptible {
                        ProcessScanner.killAllInRootfs(
                            rootfsPath = store.rootfsDir(inst.id).absolutePath,
                            installId = inst.id,
                            includeChroot = inst.method == ChrootMethod.KEY,
                            log = {},
                        )
                    }
                }
            } catch (t: Throwable) {
                if (t is kotlinx.coroutines.CancellationException) throw t
                Log.w(TAG, "stop-all failed for ${inst.id}", t)
            } finally {
                stoppingInstallIds.remove(inst.id)
            }
            startRefresh()
        }
    }

    companion object {
        private const val TAG = "tawc"
        private const val REFRESH_INTERVAL_MS = 2000L
    }

    private data class ProcessTreeRow(
        val process: ProcessInfo,
        val depth: Int,
        val hasChildren: Boolean,
        val isCollapsed: Boolean,
    )
}
