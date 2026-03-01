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

    // Wait until display busy goes off (this will not be needed in next HW versions)
    if constexpr (!HW::kHasDisplayBusyWake) {
      auto& busyWait = kDSState.busyWait[getSetDisplayMode()];
      gpio_mode_input<HW::Display::Busy>();
      while(GPIO_INPUT_GET(HW::Display::Busy) != 0) {
        microSleep(busyWait.kWaitStep);
        busyWait.currentWait += busyWait.kWaitStep;
        busyWait.missedTimes = 0; // Reset it
      }
      busyWait.missedTimes = std::min(busyWait.missedTimes, uint8_t(16));
      uint32_t reduceAmount = busyWait.kReduce << ++busyWait.missedTimes;
      busyWait.currentWait -= std::min(busyWait.currentWait / 2, reduceAmount);
    }

    if (kDSState.redrawDec) {
      kDSState.redrawDec = false;
      const auto& dec = kSettings.mWatchface.mCache.mDecimal;
      uSpi::writeArea(dec.data + dec.coord.size()*kSettings.mWatchface.mLastDraw.mMinuteD, dec.coord.x, dec.coord.y, dec.coord.w, dec.coord.h);
    }

    // Set display to sleep and then we turn off the GPIOs / advance minutes and go to sleep as well
    uSpi::hibernate();
    turnOffGpio();

    if constexpr (!HW::kHasDisplayBusyWake) {
      advanceMinutes(busyWait.currentWait);
    }

    // Set stub entry, then going to deep sleep again.
    esp_wake_stub_sleep(&wake_stub_deepsleep);
  }

  // Light on press button?
  // 8 for timer // 256 for touch
  if (wakeupCause == 256) {
    uint32_t mask;
    touch_ll_read_trigger_status_mask(&mask);

    // If it is the current set light pad button
    if ((mask >> kDSState.lightPad) & 1){
      // LED is powered from VDD, and we need to rise it to 3.3V,
      // because 1.9V is too low for the LED to light up
      Light::toggle();

      // esp_rom_delay_us(1000); // Small delay to avoid the evet to be too quick for the HW
      touch_ll_clear_trigger_status_mask(); // This will consume the touch

      // Go back to sleep, don´t touch the timer, if the user enters menu, then light will stay on
      esp_wake_stub_sleep(&wake_stub_deepsleep);
    }
    // Wake up, touch needs to handle by the Main code
    esp_default_wake_deep_sleep();
    return;
  }

  // Check if we should just do normal wakeup
  // If it is not a timer wakeup, return to handle on the Main code
  // 8 for timer // 256 for touch
  if (wakeupCause != 8 || kDSState.minutes <= 0) {
    esp_default_wake_deep_sleep();
    return;
  }

  // Turn on high power mode since it makes display use less power
  // Display boost uses -/+ 15 V, it is better 4V -> 3V -> 15V than 4V -> 1.9V -> 15V
  // It will take around 125us * 0.1V, for 1.4V ramp = 1.7ms, use 1ms of the reset
  Power::lock(Power::Flag::Display);

  // Turn off the light if it is on when we do a normal update
  Light::off();

  // Reset display to wake it up
#if (HW_VERSION >= 10)
  CLEAR_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, descRes.hold_force);
#endif
  gpio_mode_output<HW::Display::Res>();
  GPIO_OUTPUT_SET(HW::Display::Res, 0);
  esp_rom_delay_us(1'000); // HW 10 suspiciusly is 2X this time ? CHECK!
  GPIO_OUTPUT_SET(HW::Display::Res, 1);
#if (HW_VERSION >= 10)
  SET_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, descRes.hold_force);
#endif

  // Calculate the areas to update based on time and watchface states
  const auto u = kDSState.currentMinutes % 10;
  const auto d = kDSState.currentMinutes / 10;

  auto& last = kSettings.mWatchface.mLastDraw;

  uSpi::init();
  if (u != last.mMinuteU[0]) {
    // Write minute U
    const auto& uni = kSettings.mWatchface.mCache.mUnits;
    uSpi::writeArea(uni.data + uni.coord.size()*u, uni.coord.x, uni.coord.y, uni.coord.w, uni.coord.h);
    last.mMinuteU[0] = last.mMinuteU[1];
    last.mMinuteU[1] = u;
  }
  if (d != last.mMinuteD) {
    // Write minute D + repeat it in the display hibernate
    const auto& dec = kSettings.mWatchface.mCache.mDecimal;
    uSpi::writeArea(dec.data + dec.coord.size()*d, dec.coord.x, dec.coord.y, dec.coord.w, dec.coord.h);
    last.mMinuteD = d;
    kDSState.redrawDec = true;
  }
  uSpi::refresh();
  turnOffGpio();
  kDSState.displayBusy = true;

  if constexpr (HW::kHasDisplayBusyWake) {
    // Just need to go back to sleep the correct amount
    // The display hibernation will happen without affecting the timer
    advanceMinutes(0);
  } else {
    // Set wakeup timer when we guess display will finish refreshing, to put display to hibernation
    esp_wake_stub_set_wakeup_time(busyWait.currentWait);
  }

  // Set stub entry, then going to deep sleep again.
  esp_wake_stub_sleep(&wake_stub_deepsleep);
}