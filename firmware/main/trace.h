#pragma once

// Debug-only trace over UART0: display SPI traffic, wakes, power locks, peripherals,
// WiFi, and scripted touches.
// Enabled with CONFIG_LIGHTINK_TRACE (idf.py menuconfig -> LightInk debug trace),
// decoded and visualized by tools/lightink_trace.py.
// When disabled, every hook compiles to nothing.

#include <cstdint>

#include "esp_attr.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

namespace Trace {
#if CONFIG_LIGHTINK_TRACE
  constexpr bool kEnabled = true;
#else
  constexpr bool kEnabled = false;
#endif
  // Touch script replay: with the trace, or alone for power tests (CONFIG_LIGHTINK_SCRIPT)
#if CONFIG_LIGHTINK_TRACE || CONFIG_LIGHTINK_SCRIPT
  constexpr bool kScript = true;
#else
  constexpr bool kScript = false;
#endif

  void spiImpl(const void* data, uint32_t len);
  void stubWakeImpl(uint32_t cause);
  esp_sleep_wakeup_cause_t wakeCauseImpl(esp_sleep_wakeup_cause_t cause);
  uint8_t touchImpl(uint8_t btn);
  void sleepImpl();
  void powerImpl(uint8_t flag, bool locked, uint8_t mask, bool rail);
  void periphImpl(uint8_t kind, const uint16_t* values, uint8_t count);
  void radioImpl(float mhz, uint32_t bitMicros, uint8_t repetitions, uint32_t bits);
  void wifiStartedImpl();
  void ntpSyncedImpl();

  // Bytes about to be sent to the display (DC level is read from the GPIO)
  FORCE_INLINE_ATTR void spi(const void* data, uint32_t len) {
    if constexpr (kEnabled) spiImpl(data, len);
  }
  // Wake stub entry
  FORCE_INLINE_ATTR void stubWake(uint32_t cause) {
    if constexpr (kEnabled) stubWakeImpl(cause);
  }
  // App boot, a scripted step turns a timer wake into a touch wake
  inline esp_sleep_wakeup_cause_t wakeCause(esp_sleep_wakeup_cause_t cause) {
    if constexpr (kScript) return wakeCauseImpl(cause);
    return cause;
  }
  // Button handled this wake, replaced by the scripted one if any
  template<typename Btn>
  inline Btn touch(Btn btn) {
    if constexpr (kScript) return static_cast<Btn>(touchImpl(btn));
    return btn;
  }
  // Just before esp_deep_sleep_start(), arms the next scripted step
  inline void sleep() {
    if constexpr (kScript) sleepImpl();
  }
  // Power::lock/unlock of a subsystem flag, mask after it, and the rail level
  FORCE_INLINE_ATTR void power(uint8_t flag, bool locked, uint8_t mask, bool rail) {
    if constexpr (kEnabled) powerImpl(flag, locked, mask, rail);
  }
  // Vibrator pattern: on/off durations in ms
  template<typename Pattern>
  inline void vibrator(const Pattern& pattern) {
    if constexpr (kEnabled) {
      uint16_t v[32];
      uint8_t n = 0;
      for (auto ms : pattern)
        if (n < 32) v[n++] = ms;
      periphImpl(1, v, n);
    }
  }
  // Speaker pattern: (frequency Hz, duration ms) pairs
  template<typename Pattern>
  inline void speaker(const Pattern& pattern) {
    if constexpr (kEnabled) {
      uint16_t v[32];
      uint8_t n = 0;
      for (auto& [freq, ms] : pattern) {
        if (n + 2 > 32) break;
        v[n++] = freq;
        v[n++] = ms;
      }
      periphImpl(2, v, n);
    }
  }
  // OOK burst about to be sent: carrier MHz, bit time, repetitions, bits on air per repetition
  inline void radio(float mhz, uint32_t bitMicros, uint8_t repetitions, uint32_t bits) {
    if constexpr (kEnabled) radioImpl(mhz, bitMicros, repetitions, bits);
  }
  // WiFi driver started: subscribe to its events
  inline void wifiStarted() {
    if constexpr (kEnabled) wifiStartedImpl();
  }
  inline void ntpSynced() {
    if constexpr (kEnabled) ntpSyncedImpl();
  }
};
