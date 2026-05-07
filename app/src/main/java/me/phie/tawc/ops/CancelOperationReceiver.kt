package me.phie.tawc.ops

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Receives the "Cancel" PendingIntent from per-op notifications. Looks
 * up the named operation in [OperationsRegistry] and calls [Operation.cancel].
 *
 * Always bypasses [Operation.cancelConfirmation] — a user who tapped a
 * notification action has already deliberately decided. The in-app
 * cancel path through [LogScreenActivity] is the one that surfaces the
 * confirm dialog.
 *
 * `exported="false"` in the manifest. Only the system + our own process
 * can deliver to it (PendingIntent's bound component selector handles
 * the routing).
 */
class CancelOperationReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val opId = intent.getStringExtra(EXTRA_OPERATION_ID)
        if (opId.isNullOrEmpty()) {
            Log.w(TAG, "CancelOperationReceiver: no operationId in intent")
            return
        }
        val op = OperationsRegistry.get(opId)
        if (op == null) {
            Log.w(TAG, "CancelOperationReceiver: op '$opId' not in registry (already terminated?)")
            return
        }
        Log.i(TAG, "CancelOperationReceiver: cancelling '$opId' from notification")
        op.cancel()
    }

    companion object {
        private const val TAG = "tawc-ops"
        const val ACTION = "me.phie.tawc.ops.CANCEL_OPERATION"
        const val EXTRA_OPERATION_ID = "operationId"
    }
}
