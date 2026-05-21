package com.lightink.companion.ble

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import android.os.Build
import android.util.Log
import androidx.core.app.ActivityCompat
import com.google.gson.Gson
import com.google.gson.JsonObject
import java.util.UUID

class BleManager(private val context: Context) {
    private val TAG = "BleManager"
    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter = bluetoothManager.adapter
    private val scanner = bluetoothAdapter?.bluetoothLeScanner
    private var gatt: BluetoothGatt? = null
    private var configCharacteristic: BluetoothGattCharacteristic? = null
    private val gson = Gson()

    // Custom service and characteristic UUIDs (must match ESP32)
    private val LIGHTINK_SERVICE_UUID = UUID.fromString("4c494748-5449-4e4b-0000-000000000000")
    private val CONFIG_CHAR_UUID = UUID.fromString("4c494748-5449-4e4b-0000-000000000001")

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            super.onScanResult(callbackType, result)
            val device = result.device
            val deviceName = device.name ?: "Unknown"

            Log.d(TAG, "Found device: $deviceName (${device.address})")

            // Look for "LightInk" device
            if (deviceName.contains("LightInk", ignoreCase = true)) {
                Log.i(TAG, "Found LightInk device: $deviceName")
                stopScanning()
                connectToDevice(device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            super.onScanFailed(errorCode)
            Log.e(TAG, "Scan failed with error code: $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            super.onConnectionStateChange(gatt, status, newState)
            Log.d(TAG, "Connection state changed: status=$status, newState=$newState")

            if (newState == android.bluetooth.BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "Connected to device, discovering services...")
                gatt.discoverServices()
            } else if (newState == android.bluetooth.BluetoothProfile.STATE_DISCONNECTED) {
                Log.i(TAG, "Disconnected from device")
                this@BleManager.gatt = null
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            super.onServicesDiscovered(gatt, status)
            Log.d(TAG, "Services discovered: status=$status")

            if (status == BluetoothGatt.GATT_SUCCESS) {
                val service = gatt.getService(LIGHTINK_SERVICE_UUID)
                if (service != null) {
                    configCharacteristic = service.getCharacteristic(CONFIG_CHAR_UUID)
                    if (configCharacteristic != null) {
                        Log.i(TAG, "Found config characteristic")
                        readConfig()
                    } else {
                        Log.e(TAG, "Config characteristic not found")
                    }
                } else {
                    Log.e(TAG, "LightInk service not found")
                }
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            super.onCharacteristicRead(gatt, characteristic, status)
            Log.d(TAG, "Characteristic read: status=$status")

            if (status == BluetoothGatt.GATT_SUCCESS && characteristic.uuid == CONFIG_CHAR_UUID) {
                val data = characteristic.value
                val jsonStr = String(data, Charsets.UTF_8)
                Log.i(TAG, "Read config from watch: $jsonStr")

                // Parse and log the config
                try {
                    val config = gson.fromJson(jsonStr, JsonObject::class.java)
                    Log.i(TAG, "Parsed config: $config")
                    
                    // Now write updated config back
                    writeConfig(config)
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to parse config JSON", e)
                }
            }
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            super.onCharacteristicWrite(gatt, characteristic, status)
            Log.d(TAG, "Characteristic write: status=$status")

            if (status == BluetoothGatt.GATT_SUCCESS && characteristic.uuid == CONFIG_CHAR_UUID) {
                Log.i(TAG, "Config written successfully to watch")
                // Disconnect after successful write
                gatt.disconnect()
            }
        }
    }

    fun scanAndConnect() {
        Log.i(TAG, "Starting BLE scan for LightInk device...")
        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_SCAN
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            Log.e(TAG, "Missing BLUETOOTH_SCAN permission")
            return
        }
        scanner?.startScan(scanCallback)
    }

    private fun stopScanning() {
        Log.d(TAG, "Stopping BLE scan")
        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_SCAN
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        scanner?.stopScan(scanCallback)
    }

    private fun connectToDevice(device: android.bluetooth.BluetoothDevice) {
        Log.i(TAG, "Connecting to device: ${device.address}")
        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_CONNECT
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            Log.e(TAG, "Missing BLUETOOTH_CONNECT permission")
            return
        }
        gatt = device.connectGatt(context, false, gattCallback)
    }

    private fun readConfig() {
        Log.d(TAG, "Reading config from watch...")
        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_CONNECT
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        configCharacteristic?.let {
            gatt?.readCharacteristic(it)
        }
    }

    private fun writeConfig(currentConfig: JsonObject) {
        Log.d(TAG, "Writing config to watch...")
        
        // Modify config as needed (for now, just echo it back)
        val updatedConfig = currentConfig.deepCopy()
        
        // Example: update timezone to 0 (UTC)
        // updatedConfig.addProperty("tz", 0)
        
        val jsonStr = gson.toJson(updatedConfig)
        Log.i(TAG, "Sending config: $jsonStr")

        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_CONNECT
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return
        }

        configCharacteristic?.let {
            it.value = jsonStr.toByteArray(Charsets.UTF_8)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt?.writeCharacteristic(it, it.value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            } else {
                @Suppress("DEPRECATION")
                it.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                gatt?.writeCharacteristic(it)
            }
        }
    }

    fun disconnect() {
        Log.d(TAG, "Disconnecting from device...")
        if (ActivityCompat.checkSelfPermission(
                context,
                android.Manifest.permission.BLUETOOTH_CONNECT
            ) != android.content.pm.PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }
}
