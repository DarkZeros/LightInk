# LightInk Companion Android App

A minimal Kotlin Android companion app for the LightInk ESP32 smartwatch. The app scans for and connects to the watch via BLE, reads the current configuration, and can write updated settings back.

## Features

- **BLE Scanning**: Automatically scans for devices named "LightInk"
- **Configuration Exchange**: Reads JSON config from the watch and writes it back
- **No UI Required**: All operations logged to Logcat for debugging
- **Minimal Dependencies**: Uses Android's built-in BLE API and Gson for JSON

## Project Structure

```
Android/LightInkCompanion/
├── app/
│   ├── build.gradle.kts          # App-level Gradle configuration
│   ├── src/main/
│   │   ├── AndroidManifest.xml   # App manifest with BLE permissions
│   │   ├── java/com/lightink/companion/
│   │   │   ├── MainActivity.kt    # Entry point, handles permissions
│   │   │   └── ble/
│   │   │       └── BleManager.kt  # BLE scanning, connection, config exchange
│   │   └── res/
│   │       ├── layout/
│   │       │   └── activity_main.xml
│   │       └── values/
│   │           ├── colors.xml
│   │           ├── strings.xml
│   │           └── themes.xml
├── build.gradle.kts              # Root Gradle configuration
└── settings.gradle.kts           # Gradle settings

```

## Building

1. Open the project in Android Studio
2. Sync Gradle files
3. Build and run on an Android device (API 26+)

## How It Works

### BLE Flow

1. **MainActivity** requests BLE permissions (Android 12+)
2. **BleManager** scans for devices advertising as "LightInk"
3. On discovery, connects to the device and discovers GATT services
4. Finds the custom LightInk service and config characteristic
5. **Reads** current config from the watch (JSON format)
6. **Writes** the config back (can be modified before writing)
7. Disconnects after successful exchange

### Configuration Format

The config is exchanged as JSON:

```json
{
  "tz": 60,
  "beep": false,
  "vib": false,
  "night": false,
  "auto_pwr": true,
  "bat": 3700
}
```

- `tz`: Timezone offset in minutes (e.g., 60 = UTC+1)
- `beep`: Hourly beep enabled
- `vib`: Hourly vibration enabled
- `night`: Night mode (0-6am low power)
- `auto_pwr`: Auto power saving based on battery
- `bat`: Current battery voltage in mV

### Custom UUIDs

- **Service UUID**: `4c494748-5449-4e4b-0000-000000000000`
- **Config Characteristic UUID**: `4c494748-5449-4e4b-0000-000000000001`

These match the ESP32 firmware implementation.

## Permissions

The app requires the following permissions (declared in `AndroidManifest.xml`):

- `BLUETOOTH_SCAN` (Android 12+)
- `BLUETOOTH_CONNECT` (Android 12+)
- `ACCESS_FINE_LOCATION` (Android 12+)
- `BLUETOOTH` (Android 11 and below)
- `BLUETOOTH_ADMIN` (Android 11 and below)

## Logging

All BLE operations are logged with the tag `BleManager`. View logs in Android Studio's Logcat:

```
adb logcat | grep BleManager
```

## Future Enhancements

- Add UI to display and modify config values
- Persist config to SharedPreferences
- Add support for multiple paired devices
- Implement config validation
- Add error handling and retry logic
