
#pragma once

#include "battery.h"
#include "beep.h"
#include "ble.h"
#include "time.h"
#include "touch.h"
#include "watchface.h"
#include "display.h"

struct Settings {
    bool mTouchWatchDog {false};

    struct Hourly {
        bool mBeep {false};
        Beep::Sound mSound {Beep::Sound::Casio};
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
    BleSettings mBle;

    // Flash copy of the settings (settings_table.cpp): when save() last looked, and the
    // hash of what flash holds
    struct Store {
        time_t mLastCheck {0};
        uint32_t mSavedHash {0};
    } mStore;

    struct Power {
        bool mNight {false};
        bool mAuto {true};
        bool mLowVoltage {true};
    } mPower;
};
extern struct Settings kSettings;