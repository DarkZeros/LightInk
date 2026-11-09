#include "power.h"
#include "hardware.h"
#include "settings.h"
#include "trace.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_cpu.h"
#include "xtensa/xtruntime.h"
#include "xtensa/config/core-isa.h"

#include "esp32-hal-log.h"

namespace {
  RTC_SLOW_ATTR bool kPrev{false};
  RTC_SLOW_ATTR std::atomic<uint16_t> kLock{0};

  // Peterson's lock for Critical, one contender per core since it runs with
  // interrupts off. Only plain loads and stores: S32C1I is not atomic between
  // cores on RTC memory, measured on device
  RTC_SLOW_ATTR volatile uint32_t kCriticalWants[2]{};
  RTC_SLOW_ATTR volatile uint32_t kCriticalTurn{0};

  // Far above the 20 spins measured with both cores hammering it
  constexpr uint32_t kMaxCriticalSpins = 1'000;
} // namespace

#include "soc/rtc_periph.h"
#include "hal/rtc_io_periph.h"
#include "rom/gpio.h"
#include "deep_sleep_utils.h"

Power::Critical::Critical() : mIntLevel(XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL)) {
  const uint32_t me = esp_cpu_get_core_id(), other = me ^ 1;
  kCriticalWants[me] = 1;
  kCriticalTurn = other;
  // Bounded since deep sleep can stall the other core while it holds this,
  // and RTC memory keeps it set, which would hang the wake stub forever
  for (uint32_t i = 0; i < kMaxCriticalSpins; ++i) {
    if (!kCriticalWants[other] || kCriticalTurn != other)
      break;
  }
}

Power::Critical::~Critical() {
  kCriticalWants[esp_cpu_get_core_id()] = 0;
  XTOS_RESTORE_INTLEVEL(mIntLevel);
}

void Power::setHold(const rtc_io_desc_t& pad, bool enable) {
  if (enable) {
#if (HW_VERSION < 10)
    REG_SET_BIT(RTC_CNTL_HOLD_FORCE_REG, pad.hold_force);
    REG_SET_BIT(pad.reg, pad.hold);
#else
    SET_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, pad.hold_force);
#endif
    // Deep sleep hold enable
    CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_FORCE_UNHOLD);
    SET_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_AUTOHOLD_EN_M);
  } else {
#if (HW_VERSION < 10)
    REG_CLR_BIT(RTC_CNTL_HOLD_FORCE_REG, pad.hold_force);
    REG_CLR_BIT(pad.reg, pad.hold);
#else
    CLEAR_PERI_REG_MASK(RTC_CNTL_PAD_HOLD_REG, pad.hold_force);
#endif
    // Deep sleep hold disable
    CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_AUTOHOLD_EN_M);
  }
}

void Power::hold(const rtc_io_desc_t& pad, bool enable) {
  Critical lock;
  setHold(pad, enable);
}

void Power::hold(gpio_num_t pin, bool enable) {
  Critical lock;
  if (enable)
    rtc_gpio_hold_en(pin);
  else
    rtc_gpio_hold_dis(pin);
}

void Power::lock(Flag f) {
  uint16_t mask;
  bool rail;
  {
    // The mask and the pin change together, so once this returns the rail is high
    Critical lock;
    // ESP_LOGE("power", "lock -> %x + %x", kLock.load(), f);
    // If the value returned is "0", this thread is responsible for locking
    if (kLock.fetch_or(f, std::memory_order_acq_rel) == 0) {
      high();
    }
    mask = kLock.load();
    rail = kPrev;
  }
  // Outside Critical: the app trace takes the stdout lock, which aborts with interrupts off
  Trace::power(f, true, mask, rail);
}

void Power::unlock(Flag f) {
  uint16_t mask;
  bool rail;
  {
    Critical lock;
    // ESP_LOGE("power", "unlock -> %x + %x", kLock.load(), f);
    // If the value returned is the flag, this thread is responsible for releasing
    if (kLock.fetch_and(~f, std::memory_order_acq_rel) == f) {
      low();
    }
    mask = kLock.load();
    rail = kPrev;
  }
  Trace::power(f, false, mask, rail);
}

bool Power::current() {
  return kPrev;
}
Power::Flag Power::status() {
  return static_cast<Power::Flag>(kLock.load());
}

// Need to store this in RTC memory since will not be available in DeepSleep
const RTC_SLOW_ATTR rtc_io_desc_t desc __attribute__((init_priority(101))) = rtc_io_desc[rtc_io_num_map[HW::kVoltageSelectPin != (uint8_t)-1 ? HW::kVoltageSelectPin : 0]];
extern struct Settings kSettings;

void Power::set(bool high) {
  // Could be that the board only supports a given high voltage
  if constexpr (HW::kVoltageSelectPin == (uint8_t)-1)
    return;

  // Or that it supports but it does not want to
  if constexpr (!HW::kHasLowVoltage) {
    high = true;
  }

  if (!kSettings.mPower.mLowVoltage)
    high = true;

  // Caches previous values
  if (kPrev == high)
    return;
  kPrev = high;

  // The value changes based on the board! Old boards have the values fliped
  if constexpr (HW::kVoltageSelectInverted)
    high = !high;

  setHold(desc, false);

  gpio_mode_output<HW::kVoltageSelectPin>();
  GPIO_OUTPUT_SET(HW::kVoltageSelectPin, high);

  setHold(desc, true);
}
