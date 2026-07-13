package me.phie.tawc.launcher

import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.phie.tawc.R
import me.phie.tawc.install.Installation
import me.phie.tawc.install.InstallationStore

/**
 * Invisible trampoline behind every pinned home-screen shortcut
 * ([EntryShortcuts]). The shortcut intent carries only
 * (installId, desktopId, label); the entry is re-resolved with a fresh
 * rootfs scan at tap time — the same walk the launcher does on open —
 * so a pin keeps working across `.desktop` edits and never stores a
 * command.
 *
 * Stale pins (distro uninstalled or mid-(un)install, entry gone) turn
 * into a [LaunchErrorActivity] dialog instead of a crash. A hidden
 * entry still launches: hiding declutters the in-app list, and an
 * existing pin is explicit user intent. Dispatch goes through
 * [EntryLauncher], so terminal entries behave exactly like an in-app
 * launch.
 */
class ShortcutLaunchActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val installId = intent?.getStringExtra(EXTRA_INSTALL_ID) ?: ""
        val desktopId = intent?.getStringExtra(EXTRA_DESKTOP_ID) ?: ""
        val label = intent?.getStringExtra(EXTRA_LABEL).takeUnless { it.isNullOrEmpty() } ?: desktopId
        val store = InstallationStore(this)
        // Pinned-shortcut extras are the app's least-trusted input
        // (the system replays them across updates); reject a malformed
        // id before it reaches File(baseDir, id) path construction.
        val inst = if (Installation.isValidId(installId)) store.load(installId) else null
        if (inst == null) {
            fail(label, getString(R.string.launcher_installation_not_found, installId))
            return
        }
        if (inst.state != Installation.State.READY) {
            fail(label, getString(R.string.shortcut_install_not_ready, inst.state.name.lowercase()))
            return
        }
        val rootfs = store.rootfsDir(inst.id).absolutePath
        lifecycleScope.launch {
            val entry = withContext(Dispatchers.IO) {
                LauncherEntry.scan(rootfs).firstOrNull { it.id == desktopId }
            }
            if (entry == null) {
                fail(label, getString(R.string.shortcut_entry_gone))
            } else {
                EntryLauncher.launch(applicationContext, inst, entry)
                finish()
            }
        }
    }

    /** Show the error dialog while this (translucent) trampoline still
     *  holds the visible window, then drop it. */
    private fun fail(label: String, message: String) {
        LaunchErrorActivity.start(
            this,
            getString(R.string.launcher_launch_failed_title, label),
            message,
        )
        finish()
    }

    companion object {
        const val EXTRA_INSTALL_ID = "installId"
        const val EXTRA_DESKTOP_ID = "desktopId"
        const val EXTRA_LABEL = "label"
    }
}
