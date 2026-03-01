
#pragma once

#include "battery.h"
#include "time.h"
#include "touch.h"
#include "watchface.h"
#include "display.h"

struct Settings {
    bool mTouchWatchDog {false};

    struct Hourly {
        bool mBeep {false};
        bool mVib {false};
        int8_t mFirst {8};
        int8_t mLast {22};
    } mHourly;

    struct Alarms {
        bool mEnabled {true};

    };
    std::array<Alarms, 5> mAlarms; // Max 5 alarms fttb

    UiSettings mUi;
    TimeSettings mTime;
    TouchSettings mTouch;
    BatterySettings mBattery;
    DisplaySettings mDisplay;
    WatchfaceSettings mWatchface;

    struct Power {
        bool mNight {false};
        bool mAuto {true};
        bool mLowVoltage {true};
    } mPower;
};
extern struct Settings kSettings;