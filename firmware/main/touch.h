#pragma once

#include <array>
#include <cstdint> 

#include <magic_enum.hpp>

#include "hardware.h"

#include "esp_attr.h"
#include "hal/touch_sensor_ll.h"

// Mask of the pads that triggered, safe to use from the wake stub.
// The ESP32 reports every measured pad, even the ones not enabled
// for wakeup (worken), so filter those out.
FORCE_INLINE_ATTR uint32_t touchTriggerMask() {
#if CONFIG_IDF_TARGET_ESP32
  return TOUCH_LL_BITS_SWAP(SENS.sar_touch_ctrl2.touch_meas_en & SENS.sar_touch_enable.touch_pad_worken);
#else
  uint32_t mask;
  touch_ll_read_trigger_status_mask(&mask);
  return mask;
#endif
}

enum MeasureRate {
    // _125ms = 1, // Extremely fast
    _250ms = 2,
    _500ms = 4,
    _1s = 8,
    _2s = 15, // 16 would overflow
    // _4s = 32, // Not possible, limit is 2s
};

enum MeasureCycles {
    _31ms = 1,  // 50-60 -> 30-35 // 33-40 -> 20-25
    _62ms = 2,  // 110 -> 60
};

struct TouchSettings {
  // More cycles more accurate, and more power
  // More often checks, also more power
  MeasureCycles mCycles[2] = {_31ms, _31ms};
  MeasureRate mRate[2] = {_250ms, _1s};
  bool mHaptic {false};
  int8_t mSensitivity {60};
  std::array<uint8_t, 4> mMap{{0,1,2,3}};

  struct Setup {
    bool mMode : 1 {false};
    bool mPower : 1 {false};
    auto operator<=>(const Setup&) const = default;
  };
  std::optional<Setup> mSetup{};
};

class Touch {
private:
    TouchSettings& mSettings;
public:
    explicit Touch(TouchSettings& settings);

    void initialize();
    void setUp(bool onlyMenuLight);

    enum Btn {
        NONE = 0,
        DOWN = 0b0001,
        UP   = 0b0010,
        MENU = 0b0100,
        BACK = 0b1000,
        // Alias
        LIGHT = BACK,
    };
    std::vector<uint16_t> readAll() const;
    Btn read() const; // The press that woke us
    void clear() const;
    Btn readAndClear() const;
    Btn poll() const; // Presses since the last clear, for use while awake. Also clears

    void enable() const;
    void disable() const;

    // While one is alive the pads are measured only by measureNow(), not by the touch
    // FSM timer. On the PICO-D4 each FSM measurement stalls RC_FAST for up to ~0.1 ms,
    // which clicks a playing tone. Counted, the last one restores the timer. ESP32 only
    struct Manual {
        Manual();
        ~Manual();
    };
    // Measures all pads once (<1 ms, blocks) and updates the status poll() reads.
    // Does nothing unless a Manual is alive, or if the last one was less than the
    // measure rate (mRate) ago
    static void measureNow();

private:
    Btn toBtn(uint32_t mask) const;
};