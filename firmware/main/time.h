#pragma once

#include <time.h>
#include <TimeLib.h>

#include <optional>

// constexpr static uint32_t kDefaultCalibration{16'000'000};
// -137ppm, based on tests, better starting point
constexpr static uint32_t kDefaultCalibration{15'997'810};

struct TimeSync {
    timeval mTime;
    timeval mDrift;
};

struct TimeSettings {
    // uSeconds in Q13.19 fixed-point
    // 1/32768 * 2^19 = 16'000'000
    uint32_t mCalibration{kDefaultCalibration};

    int16_t mMinutesWest{0};
    std::optional<TimeSync> mSync;
};

/* This class handles the time
 * It has epoch / tmElement and can handle timezones
*/
class Time {
    TimeSettings& mSettings;

    timeval mTv;
    tmElements_t mElements;

public:
    // The calibration is reset by IDF on boot, set ours again before reading the time
    explicit Time(TimeSettings& settings) : mSettings{settings} {calSet(); readTime();};

    void readTime();
    void setTime(const timeval);
    // Wrappers
    void adjustTime(const timeval& time);
    void adjustTime(const int32_t& seconds);
    void setTime(const tmElements_t& elements, bool raw = false);
    void setTime(const time_t&);

    // Calibration
    void calSet();
    void calReset();
    void calUpdate();
    float getPpm() {return (mSettings.mCalibration * 1.f - kDefaultCalibration) / (kDefaultCalibration / 1'000'000); }

    const timeval& getTimeval() const { return mTv; }
    const tmElements_t& getElements() const { return mElements; }
    int16_t& getMinutesWest() const { return mSettings.mMinutesWest; }
};