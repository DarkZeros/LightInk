#pragma once

#include <functional>
#include <future>
#include <string_view>
#include <vector>

#include "spi.h"
#include "gps.h"
#include "radio.h"
#include "display.h"
#include "watchface.h"
#include "battery.h"
#include "secrets.h"
#include "touch.h"
#include "ui.h"
#include "secrets.h"
#include "time.h"
#include "peripherals.h"

/* This is the primary class of the project.
 * It has the entry point from deepsleep as well as all
 * the code that handles the setup/menus/misc.
 */
class Core {
    std::vector<std::future<void>> mTasks;

public:
    Core();

    void handleTouch();
    void draw();
    // Popup for blocking work, drawn right away. The final message goes through
    // kSettings.mUi.popup() instead: the draw at the end of the wake shows it
    void progress(std::string_view msg);
    // Shows msg while job runs in the background, BACK sets its Cancel (seen the next
    // time the job checks it). Returns with the popup removed once the job has ended
    void runCancellable(std::string_view msg, std::function<void(const Cancel&)> job);
    const UI::Any& findUi();
    UI::Any generateMenus();
    void regenerateMenus() { mUi.emplace(generateMenus()); };
    void setNextUpdate(uint32_t seconds);
    void finishTasks();

    bool NTPSync();
    // Bluetooth menu flows, blocking, BACK cancels
    void pairPhone();
    void syncPhone();
    Ble::Watch bleWatch();

    Time mTime;
    Battery mBattery;
    const bool mFirstTimeBoot;
    Spi mSpi;
    Radio mRadio;
    Display mDisplay;
    Gps mGps;
    Touch mTouch;

    const tmElements_t& mNow;
    std::optional<UI::Any> mUi; // optional to allow re-generation of complex type

    std::optional<uint32_t> mNextUpdate;
    bool mBleUsed {false}; // A menu flow used the radio, no background sync this wake
};