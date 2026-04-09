/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <inttypes.h>
#include "esp_sleep.h"
#include "esp_cpu.h"
#include "rom/ets_sys.h"
#include "rom/gpio.h"
#include "esp_rom_sys.h"
#include "esp_wake_stub.h"
#include "sdkconfig.h"

// For touch detection and Light on/off
#include "hal/touch_sensor_ll.h"
// For WDT feeding
#include "hal/wdt_hal.h"

#include "hardware.h"
#include "power.h"
#include "light.h"
#include "uspi.h"
#include "deep_sleep.h"

#include "esp_attr.h"
#include "soc/rtc_periph.h"
#include "rom/gpio.h"

RTC_DATA_ATTR DeepSleepState kDSState;

void RTC_IRAM_ATTR turnOffGpio() {
  using B = HW::Spi;
  using D = HW::Display;
  for (auto& pin : std::array{D::Cs, D::Dc, D::Res, B::Mosi, B::Sck}) {
    GPIO_DIS_OUTPUT(pin);
  }
  gpio_mode_input<HW::Display::Busy>();
}

void RTC_IRAM_ATTR feed_wdt() {
  // More than 500ms it is better to reset the MWDT0
#if(HW_VERSION < 10)
  TIMERG0.wdtwprotect.wdt_wkey = TIMG_WDT_WKEY_VALUE;
#else
  TIMERG0.wdtwprotect.wdt_wkey = TIMG_WDT_WKEY_V;
#endif
  TIMERG0.wdtfeed.wdt_feed = 1;
  TIMERG0.wdtwprotect.wdt_wkey = 0;
}

#if (HW_VERSION >= 10)
const RTC_DATA_ATTR rtc_io_desc_t descRes = rtc_io_desc[rtc_io_num_map[HW::Display::Res]];
#endif

void RTC_IRAM_ATTR microSleep(uint32_t micros) {
  constexpr auto step = 400'000;
  feed_wdt();
  while (micros > step) {
    micros -= step;
    esp_rom_delay_us(step);
    feed_wdt();
  }
  esp_rom_delay_us(micros);
  feed_wdt();
}

void RTC_IRAM_ATTR advanceMinutes(uint32_t minus) {
  // Guess the amount to sleep until next one and advance counters
  kDSState.currentMinutes += kDSState.stepSize;
  kDSState.minutes -= kDSState.stepSize;
  auto minutes = kDSState.stepSize + (kDSState.minutes < 0 ? kDSState.minutes : 0);
  esp_wake_stub_set_wakeup_time(minutes * 60'000'000 - minus);
}

// wake up stub function stored in RTC memory
void RTC_IRAM_ATTR wake_stub_deepsleep(void)
{
  // This sets up the delay to work properly
  auto& busyWait = kDSState.busyWait[getSetDisplayMode()];
#if (HW_VERSION < 10)
  ets_update_cpu_frequency_rom(ets_get_detected_xtal_freq() / 1'000'000);
#endif

  const auto wakeupCause = esp_wake_stub_get_wakeup_cause();

  // If we were waiting for a display finish, we need to complete it first
  if (kDSState.displayBusy) {
    kDSState.displayBusy = false;

    // Go back to low power mode, since to update the RAM low power is enough
    Power::unlock(Power::Flag::Display);
    uSpi::init();

    // Wait until display busy goes off
    if constexpr (!HW::kHasDisplayBusyWake) {
      auto& busyWait = kDSState.busyWait[getSetDisplayMode()];
      gpio_mode_input<HW::Display::Busy>();
      while(GPIO_INPUT_GET(HW::Display::Busy) != 0) {
        microSleep(busyWait.kWaitStep);
        busyWait.currentWait += busyWait.kWaitStep;
        busyWait.missedTimes = 0;
      }
      busyWait.missedTimes = std::min(busyWait.missedTimes, uint8_t(16));
      uint32_t reduceAmount = busyWait.kReduce << ++busyWait.missedTimes;
      busyWait.currentWait -= std::min(busyWait.currentWait / 2, reduceAmount);
    }

    // Set display to sleep and then we turn off the GPIOs / advance minutes and go to sleep
    uSpi::hibernate();
    turnOffGpio();

    if constexpr (!HW::kHasDisplayBusyWake) {
      advanceMinutes(busyWait.currentWait);
    }

    esp_wake_stub_sleep(&wake_stub_deepsleep);
  }

  // Light on press button?
  // 8 for timer // 256 for touch
  if (wakeupCause == 256) {
    uint32_t mask;
    touch_ll_read_trigger_status_mask(&mask);

    if ((mask >> kDSState.lightPad) & 1) {
      Light::toggle();
      touch_ll_clear_trigger_status_mask();
      esp_wake_stub_sleep(&wake_stub_deepsleep);
    }
    // Wake up, touch needs to handle by the Main code
    esp_default_wake_deep_sleep();
    return;
  }

  // Check if we should just do normal wakeup
  if (wakeupCause != 8 || kDSState.minutes <= 0) {
    esp_default_wake_deep_sleep();
    return;
  }

  // Check if we have a pre-computed delta to apply
  if (kDSState.mDeltaIndex >= kDSState.mDeltaCount ||
      !kDSState.mDeltas[kDSState.mDeltaIndex].valid()) {
    // No more pre-computed frames — do a full CPU wakeup to recompute
    esp_default_wake_deep_sleep();
    return;
  }

  // Turn on high power mode for display
  Power::lock(Power::Flag::Display);
  Light::off();

  // Reset display to wake it up
#if (HW_VERSION >= 10)
  CLEAR_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, descRes.hold_force);
#endif
  gpio_mode_output<HW::Display::Res>();
  GPIO_OUTPUT_SET(HW::Display::Res, 0);
  esp_rom_delay_us(1'000);
  GPIO_OUTPUT_SET(HW::Display::Res, 1);
#if (HW_VERSION >= 10)
  SET_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, descRes.hold_force);
#endif

  // Fetch the current delta frame
  const DeltaFrame& frame = kDSState.mDeltas[kDSState.mDeltaIndex];

  uSpi::init();

  // Send the delta bytes directly to the display via SPI.
  // The delta payload contains triplets: (offset_lo, offset_hi, value).
  // We iterate through and send each changed byte to its display location.
  {
    constexpr size_t kEntrySize = 3;
    const uint8_t* p   = frame.mPayload;
    const uint8_t* end = frame.mPayload + frame.mPayloadSize;

    while (p + kEntrySize <= end) {
      uint16_t offset = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
      uint8_t  value  = p[2];
      p += kEntrySize;

      // Convert buffer offset to display coordinates
      uint8_t row    = offset / Display::WB_BITMAP;
      uint8_t colByte = offset % Display::WB_BITMAP;
      uint8_t x      = colByte * 8;

      // Send this single byte to the display
      uSpi::writeArea(&value, x, row, 8, 1);
    }
  }

  // Advance to next frame
  kDSState.mDeltaIndex++;

  uSpi::refresh();
  turnOffGpio();
  kDSState.displayBusy = true;

  if constexpr (HW::kHasDisplayBusyWake) {
    advanceMinutes(0);
  } else {
    esp_wake_stub_set_wakeup_time(busyWait.currentWait);
  }

  esp_wake_stub_sleep(&wake_stub_deepsleep);
}
