package me.phie.tawc.ops

/**
 * Coarse stages used by the generic ops layer. Concrete operations
 * (install, uninstall, …) carry their own finer-grained stage enum
 * for internal use; at the [Operation] surface they project down to
 * one of these four values.
 */
enum class OperationStage {
    /** Just-registered, no progress reported yet. */
    IDLE,

    /** Work in flight. Progress bar visible; Cancel button visible. */
    RUNNING,

    /** Successful terminal. Progress bar hidden; status painted success-green. */
    DONE,

    /** Failed or cancelled terminal. Progress bar hidden; status painted danger-red. */
    FAILED;

    val isTerminal: Boolean get() = this == DONE || this == FAILED
}

/**
 * Snapshot for [Operation.progress]. [percent] is `null` when the
 * operation can't report a meaningful 0..100; the panel renders an
 * indeterminate spinner in that case.
 */
data class OperationProgress(
    val stage: OperationStage,
    val message: String,
    val percent: Int? = null,
)
