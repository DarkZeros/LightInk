#include "time.h"
#include "sys/time.h"
#include "esp32-hal-log.h"
#include "esp_private/esp_clk.h"
#include "sdkconfig.h"

#include <algorithm>

void Time::readTime() {
    gettimeofday(&mTv, NULL);
    // Apply timezone manually only to Elements
    breakTime(mTv.tv_sec + getMinutesWest() * 60, mElements);
}

void Time::adjustTime(const timeval& time) {
    gettimeofday(&mTv, NULL); // redundant? Maybe 2ms drift?
    mTv.tv_sec += time.tv_sec;
    mTv.tv_usec += time.tv_usec;
    setTime(mTv);
}

void Time::adjustTime(const int32_t& seconds) {
    struct timeval time{seconds, 0};
    // ESP_LOGE("adjust", "%ld", seconds);
    adjustTime(time);
}

void Time::setTime(const time_t& seconds) {
    setTime(timeval{.tv_sec = seconds, .tv_usec = 0});
}

void Time::setTime(const tmElements_t& elements, bool raw) {
    setTime(makeTime(elements) - !raw * mSettings.mMinutesWest * 60);
}

void Time::setTime(const timeval tm) {
    gettimeofday(&mTv, NULL); // redundant? Maybe 2ms drift?
    // Store difference in drift
    if (auto& sync = mSettings.mSync) {
        sync->mDrift.tv_sec += tm.tv_sec - mTv.tv_sec;
        sync->mDrift.tv_usec += tm.tv_usec - mTv.tv_usec;
    }
    // Set it and update times
    settimeofday(&tm, NULL);
    readTime();
}

#if CONFIG_IDF_TARGET_ESP32
extern "C" void __real_esp_clk_slowclk_cal_set(uint32_t cal);
#endif

void Time::calSet() {
#if CONFIG_IDF_TARGET_ESP32
    // The wrapped one ignores IDF's calls, see sleep_wrap.cpp
    __real_esp_clk_slowclk_cal_set(mSettings.mCalibration);
#else
    esp_clk_slowclk_cal_set(mSettings.mCalibration);
#endif
}
void Time::calUpdate() {
    if (!mSettings.mSync) {
        mSettings.mSync.emplace(mTv, timeval{});
        return;
    }
    auto& sync = *mSettings.mSync;
    auto& time = sync.mTime;
    double elapsed = (mTv.tv_sec - time.tv_sec) + (mTv.tv_usec - time.tv_usec) * 0.000'001;
    double drift = sync.mDrift.tv_sec + sync.mDrift.tv_usec * 0.000'001;
    uint32_t cal = 1.0 * mSettings.mCalibration * elapsed / (elapsed - drift);

    // ESP_LOGE("cal", "drift %f elapsed %f, cal %lu", drift, elapsed, cal);
    // Update drift, time and calibration
    drift -= (1.f * cal - mSettings.mCalibration) / mSettings.mCalibration * (elapsed - drift);
    sync.mDrift.tv_sec = drift;
    drift -= sync.mDrift.tv_sec;
    sync.mDrift.tv_usec = drift * 1'000'000;
    time = mTv; // The drift is from the last sync to now
    mSettings.mCalibration = std::clamp(cal, kDefaultCalibration / 10, kDefaultCalibration * 10);

    calSet();
}
void Time::calReset() {
    mSettings.mSync.reset();
    mSettings.mCalibration = kDefaultCalibration;
    calSet();
}