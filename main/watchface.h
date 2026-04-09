#pragma once

#include <TimeLib.h>
#include "display.h"
#include "diff_display.h"

// ============================================================
// WatchfaceState — the data the core provides to each draw call.
// The core fills in the common fields; the watchface only reads them.
// ============================================================
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
};

class Core;
class Settings;

// ============================================================
// Watchface — abstract base class.
// Subclasses implement draw() to render a complete frame.
// The core calls draw() with prev=nullptr for the very first
// render (full redraw required), and with a valid prev pointer
// for subsequent frames so the watchface can skip unchanged parts.
// ============================================================
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
    //   prev == nullptr  → first draw, render everything.
    //   prev != nullptr  → incremental draw, only re-render what changed.
    // The watchface must NOT call refresh(); the core handles that.
    virtual void draw(Display& gfx,
                      const WatchfaceState* prev,
                      const WatchfaceState& next) = 0;
};
