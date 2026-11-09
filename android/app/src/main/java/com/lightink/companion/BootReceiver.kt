package com.lightink.companion

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** After a reboot or an app update: be reachable again if changes wait or always reachable is on. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) return
        WatchStore.init(context)
        // From the background only a foreground start is allowed: only when it will notify
        val s = WatchStore.state.value
        if (s.paired && (s.pending.isNotEmpty() || s.alwaysReachable) && Permissions.granted(context)) {
            SyncService.start(context, foreground = true)
        }
    }
}
