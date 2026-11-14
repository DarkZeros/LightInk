# LightInk companion app

Android app (Kotlin, Jetpack Compose) that pairs with the watch over Bluetooth LE and
edits its settings. The phone advertises and hosts the GATT service, the watch connects
when it wants to sync. The app has no settings list of its own: it builds its screens
from the description the watch sends. Wire format: [docs/ble_protocol.md](../docs/ble_protocol.md),
kept in sync with `app/src/main/java/com/lightink/companion/Protocol.kt`.

## Requirements

- JDK 17 to 21. If the default `java` is another version, point `JAVA_HOME` at one of
  these.
- Android SDK with platform 34. Either set `ANDROID_HOME`, or create `local.properties`
  here with `sdk.dir=/path/to/Android/Sdk` (gitignored).
- Nothing else: the Gradle wrapper downloads Gradle 8.14 and the dependencies on the
  first build.

## Build

All commands run from this directory (`android/`).

```bash
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64   # If needed
export ANDROID_HOME=~/Android/Sdk                      # Or local.properties

./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk
./gradlew testDebugUnitTest    # Wire format tests, no phone needed
```

Android Studio works as well: open this directory as the project.

## Install on the phone

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
# or build and install in one step
./gradlew installDebug
```

The phone needs Android 8.0 (API 26) or newer.

`-r` replaces the app and keeps its data, so the watch stays paired. Uninstalling the
app loses its half of the pairing, while Android keeps its own Bluetooth pairing with
the watch. To pair again, forget the phone on the watch and remove "LightInk" in the
phone's Bluetooth settings.

Debug builds are signed with the debug key of the computer that built them
(`~/.android/debug.keystore`). A build from another computer can't replace the
installed one; Android asks to uninstall it first, which loses the pairing.

### adb without a cable

1. On the phone: *Developer options › Wireless debugging*, on.
2. The first time on a computer: *Pair device with pairing code* on that screen, then
   `adb pair <ip>:<pairing port>` with the code.
3. `adb connect <ip>:<port>`, with the address shown on the Wireless debugging screen.
   The port changes each time it is turned on; `avahi-browse -t -r _adb-tls-connect._tcp`
   finds it from the computer.

## Pairing with the watch

1. In the app, tap *Pair watch*.
2. On the watch, open *Bluetooth › Pair phone*.
3. The watch shows a 6 digit code: type it on the phone when asked.

To pair again, use *Forget watch* in the app and *Bluetooth › Forget phone* on the
watch first, so both sides start clean. If the watch says "Pairing failed" right away,
Android still remembers it: remove "LightInk" (or its address) in the phone's Bluetooth
settings and try again.
