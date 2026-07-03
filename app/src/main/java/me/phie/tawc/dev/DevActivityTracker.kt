package me.phie.tawc.dev

import android.app.Activity
import android.app.Application
import android.os.Bundle

/**
 * Debug-only registry of live activities, registered from
 * [me.phie.tawc.TawcApplication.onCreate] next to [ExecBroker.start].
 * Lets broker actions reset UI state between integration tests —
 * currently `test-init` finishing [me.phie.tawc.ops.LogScreenActivity]
 * instances that broker install/uninstall/run actions left on top of
 * the task.
 */
internal object DevActivityTracker : Application.ActivityLifecycleCallbacks {

    private val live = mutableListOf<Activity>()

    fun liveActivities(): List<Activity> = synchronized(live) { live.toList() }

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        synchronized(live) { live.add(activity) }
    }

    override fun onActivityDestroyed(activity: Activity) {
        synchronized(live) { live.remove(activity) }
    }

    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityResumed(activity: Activity) {}
    override fun onActivityPaused(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
}
