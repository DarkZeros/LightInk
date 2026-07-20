# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

LightInk is an ultra-low-power (~2µA idle) E-Ink ESP32 smartwatch that can run entirely on solar power. It is based on the "Watchy" concept but with custom hardware and a heavily optimized firmware. The whole design philosophy is *minimize power at all costs* — most non-obvious code exists to shave microamps, and that constraint should drive changes.

Three sub-projects live in one git repo:
- `firmware/` — the ESP32 firmware (ESP-IDF + Arduino-as-a-component, C++23). This is the bulk of the work.
- `Android/LightInkCompanion/` — a minimal Kotlin BLE companion app that reads/writes watch config as JSON.
- `hardware/` — PCB schematics/BOM (`hardware/pcb/`) and case STLs (`hardware/case/`).

## Building the firmware

The ESP-IDF is a **git submodule** (`firmware/esp-idf`, a DarkZeros fork of v5.1 with local changes — do not assume it matches upstream). All commands run from `firmware/`.

```bash
git submodule update --init --recursive     # first checkout only
cd firmware
./setup.sh                                   # installs esp-idf toolchain, sources export.sh, fullclean
idf.py build flash monitor
```

`setup.sh` runs `./esp-idf/install.sh` then `. ./esp-idf/export.sh`. In a fresh shell you must re-source the environment (`. firmware/esp-idf/export.sh`) before `idf.py` works.

Other useful `idf.py` commands: `idf.py build`, `idf.py flash`, `idf.py monitor`, `idf.py fullclean`, `idf.py menuconfig`, `idf.py save-defconfig`.

### Targets and config

- Two targets: **esp32** (older HW, up to board v4) and **esp32s3** (current WIP board, v10). Per-target defaults live in `sdkconfig.defaults.esp32` / `sdkconfig.defaults.esp32s3`; the active config is `sdkconfig` (gitignored, regenerated).
- Partition table is custom (`partitions.csv`): a 2M factory + 1M OTA slot.
- `main/secrets.cpp` is **gitignored** — it defines `kWifiNetworks` and `kSignals` (declared in `main/secrets.h`). It must exist for the build to link; create it locally.

### Tests

`firmware/pytest.py` is the stock ESP-IDF hello-world example harness (pytest-embedded) and is not wired to the LightInk app — there is no meaningful firmware test suite here yet.

## Firmware architecture

The firmware has **no main loop**. `app_main` (`main/main.cpp`) enables power management (light sleep) and constructs a single `Core` object; the entire program is `Core`'s constructor, which runs once per wake and ends in `esp_deep_sleep_start()`. Every wake is a full boot from deep sleep.

### The wake/sleep cycle — read `main/core.cpp` first

`Core::Core()` (`main/core.cpp`) is the heart of the system. On each wake it:
1. Branches on `esp_sleep_get_wakeup_cause()`: `TOUCHPAD` → `handleTouch()`; `TIMER` → periodic redraw / watchdog reset to watchface; `EXT1` → LoRa packet RX; first boot → GPIO/time/GPS init.
2. Draws either the watchface (when `kSettings.mUi.mDepth < 0`) or the current menu UI element.
3. Computes how long to sleep (adaptive `stepSize` based on battery %, night mode, and any `setNextUpdate()` request), arms the wake sources, and enters deep sleep.

State that must survive deep sleep lives in `RTC_DATA_ATTR` globals — chiefly `kSettings` (`main/settings.h`, the full persisted config tree) and `kDSState` (`main/deep_sleep.h`, wake-stub bookkeeping).

### The deep-sleep wake stub — the key power trick

To update the clock every minute without a full ESP32 boot (~1ms of CPU on-time), the firmware registers a **ROM wake stub** (`esp_set_deep_sleep_wake_stub(&wake_stub_deepsleep)`). `main/deep_sleep.cpp` + `main/uspi.cpp` implement a minimal hardware SPI display driver (`uSpi` namespace) that runs entirely from `RTC_IRAM_ATTR` code during the stub, redraws the changed digits, and goes back to sleep — never running `app_main`. Anything the stub touches (functions, data) must be RTC/IRAM-resident. This is why there are effectively **two display drivers**: the full Adafruit-GFX-based one (`main/display*.cpp`) for normal draws, and the stripped `uSpi` one for wake-stub updates.

