#include "light.h"
#include "hardware.h"
#include "power.h"

#include "driver/gpio.h"

#include "esp32-hal-log.h"

#include "Arduino.h"

namespace {
  RTC_SLOW_ATTR bool kPrev{false};
} // namespace

#include "soc/rtc_periph.h"
#include "rom/gpio.h"
#include "deep_sleep_utils.h"

void Light::onFor(uint32_t ms) {
  on();
  delay(ms);
  off();
}

bool Light::toggle() {
  set(!kPrev);
  return kPrev;
}

// Need to store this in RTC memory since will not be available in DeepSleep
const RTC_SLOW_ATTR rtc_io_desc_t desc = rtc_io_desc[rtc_io_num_map[HW::kLightPin != (uint8_t)-1 ? HW::kLightPin : 0]];

void Light::set(bool high) {
  if constexpr (HW::kLightPin == (uint8_t)(-1)) return;
  // Caches previous values
  if (kPrev == high)
    return;

  Power::hold(desc, false);

  gpio_mode_output<HW::kLightPin>();
  GPIO_OUTPUT_SET(HW::kLightPin, high);

  // LED requires high power while it is on, locking it
  if (high) {
    Power::lock(Power::Flag::Light);
  } else {
    Power::unlock(Power::Flag::Light);
  }

  Power::hold(desc, true);

  kPrev = high;
}
