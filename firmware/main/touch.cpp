#include "touch.h"
#include "deep_sleep.h"
#include "power.h"

#include "soc/rtc.h"

#include "hal/touch_sensor_hal.h"
#include "driver/touch_sensor.h"
#include "esp_sleep.h"

namespace {
  RTC_DATA_ATTR bool mInitialized {false};
  RTC_DATA_ATTR MeasureRate mCurrentRate {};
};

Touch::Touch(TouchSettings& settings) : mSettings{settings} {
};

void Touch::initialize() {
  touch_pad_init();
  touch_pad_set_voltage(TOUCH_HVOLT_2V4, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);

  // Touching increases "C" reducing the measure
  // touch_pad_set_trigger_mode(TOUCH_TRIGGER_BELOW); // By default
  // touch_pad_intr_enable();
  // touch_pad_denoise_disable();
  touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
#if (HW_VERSION >= 10)
  touch_pad_fsm_start();
#endif

  for (auto p : HW::Touch::Pad) {
    auto pad = (touch_pad_t)p;
#if(HW_VERSION < 10)
    touch_pad_config(pad, 0);
#else
    touch_pad_config(pad);
    touch_ll_set_threshold(pad, 100000);
    touch_pad_sleep_channel_enable(pad, false); // We should not set sleep pads
#endif
  }
}

void Touch::setUp(bool onlyMenuLight) {
  if (!mInitialized) {
    mInitialized = true;
    initialize();
  }
#if(HW_VERSION >= 10)
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif

  const auto rate = mSettings.mRate[onlyMenuLight];
  if (rate == mCurrentRate)
    return;
  mCurrentRate = rate;

#if(HW_VERSION < 10)
  touch_ll_set_meas_time(mSettings.mCycles[onlyMenuLight] * 1024);
  touch_pad_set_measurement_interval((32 * 1024) / MeasureRate::_1s * rate);
#else
  touch_ll_set_meas_times(mSettings.mCycles[onlyMenuLight] * 1024);
  touch_pad_set_measurement_interval((4 * 1024) / MeasureRate::_1s * rate); //???
#endif

  auto setTouchPad = [&](auto&& v, bool enabled) {
    auto pad = (touch_pad_t)HW::Touch::Pad[mSettings.mMap[v]];
#if(HW_VERSION < 10)
    uint16_t th = enabled ? mSettings.mSensitivity : 0;
    touch_pad_clear_group_mask((1 << pad), (1 << pad), (1 << pad));
    if (enabled)
       touch_pad_set_group_mask((1 << pad), (1 << pad), (1 << pad));
#else
    uint16_t th = enabled ? 100 : 1000000;
#endif
    touch_ll_set_threshold(pad, th);
    // ESP_LOGE("setup", "%d, %d", pad, th);
  };
  setTouchPad(HW::Touch::BotR, !onlyMenuLight);
  setTouchPad(HW::Touch::TopR, !onlyMenuLight); 
  setTouchPad(HW::Touch::TopL, true);
  setTouchPad(HW::Touch::BotL, true);

  kDSState.lightPad = HW::Touch::Pad[mSettings.mMap[HW::Touch::BotL]];
}

void Touch::enable() const {
  esp_sleep_enable_touchpad_wakeup();
}

void Touch::disable() const {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TOUCHPAD);
}

Touch::Btn Touch::read() const {
  uint32_t mask;
  touch_ll_read_trigger_status_mask(&mask);
  // ESP_LOGE("mask", "%lu", mask);

  // Strange, but For S3 if the mask is unset, it is the last-pad
  if constexpr (HW::kChipType == HW_chips::ESP_32_S3) {
    if (mask == 0) {
      mask = 1 << HW::Touch::Pad.back();
    }
  }

  uint8_t bitmask {};
  for (auto i = 0; i < HW::Touch::Pad.size(); i++)
  {
    auto pad = HW::Touch::Pad[mSettings.mMap[i]];
    // uint16_t padLevel = touch_ll_read_raw_data((touch_pad_t)pad);
    // ESP_LOGE("val", "P%d; %d: %d", Power::current(), pad, padLevel);
    bitmask |= ((mask >> pad) & 1) << i;
  }
  return (Touch::Btn)bitmask;
}

std::vector<uint16_t> Touch::readAll() const {
  std::vector<uint16_t> res;
  for (auto i = 0; i < HW::Touch::Pad.size(); i++)
  {
    auto pad = HW::Touch::Pad[mSettings.mMap[i]];
    res.emplace_back(touch_ll_read_raw_data((touch_pad_t)pad));
  }
  return res;
}

void Touch::clear() const {
  touch_ll_clear_trigger_status_mask();
}

Touch::Btn Touch::readAndClear() const {
  auto btn = read();
  clear();
  return btn;
}