### Hardware abstraction — `main/hardware.h`

All board revisions (v1–v4 ESP32, v10 ESP32-S3) are `constexpr struct`s (`HW_1`..`HW_10`) selected at compile time via `HW_VERSION` (derived from the IDF target). Pin assignments and capability flags (`kHasLora`, `kHasGps`, `kHasDisplayBusyWake`, `kHasLowVoltage`, ...) are `constexpr`, so board-specific code is guarded with `if constexpr (HW::kHasGps)` etc. and compiled away for boards that lack the feature. Use `HW::` and these flags rather than hardcoding pins or `#ifdef`s.

### Subsystems (one class each, all owned by `Core`)

`Time`, `Battery`, `Spi`, `Radio` (LoRa/GPS-signal via RadioLib), `Display`, `Gps`, `Touch` (capacitive pads, not buttons). Peripherals (speaker, vibrator) are free functions in `Peripherals::`. `Power::Lock` is an RAII guard that reference-counts which subsystems need the boosted power rail. `Light::` controls the backlight LED.

### The UI/menu system — `main/ui.h`, `main/menus.cpp`

Menus are a compile-time tree built from `std::variant` (`UI::Any`). Behavior is duck-typed via SFINAE detectors generated by the `GENERATE_HAS(func)` macro in `ui.h` (`has_render`, `has_sub`, `has_button`, `has_button_updown`, ...). `Core::findUi()` walks `kSettings.mUi.mState[]`/`mDepth` to resolve the currently focused element; `std::visit` dispatches rendering and touch events, falling back to default navigation (back = `mDepth--`) when an element doesn't implement a handler. To add a menu entry, add a struct implementing the relevant `render`/`button*`/`sub` methods and wire it into `generateMenus()`.

### BLE companion sync — `main/ble.cpp`

NimBLE GATT server exposing a JSON config characteristic. Service UUID `4c494748-5449-4e4b-0000-000000000000`, config char `...0001` — these must stay in sync with `Android/.../ble/BleManager.kt`. On a timer wake, if `kSettings.mBlePaired`, `Core` auto-runs `ble_init()/ble_sync()/ble_deinit()`.

### Networking

WiFi/NTP/geolocation live in `Core::NTPSync()` (`core.cpp`): connects via credentials in `secrets.cpp`, syncs SNTP (`pool.ntp.org`), and fetches lat/lon + timezone offset from `ip-api.com`. WiFi is only powered on transiently because it is expensive.

## Conventions

- **C++23**, `-fpermissive -Wno-volatile`, size-optimized (`-Os`). Uses `magic_enum`, `fmt`, `std::optional`, `std::variant`, `std::future`/`std::async` (deferred + async launch) heavily.
- Member fields use an `m`-prefix (`mTime`, `mDisplay`); compile-time constants use a `k`-prefix (`kSettings`, `kDSState`, `kMaxTimeGpsOn`).
- Dependencies are managed two ways: git submodules under `firmware/components/` (see `.gitmodules`: WiFiManager, Adafruit GFX/BusIO, NTPClient, Time, ArduinoJson, ArduinoNvs, magic_enum, sunset, RadioLib) and IDF Component Manager entries in `main/idf_component.yml` (`espressif/fmt`, `espressif/arduino-esp32`). New `main/` source files must be added to the `srcs` list in `main/CMakeLists.txt`.
- `main/format.sh` documents the clang-format style (LLVM base + aligned macros/assignments), though most of the codebase predates it.

## Working on power-sensitive code

Before changing anything in the wake path, sleep configuration, GPIO setup, or the `uSpi`/wake-stub code, understand that correctness here is measured in microamps. Leaving a GPIO floating, a peripheral rail on, or a wake source armed can multiply idle consumption by orders of magnitude. The first-boot block in `Core::Core()` deliberately forces unused GPIOs to input, and `turnOffGpio()` in `deep_sleep.cpp` releases display pins before sleep — preserve these patterns.
