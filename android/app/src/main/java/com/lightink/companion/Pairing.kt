package com.lightink.companion

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.util.Log

/** The pairing request SyncService took over, answered with the code typed in the app. */
object PairingCode {
    @Volatile var device: BluetoothDevice? = null

    @SuppressLint("MissingPermission")
    fun submit(code: String): Boolean {
        val d = device ?: return false
        // For LE, Android turns the PIN digits into the 6 digit passkey
        return runCatching { d.setPin(code.toByteArray(Charsets.US_ASCII)) }.getOrDefault(false)
    }
}

/**
 * Android's bond with the watch. Android refuses to pair again with a device it still
 * has a bond with ("key missing"), so the app drops it before pairing and on Forget.
 */
object Bonds {
    private const val TAG = "LightInkBonds"

    /**
     * Removes the bond with [address]. True when there was none or the removal started.
     * removeBond() isn't public API: phones that refuse it return false, and the user
     * removes the watch in Bluetooth settings instead.
     */
    @SuppressLint("MissingPermission", "DiscouragedPrivateApi")
    fun remove(context: Context, address: String?): Boolean {
        if (address == null) return true
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return false
        val device = runCatching { adapter.bondedDevices }.getOrNull()
            ?.firstOrNull { it.address == address } ?: return true
        return runCatching { device.javaClass.getMethod("removeBond").invoke(device) as Boolean }
            .onFailure { Log.w(TAG, "removeBond refused", it) }
            .getOrDefault(false)
    }

    /** Drops the bonds of the paired watch and of a pairing that failed halfway. */
    fun clearForPairing(context: Context, state: WatchState): Boolean =
        remove(context, state.watchAddress) and remove(context, state.pairingAddress)
}
