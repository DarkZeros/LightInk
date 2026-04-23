#pragma once

#include <future>
#include <vector>

#include "spi.h"
#include "gps.h"
#include "radio.h"
#include "display.h"
#include "watchface.h"
#include "battery.h"
#include "secrets.h"
#include "input.h"
#include "touch.h"
#include "buttons.h"
#include "ui.h"
#include "secrets.h"
#include "time.h"
#include "hardware.h"

/* This is the primary class of the project.
 * It has the entry point from deepsleep as well as all
 * the code that handles the setup/menus/misc.
 */
class Core {
    std::vector<std::future<void>> mTasks;
    std::optional<uint32_t> mNextUpdate;

public:
    Core();

    void handleTouch();
    const UI::Any& findUi();
    UI::Any generateMenus();
    void regenerateMenus() { mUi.emplace(generateMenus()); };
    void setNextUpdate(uint32_t seconds);
    void finishTasks();

    bool NTPSync();

    Time mTime;
    Battery mBattery;
    const bool mFirstTimeBoot;
    Spi mSpi;
    Radio mRadio;
    Display mDisplay;
    Gps mGps;

    // Input selection based on hardware configuration (compile-time)
    std::conditional_t<HW::kInputType == InputType::TOUCH, Touch, ButtonInput> mInput;

    const tmElements_t& mNow;
    std::optional<UI::Any> mUi; // optional to allow re-generation of complex type

};
