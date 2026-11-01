#pragma once

// Debug-only SSD1681 experiments, replacing the watch app when
// CONFIG_LIGHTINK_DISPLAY_TEST is set (idf.py menuconfig -> LightInk display tests).
// Each test leaves a picture on the panel that answers one question about the
// controller, see docs/display_tests.md. When disabled this compiles to nothing.

#include "sdkconfig.h"

namespace DisplayTest {
#if CONFIG_LIGHTINK_DISPLAY_TEST
  constexpr bool kEnabled = true;
#else
  constexpr bool kEnabled = false;
#endif

  [[noreturn]] void runImpl();

  // Never returns when enabled: handles the wake, draws, deep sleeps
  inline void run() {
    if constexpr (kEnabled) runImpl();
  }
};
