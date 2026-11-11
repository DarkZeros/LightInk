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
    private const val WATCH_NAME = "LightInk" // The watch's GAP device name

    /**
     * Removes the bond with [address]. True when there was none or the removal started.
     * removeBond() isn't public API: phones that refuse it return false, and the user
     * removes the watch in Bluetooth settings instead.
     *
     * With [watchOnly] the bond is left alone unless the device calls itself a LightInk:
     * for an address the watch didn't confirm with its token, which must never take
     * another device's bond (headphones, ...) with it.
     */
    @SuppressLint("MissingPermission", "DiscouragedPrivateApi")
    fun remove(context: Context, address: String?, watchOnly: Boolean = false): Boolean {
        if (address == null) return true
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return false
        val device = runCatching { adapter.bondedDevices }.getOrNull()
            ?.firstOrNull { it.address == address } ?: return true
        if (watchOnly && runCatching { device.name }.getOrNull() != WATCH_NAME) return true
        return runCatching { device.javaClass.getMethod("removeBond").invoke(device) as Boolean }
            .onFailure { Log.w(TAG, "removeBond refused", it) }
            .getOrDefault(false)
    }

    /** Whether Android still holds a bond with [address]. True when it can't be told. */
    @SuppressLint("MissingPermission")
    fun has(context: Context, address: String?): Boolean {
        if (address == null) return true
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return true
        if (!adapter.isEnabled) return true
        val bonded = runCatching { adapter.bondedDevices }.getOrNull() ?: return true
        return bonded.any { it.address == address }
    }

    /** Drops the bonds of the paired watch and of a pairing that failed halfway. */
    fun clearForPairing(context: Context, state: WatchState): Boolean =
        remove(context, state.watchAddress) and remove(context, state.pairingAddress, watchOnly = true)
}
