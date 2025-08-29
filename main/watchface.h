#pragma once

#include "display.h"
#include "battery.h"
#include "time.h"
#include <TimeLib.h>
#include "ui.h"

struct WatchfaceSettings {
    uint8_t mType : 2 {0}; // Watchface type selected from the presets
    bool mDebug {false};

    struct Cache {
        get_arduino_panic_handler
    };


    // // Draw cache, try to only draw once the hour
    // // Also can be used to SPI transfer form deep_sleep wake stub
    // struct {
    //     // Rotation&watchface, invalidates cache if changes
    //     bool mDone : 1 {false};
    //     uint8_t mRotation : 2 {2};
    //     uint8_t mType : 2 {0};

    //     struct Units {
    //         Rect coord {}; 
    //         uint8_t data[270 * 10]{}; // 2700 B = 30%
    //     } mUnits;
    //     struct Decimal {
    //         Rect coord {};
    //         uint8_t data[270 * 6]{}; // 1620 B = 20%
    //     } mDecimal;
    // } mCache;

    // // Store information about the last draw, the most important one is
    // // if the draw was valid or not, there is 128 bytes for extra data for the watchfaces
    // struct {
    //     bool mValid{false}; // If the last draw was a watchface
    //     uint8_t mMinuteU[2]{}, mMinuteD{}; // Front and back buffers for Units
    //     uint8_t mStore[128]{}; // Scratch data the Watchfaces want to store
    // } mLastDraw;

    struct {
        // TODO Move me inside watchface belonging to
        bool mBattery {true};
        bool mMoon {true};
        bool mSun {true};
        bool mTides {false};
    } mConfig;
};

class Core;
class Settings;

/* This class handles the Draw of the display watchface
 * in an optimal way, caching elements to avoid redrawing
 */
class Watchface {
protected:
    // The watchface can access anything const
    struct {
        const Settings& mConst;
        WatchfaceSettings& mWatchface;
    } mSettings;
    const Core& mCore;
    Display& mDisplay;

    // Needs to implement minute uni/dec draw & return Rect coordinates
    virtual void drawU(uint8_t d);
    virtual void drawD(uint8_t d);
    virtual Rect rectU();
    virtual Rect rectD();

    // Can optionally implement Other element drawing based & return vect of rect
    virtual std::vector<Rect> render() { return {}; }

public:
    explicit Watchface(
        const Settings& settings,
        WatchfaceSettings& watchSet,
        const Core& core,
        Display& display)
    : mSettings{settings, watchSet}
    , mCore(core)
    , mDisplay(display)
    {}

    void draw();
    void updateCache();
};