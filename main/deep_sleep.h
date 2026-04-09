#pragma once

#include <inttypes.h>
#include <magic_enum.hpp>

#include "display.h"
#include "diff_display.h"
#include "hardware.h"

static constexpr auto kMaxDeltaData = 1 * 1024;

struct DeepSleepState {
  // This is needed because the HW pin is not properly wired to a RTC GPIO
  [[no_unique_address]] struct BusyWait {
    constexpr static auto kStartWait = 25'000u; // Super low value to start with
    constexpr static auto kReduce = 500u;
    constexpr static auto kWaitStep = 100u;

    uint32_t currentWait {kStartWait};
    uint8_t missedTimes {0};
  } busyWait[magic_enum::enum_count<DisplayMode>() * !HW::kHasDisplayBusyWake];

  // Display minute update variables
  uint8_t currentMinutes {0};
  uint8_t stepSize {1};
  int8_t  minutes {10};
  bool    displayBusy {false};

  // Light
  uint8_t lightPad {0};
  
  // Pre-computed delta frames for the wake stub to apply.
  uint32_t mDeltaOffset {0};
  uint8_t mDeltasLeft {0};
  uint8_t mDeltaData[kMaxDeltaData];

};

extern struct DeepSleepState kDSState;

extern void wake_stub_deepsleep(void);
