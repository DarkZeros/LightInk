package com.lightink.companion

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.lightink.companion.ui.HomeScreen
import com.lightink.companion.ui.LightInkTheme
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

object Permissions {
    /** Advertising and the GATT server. No location: the phone never scans. */
    val required: Array<String> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        emptyArray() // BLUETOOTH and BLUETOOTH_ADMIN are granted at install
    }

    fun granted(context: Context) = required.all {
        ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
    }
}

class MainActivity : ComponentActivity() {
    private var permissionsGranted by mutableStateOf(false)

    private val askPermissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { refreshPermissions() }

    private val enableBluetooth = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { ensureService() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WatchStore.init(this)
        enableEdgeToEdge()
        refreshPermissions()

        // Start the service whenever there is a watch to serve (pairing included)
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                WatchStore.state.map { it.needsService }.distinctUntilChanged()
                    .collect { if (it) ensureService() }
            }
        }

        setContent {
            LightInkTheme {
                HomeScreen(
                    permissionsGranted = permissionsGranted,
                    onGrantPermissions = ::requestPermissions,
                    onEnableBluetooth = {
                        enableBluetooth.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
                    },
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refreshPermissions()
    }

    private fun requestPermissions() {
        val notifications = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            arrayOf(Manifest.permission.POST_NOTIFICATIONS) // Optional, for the service notice
        } else emptyArray()
        askPermissions.launch(Permissions.required + notifications)
    }

    private fun refreshPermissions() {
        permissionsGranted = Permissions.granted(this)
        ensureService()
    }

    private fun ensureService() {
        if (permissionsGranted && WatchStore.state.value.needsService) SyncService.start(this)
    }
}
