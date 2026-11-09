package com.lightink.companion

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** The watch syncs on its own schedule: be reachable again after a reboot or an app update. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED &&
            intent.action != Intent.ACTION_MY_PACKAGE_REPLACED
        ) return
        WatchStore.init(context)
        if (WatchStore.state.value.paired && Permissions.granted(context)) SyncService.start(context)
    }
}
