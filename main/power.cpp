#include "power.h"
#include "hardware.h"
#include "settings.h"

#include "driver/gpio.h"

#include "esp32-hal-log.h"

namespace {
  RTC_SLOW_ATTR bool kPrev{false};
  RTC_SLOW_ATTR std::atomic<uint8_t> kLock{0};
} // namespace

#include "soc/rtc_periph.h"
#include "rom/gpio.h"
#include "deep_sleep_utils.h"

void Power::lock(Flag f) {
  // ESP_LOGE("power", "lock -> %x + %x", kLock.load(), f);
  // If the value returned is "0", this thread is responsible for locking
  if (kLock.fetch_or(f, std::memory_order_acq_rel) == 0) {
    high();
  }
}

void Power::unlock(Flag f) {
  // ESP_LOGE("power", "unlock -> %x + %x", kLock.load(), f);
  // If the value returned is the flag, this thread is responsible for releasing
  if (kLock.fetch_and(~f, std::memory_order_acq_rel) == f) {
    low();
  }
}

bool Power::current() {
  return kPrev;
}
Power::Flag Power::status() {
  return static_cast<Power::Flag>(kLock.load());
}

// Need to store this in RTC memory since will not be available in DeepSleep
const RTC_SLOW_ATTR rtc_io_desc_t desc = rtc_io_desc[rtc_io_num_map[HW::kVoltageSelectPin]];
extern struct Settings kSettings;

void Power::set(bool high) {
  // Could be that the board only supports a given high voltage
  if constexpr (!HW::kHasLowVoltage)
    high = true;

  if (!kSettings.mPower.mLowVoltage)
    high = true;

  // Caches previous values
  if (kPrev == high)
    return;
  kPrev = high;

  // The value changes based on the board! Old boards have the values fliped
  if constexpr (HW::kVoltageSelectInverted)
    high = !high;

  // Hold disable
#if (HW_VERSION < 10)
  REG_CLR_BIT(RTC_CNTL_HOLD_FORCE_REG, desc.hold_force);
  REG_CLR_BIT(desc.reg, desc.hold);
#else
  CLEAR_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, desc.hold_force);
#endif
  // Deep sleep hold disable
  CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_AUTOHOLD_EN_M);

  gpio_mode_output<HW::kVoltageSelectPin>();
  GPIO_OUTPUT_SET(HW::kVoltageSelectPin, high);

  // Hold enable
#if (HW_VERSION < 10)
  REG_SET_BIT(RTC_CNTL_HOLD_FORCE_REG, desc.hold_force);
  REG_SET_BIT(desc.reg, desc.hold);
#else
  SET_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, desc.hold_force);
#endif

  // Deep sleep hold enable
  CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_FORCE_UNHOLD);
  SET_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_AUTOHOLD_EN_M);
}