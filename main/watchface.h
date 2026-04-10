#pragma once

#include <optional>
#include <TimeLib.h>
#include "diff_display.h"

struct WatchfaceState {
  tmElements_t mTime;     // Hour, Minute, Second, Day, Month, Year, Wday
  uint16_t     mBattery;  // battery percent * 10  (e.g. 1000 = 100.0%)
  // Extend here as needed (GPS location, radio packet, etc.)
};

// Settings that survive across boots (stored in RTC via Settings)
struct WatchfaceSettings {
  uint8_t mType : 2 {0}; // Watchface type selected from the presets
  bool    mDebug {false};
  struct {
    bool mBattery {true};
    bool mMoon    {true};
    bool mSun     {true};
    bool mTides   {false};
  } mConfig;
  std::optional<WatchfaceState> mLastState;
};

class Core;
class Settings;

class Watchface {
protected:
  const Settings&    mSettings;
  WatchfaceSettings& mWatchfaceSettings;
  const Core&        mCore;
  constexpr static uint8_t mainColor = 0xFF;
  constexpr static uint8_t backColor = 0x00;

public:
  explicit Watchface(const Settings& settings,
                     WatchfaceSettings& watchSet,
                     const Core& core)
      : mSettings(settings)
      , mWatchfaceSettings(watchSet)
      , mCore(core)
  {}
  virtual ~Watchface() = default;

  // Draw the watchface into `gfx`.
  // The watchface must NOT call refresh(); the core handles that.
  virtual void draw(Display& gfx,
                    const std::optional<WatchfaceState> prev,
                    const WatchfaceState& next) = 0;
};
