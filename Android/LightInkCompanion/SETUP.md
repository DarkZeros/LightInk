# LightInk Companion - Setup & Build Guide

## Prerequisites

### Required
- **Android SDK** (API 26+)
- **Java Development Kit (JDK)** 11 or higher
- **Gradle** (included via gradlew)

### Optional
- **Android Studio** (for development, not required for building)
- **adb** (Android Debug Bridge, for installing on device)

## Setup

### 1. Install Android SDK

#### Linux/macOS
```bash
# Download Android SDK Command Line Tools from:
# https://developer.android.com/studio#command-tools

# Extract to ~/Android/Sdk
mkdir -p ~/Android/Sdk
unzip commandlinetools-*.zip -d ~/Android/Sdk

# Set environment variable
export ANDROID_SDK_ROOT=~/Android/Sdk
```

#### Windows
```cmd
# Download Android SDK Command Line Tools from:
# https://developer.android.com/studio#command-tools

# Extract to %USERPROFILE%\Android\Sdk
# Set environment variable:
setx ANDROID_SDK_ROOT %USERPROFILE%\Android\Sdk
```

### 2. Install SDK Components

```bash
# Set SDK root
export ANDROID_SDK_ROOT=~/Android/Sdk

# Install required SDK components
$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager --sdk_root=$ANDROID_SDK_ROOT \
  "platforms;android-34" \
  "build-tools;34.0.0" \
  "platform-tools"
```

### 3. Install Java Development Kit (JDK)

#### Linux (Ubuntu/Debian)
```bash
sudo apt-get install openjdk-11-jdk
```

#### macOS
```bash
brew install openjdk@11
```

#### Windows
Download from: https://www.oracle.com/java/technologies/downloads/

## Building the APK

### Using the Build Script (Recommended)

```bash
cd Android/LightInkCompanion

# Build debug APK (default)
./build.sh

# Build release APK
./build.sh release
```

The script will:
1. Auto-detect Android SDK location
2. Run Gradle build
3. Output APK path and size
4. Show installation instructions

### Using Gradle Directly

```bash
cd Android/LightInkCompanion

# Build debug APK
./gradlew assembleDebug

# Build release APK
./gradlew assembleRelease

# Clean build
./gradlew clean assembleDebug
```

### Output Locations

- **Debug APK**: `app/build/outputs/apk/debug/app-debug.apk`
- **Release APK**: `app/build/outputs/apk/release/app-release-unsigned.apk`

## Installing on Device

### Prerequisites
- Android device with USB debugging enabled
- USB cable
- `adb` installed and in PATH

### Installation

```bash
# Install APK
adb install -r app/build/outputs/apk/debug/app-debug.apk

# Install and run
adb install -r app/build/outputs/apk/debug/app-debug.apk && \
adb shell am start -n com.lightink.companion/.MainActivity

# View logs
adb logcat | grep BleManager
```

## Troubleshooting

### "ANDROID_SDK_ROOT not found"
```bash
# Set environment variable
export ANDROID_SDK_ROOT=~/Android/Sdk

# Or add to ~/.bashrc or ~/.zshrc for persistence
echo 'export ANDROID_SDK_ROOT=~/Android/Sdk' >> ~/.bashrc
source ~/.bashrc
```

### "gradlew: command not found"
```bash
# Make sure you're in the project directory
cd Android/LightInkCompanion

# Make gradlew executable
chmod +x gradlew
```

### "No connected devices"
```bash
# List connected devices
adb devices

# Enable USB debugging on your Android device:
# Settings > Developer Options > USB Debugging
```

### "Could not find or load main class org.gradle.wrapper.GradleWrapperMain"
```
# The gradle-wrapper.jar is missing from gradle/wrapper/
# This can happen if it was excluded by .gitignore or a fresh clone

# Option 1: Run build.sh which will auto-download it
./build.sh

# Option 2: Download manually using Gradle
gradle wrapper

# Option 3: Download from the Gradle distribution
# Check gradle/wrapper/gradle-wrapper.properties for the distribution URL
# Download the zip, extract gradle-wrapper-*.jar from lib/ and place in gradle/wrapper/
```

### Build fails with "Could not find tools.jar"
```bash
# Set JAVA_HOME
export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64
# (path may vary depending on your JDK installation)
```

## Development

### Opening in Android Studio

1. Open Android Studio
2. File → Open → Select `Android/LightInkCompanion` folder
3. Wait for Gradle sync to complete
4. Build and run from IDE

### Code Structure

```
app/src/main/
├── java/com/lightink/companion/
│   ├── MainActivity.kt          # Entry point
│   └── ble/
│       └── BleManager.kt        # BLE logic
├── res/
│   ├── layout/
│   │   └── activity_main.xml
│   └── values/
│       ├── colors.xml
│       ├── strings.xml
│       └── themes.xml
└── AndroidManifest.xml
```

### Modifying Configuration

Edit `BleManager.kt` to change:
- Device name to scan for (currently "LightInk")
- Service/Characteristic UUIDs
- Config JSON fields
- Timeout behavior

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build APK

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - uses: actions/setup-java@v3
        with:
          java-version: '11'
      - name: Build APK
        run: |
          cd Android/LightInkCompanion
          chmod +x build.sh
          ./build.sh debug
      - name: Upload APK
        uses: actions/upload-artifact@v3
        with:
          name: app-debug.apk
          path: Android/LightInkCompanion/app/build/outputs/apk/debug/app-debug.apk
```

## Next Steps

1. Build the APK using `./build.sh`
2. Install on your Android device
3. Pair with LightInk watch via BLE Sync menu
4. Check Logcat for debug output: `adb logcat | grep BleManager`
