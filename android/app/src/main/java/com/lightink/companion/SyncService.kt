package com.lightink.companion

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.ParcelUuid
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import java.util.UUID

/**
 * Keeps the phone reachable for the watch: advertises the LightInk payload and serves
 * the GATT service. The watch decides when to talk (a short window while its display
 * refreshes).
 *
 * It's a foreground service, with its notification, only while something needs the
 * watch: pairing, changes waiting, or "Always reachable". Otherwise it runs without a
 * notification while the app is open, and Android stops it some time after.
 */
@SuppressLint("MissingPermission") // Started only once the permissions are granted
class SyncService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var manager: BluetoothManager
    private var server: BluetoothGattServer? = null
    private var advertising: AdvertisingSet? = null
    private var advertisingCallback: AdvertisingSetCallback? = null
    private var advertisingFast: Boolean? = null

    // Writes arrive in pieces, joined per device and characteristic
    private val assemblers = HashMap<Pair<String, UUID>, Protocol.Assembler>()
    // A long read must see the same value for every offset
    private var pendingServed: ByteArray? = null
    // Devices connected to the GATT server, to recognise the watch's pairing request.
    // Android reports every LE link of the phone here (headphones, ...), not only the watch
    private val connectedAddresses = java.util.concurrent.ConcurrentHashMap.newKeySet<String>()
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        WatchStore.init(this)
        manager = getSystemService(BluetoothManager::class.java)
        updateForeground(notice(WatchStore.state.value))
        ContextCompat.registerReceiver(
            this, bluetoothState, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        // Before the system's pairing dialog (priority 0): the app asks for the code
        val pairing = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_PAIRING_REQUEST)
            addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
            priority = 999
        }
        ContextCompat.registerReceiver(this, pairingRequests, pairing, ContextCompat.RECEIVER_EXPORTED)
        scope.launch {
            WatchStore.state
                .map { Triple(it.needsService, it.advertisementFlags() to it.token, it.fastAdvertising) }
                .distinctUntilChanged()
                .collect { (needed, _, _) -> if (needed) refresh() else stopSelf() }
        }
        scope.launch {
            WatchStore.state.map { notice(it) }.distinctUntilChanged().collect { updateForeground(it) }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        refresh()
        // Restarted by the system only while it has something to deliver
        return if (notice(WatchStore.state.value) != null) START_STICKY else START_NOT_STICKY
    }

    override fun onDestroy() {
        unregisterReceiver(bluetoothState)
        unregisterReceiver(pairingRequests)
        PairingCode.device = null
        WatchStore.setCodeRequested(false)
        stopBluetooth()
        WatchStore.setConnected(false)
        scope.cancel()
        super.onDestroy()
    }

    private val bluetoothState = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_ON -> refresh()
                BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                    stopBluetooth()
                    WatchStore.setBluetoothProblem("Bluetooth is off")
                }
            }
        }
    }

    /**
     * Android's pairing request for the watch being paired: the system dialog is stopped
     * and the app asks for the code with a number pad, then answers with setPin(). Any
     * other request, or a phone that doesn't let the broadcast be stopped, gets the
     * system dialog as before.
     */
    private val pairingRequests = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = IntentCompat.getParcelableExtra(
                intent, BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java,
            ) ?: return
            when (intent.action) {
                BluetoothDevice.ACTION_PAIRING_REQUEST -> {
                    val variant = intent.getIntExtra(BluetoothDevice.EXTRA_PAIRING_VARIANT, -1)
                    // The watch's passkey arrives as the PIN variant, which setPin() answers
                    if (!WatchStore.state.value.pairing || device.address !in connectedAddresses ||
                        variant != BluetoothDevice.PAIRING_VARIANT_PIN
                    ) return
                    if (isOrderedBroadcast) abortBroadcast()
                    // A pairing that fails halfway leaves a bond: remember whose, to drop it
                    WatchStore.setPairingAddress(device.address)
                    PairingCode.device = device
                    WatchStore.setCodeRequested(true)
                }
                BluetoothDevice.ACTION_BOND_STATE_CHANGED -> {
                    val bond = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
                    if (device.address == PairingCode.device?.address && bond != BluetoothDevice.BOND_BONDING) {
                        PairingCode.device = null
                        WatchStore.setCodeRequested(false)
                    }
                }
            }
        }
    }

    /** Brings the GATT server and the advertisement in line with the current state. */
    private fun refresh() {
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            WatchStore.setBluetoothProblem("Bluetooth is off")
            return
        }
        if (!adapter.isMultipleAdvertisementSupported) {
            WatchStore.setBluetoothProblem("This phone can't advertise over Bluetooth LE")
            return
        }
        WatchStore.setBluetoothProblem(null)
        if (server == null) startServer()
        updateAdvertising()
    }

    private fun stopBluetooth() {
        advertisingCallback?.let { manager.adapter?.bluetoothLeAdvertiser?.stopAdvertisingSet(it) }
        advertisingCallback = null
        advertising = null
        advertisingFast = null
        server?.close()
        server = null
        connectedAddresses.clear()
    }

    // --- Advertising -------------------------------------------------------------------

    private fun advertiseData(state: WatchState): AdvertiseData = AdvertiseData.Builder()
        .setIncludeDeviceName(false)
        .setIncludeTxPowerLevel(false)
        .addManufacturerData(
            Protocol.COMPANY_ID,
            Protocol.advertisement(state.advertisementFlags(), if (state.pairing) 0 else state.token),
        )
        .build()

    private fun updateAdvertising() {
        val state = WatchStore.state.value
        val advertiser = manager.adapter?.bluetoothLeAdvertiser ?: return
        if (advertisingFast == state.fastAdvertising) {
            advertising?.setAdvertisingData(advertiseData(state))
            return
        }
        // A new interval needs a new set
        advertisingCallback?.let { advertiser.stopAdvertisingSet(it) }
        advertising = null
        // Legacy advertising: the watch's controller only scans those (BLE 4.2). The watch
        // listens for ~200 ms, so the 100 ms interval is what makes it find the phone
        val params = AdvertisingSetParameters.Builder()
            .setLegacyMode(true)
            .setConnectable(true)
            .setScannable(true)
            .setInterval(
                if (state.fastAdvertising) AdvertisingSetParameters.INTERVAL_LOW
                else AdvertisingSetParameters.INTERVAL_MEDIUM
            )
            .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_MEDIUM)
            .build()
        val scanResponse = AdvertiseData.Builder()
            .addServiceUuid(ParcelUuid(Protocol.SERVICE))
            .build()
        val callback = object : AdvertisingSetCallback() {
            override fun onAdvertisingSetStarted(set: AdvertisingSet?, txPower: Int, status: Int) {
                if (status == ADVERTISE_SUCCESS) {
                    advertising = set
                    // The state may have moved on while it started
                    set?.setAdvertisingData(advertiseData(WatchStore.state.value))
                } else {
                    Log.e(TAG, "advertising failed: $status")
                    WatchStore.setBluetoothProblem("Bluetooth advertising failed ($status)")
                    advertisingFast = null
                }
            }
        }
        advertisingCallback = callback
        advertisingFast = state.fastAdvertising
        advertiser.startAdvertisingSet(params, advertiseData(state), scanResponse, null, null, callback)
    }

    // --- GATT server -------------------------------------------------------------------

    private fun startServer() {
        val server = manager.openGattServer(this, gattCallback) ?: run {
            WatchStore.setBluetoothProblem("Could not open the Bluetooth GATT server")
            return
        }
        // Only an encrypted link with the bonded watch (the pairing code was entered)
        val write = BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED_MITM
        val read = BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED_MITM
        val service = BluetoothGattService(Protocol.SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(
            BluetoothGattCharacteristic(Protocol.STATE, BluetoothGattCharacteristic.PROPERTY_WRITE, write)
        )
        service.addCharacteristic(
            BluetoothGattCharacteristic(Protocol.PENDING, BluetoothGattCharacteristic.PROPERTY_READ, read)
        )
        service.addCharacteristic(
            BluetoothGattCharacteristic(Protocol.SCHEMA, BluetoothGattCharacteristic.PROPERTY_WRITE, write)
        )
        server.addService(service)
        this.server = server
    }

    private val gattCallback = object : BluetoothGattServerCallback() {
        // Callbacks can come from several binder threads
        @Synchronized
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            val connected = newState == BluetoothProfile.STATE_CONNECTED
            if (connected) connectedAddresses += device.address else connectedAddresses -= device.address
            if (!connected) {
                assemblers.keys.removeAll { it.first == device.address }
                pendingServed = null
            }
            // "Syncing…" is about the watch, whose address is known once paired
            val watch = WatchStore.state.value.watchAddress
            if (watch == null || device.address == watch) WatchStore.setConnected(connected)
        }

        @Synchronized
        override fun onCharacteristicReadRequest(
            device: BluetoothDevice, requestId: Int, offset: Int,
            characteristic: BluetoothGattCharacteristic,
        ) {
            if (characteristic.uuid != Protocol.PENDING) {
                respond(device, requestId, BluetoothGatt.GATT_READ_NOT_PERMITTED, offset, null)
                return
            }
            val value = if (offset == 0) WatchStore.pendingPayload().also { pendingServed = it }
            else pendingServed ?: WatchStore.pendingPayload()
            if (offset > value.size) {
                respond(device, requestId, BluetoothGatt.GATT_INVALID_OFFSET, offset, null)
                return
            }
            respond(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value.copyOfRange(offset, value.size))
        }

        @Synchronized
        override fun onCharacteristicWriteRequest(
            device: BluetoothDevice, requestId: Int, characteristic: BluetoothGattCharacteristic,
            preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?,
        ) {
            val data = value ?: ByteArray(0)
            val status = if (preparedWrite) {
                BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED // The watch sends pieces instead
            } else {
                val assembler = assemblers.getOrPut(device.address to characteristic.uuid) {
                    Protocol.Assembler()
                }
                val whole = runCatching { assembler.add(data) }
                when {
                    whole.isFailure -> BluetoothGatt.GATT_INVALID_OFFSET
                    whole.getOrNull() == null -> BluetoothGatt.GATT_SUCCESS // More to come
                    else -> handleWrite(device, characteristic.uuid, whole.getOrThrow()!!)
                }
            }
            if (responseNeeded) respond(device, requestId, status, offset, data)
        }

        @Synchronized
        override fun onExecuteWrite(device: BluetoothDevice, requestId: Int, execute: Boolean) {
            respond(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
        }
    }

    private fun handleWrite(device: BluetoothDevice, uuid: UUID, data: ByteArray): Int = when (uuid) {
        Protocol.SCHEMA ->
            if (WatchStore.onSchema(data)) BluetoothGatt.GATT_SUCCESS else BluetoothGatt.GATT_FAILURE
        Protocol.STATE ->
            if (WatchStore.onState(data, device.address)) BluetoothGatt.GATT_SUCCESS
            else BluetoothGatt.GATT_INSUFFICIENT_AUTHORIZATION
        else -> BluetoothGatt.GATT_WRITE_NOT_PERMITTED
    }

    private fun respond(device: BluetoothDevice, requestId: Int, status: Int, offset: Int, value: ByteArray?) {
        server?.sendResponse(device, requestId, status, offset, value)
    }

    // --- Foreground notification -------------------------------------------------------

    /** Title and text of the notification, or null when nothing needs it. */
    private fun notice(s: WatchState): Pair<String, String>? = when {
        s.pairing && s.codeRequested -> "Pairing with the watch" to "Type the code shown on the watch"
        s.pairing -> "Pairing with the watch" to "Waiting for the watch"
        s.pending.isNotEmpty() ->
            "${s.pending.size} change(s) waiting for the watch" to "They apply at its next sync"
        s.alwaysReachable && s.paired -> "LightInk" to "The watch can sync any time"
        else -> null
    }

    private fun updateForeground(notice: Pair<String, String>?) {
        if (notice == null) {
            if (foreground) ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            foreground = false
            return
        }
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Watch link", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Shown while changes wait for the LightInk watch"
                setShowBadge(false)
            }
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val notification: Notification = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_watch)
            .setContentTitle(notice.first)
            .setContentText(notice.second)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
        ServiceCompat.startForeground(
            this, NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE,
        )
        foreground = true
    }

    companion object {
        private const val TAG = "LightInkSync"
        private const val CHANNEL = "link"
        private const val NOTIFICATION_ID = 1

        /** Foreground start (from the background, e.g. boot) only when it will notify. */
        fun start(context: Context, foreground: Boolean = false) {
            val intent = Intent(context, SyncService::class.java)
            if (foreground) ContextCompat.startForegroundService(context, intent)
            else context.startService(intent)
        }
    }
}
