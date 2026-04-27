package me.phie.tawc.install

/**
 * Coarse stages of the install pipeline. Used to drive a progress bar /
 * status label in the UI and to label log lines in broadcast output.
 */
enum class InstallStage {
    IDLE,
    DOWNLOADING,
    EXTRACTING,
    CONFIGURING,
    /** Distro-agnostic name for "init the package manager / its keyring". */
    PKG_KEYRING,
    /** Distro-agnostic name for "install the base package set". */
    PKG_INSTALL,
    UNMOUNTING,
    DELETING,
    DONE,
    FAILED;
}

/**
 * Snapshot of an in-progress operation. [percent] is `null` when progress is
 * indeterminate. [message] is a short human-readable description for the
 * status line; long log output is delivered separately via the log callback.
 */
data class InstallProgress(
    val stage: InstallStage,
    val message: String,
    val percent: Int? = null,
    val errorMessage: String? = null,
)
