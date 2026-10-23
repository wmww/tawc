package me.phie.tawc.compositor

import me.phie.tawc.ExternalAccessMode
import me.phie.tawc.Settings

/**
 * Access policy for [ExternalWaylandService], split out from the service
 * so it can be unit-tested without Binder or a device.
 *
 * The caller is identified by uid, then resolved to package names. A uid
 * can own several packages (shared uid), so a match on any of them is a
 * match. The service's own uid is always allowed.
 */
object ExternalWaylandAccess {

    /** Result of a policy decision, with enough detail to log a denial. */
    data class Decision(val allowed: Boolean, val reason: String)

    /**
     * @param callerUid uid from Binder (bind path) or `SO_PEERCRED`
     *   (startService path).
     * @param callerPackages packages owning [callerUid]; empty when
     *   unknown or hidden by package visibility.
     */
    fun decide(
        callerUid: Int,
        callerPackages: Collection<String>,
        ownUid: Int,
        mode: ExternalAccessMode,
        allowedPackages: Set<String>,
    ): Decision {
        if (callerUid == ownUid) return Decision(true, "own uid")

        return when (mode) {
            ExternalAccessMode.DISABLED ->
                Decision(false, "external access is disabled")

            ExternalAccessMode.ANY ->
                if (callerPackages.isEmpty()) {
                    // Every real caller has at least one package; an empty
                    // list means the uid could not be resolved (uninstalled
                    // mid-call, or hidden by package visibility). Refusing
                    // keeps "any app" from also meaning "any unresolvable
                    // uid".
                    Decision(false, "uid $callerUid has no visible package")
                } else {
                    Decision(true, "any app")
                }

            ExternalAccessMode.ALLOWLIST ->
                if (callerPackages.isEmpty()) {
                    Decision(false, "uid $callerUid has no visible package")
                } else {
                    val hit = callerPackages.firstOrNull { it in allowedPackages }
                    if (hit != null) {
                        Decision(true, "allowed package $hit")
                    } else {
                        Decision(
                            false,
                            "uid $callerUid (${callerPackages.joinToString()}) is not in the allowlist",
                        )
                    }
                }
        }
    }

    /** [decide] against the live [Settings] values. */
    fun decide(
        callerUid: Int,
        callerPackages: Collection<String>,
        ownUid: Int,
    ): Decision = decide(
        callerUid = callerUid,
        callerPackages = callerPackages,
        ownUid = ownUid,
        mode = Settings.externalAccessMode,
        allowedPackages = Settings.externalAllowedPackages,
    )

    /**
     * Packages to show in the allowlist picker. Launchable apps only, so
     * the list stays meaningful, plus anything already allowed even if it
     * has no launcher activity (a service-only helper package).
     */
    fun allowlistCandidates(
        launchable: Collection<String>,
        alreadyAllowed: Collection<String>,
    ): List<String> = (launchable + alreadyAllowed).distinct().sorted()
}
