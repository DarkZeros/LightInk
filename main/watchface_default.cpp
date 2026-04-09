#include "core.h"
#include "settings.h"
#include "watchface_default.h"
#include "trace.h"

#include <Fonts/FreeMonoBold9pt7b.h>
#include "fonts/DSEG7_Classic_Bold_53.h"
#include "fonts/Seven_Segment10pt7b.h"
#include "fonts/DSEG7_Classic_Regular_15.h"
#include "fonts/DSEG7_Classic_Bold_25.h"
#include "fonts/DSEG7_Classic_Regular_39.h"
#include "icons.h"

#include <sunset.h>

// ============================================================
// Moon phase helpers (unchanged from original)
// ============================================================

static double JulianDateFromUnixTime(uint64_t t) {
    return (t / 86'400.0) + 2440587.5;
}

static double mod360(double d) {
    double t = std::fmod(d, 360.0);
    if (t < 0) t += 360;
    return t;
}

static double getIlluminatedFractionOfMoon(uint64_t t) {
    const double jd = JulianDateFromUnixTime(t);
    const double toRad = PI / 180.0;
    const double T = (jd - 2451545) / 36525.0;

    const auto D  = mod360(297.8501921 + 445267.1114034 * T - 0.0018819 * T * T
                           + 1.0 / 545868.0 * T * T * T
                           - 1.0 / 113065000.0 * T * T * T * T) * toRad;
    const auto M  = mod360(357.5291092 + 35999.0502909 * T - 0.0001536 * T * T
                           + 1.0 / 24490000.0 * T * T * T) * toRad;
    const auto Mp = mod360(134.9633964 + 477198.8675055 * T + 0.0087414 * T * T
                           + 1.0 / 69699.0 * T * T * T
                           - 1.0 / 14712000.0 * T * T * T * T) * toRad;

    const auto i = mod360(180 - D * 180 / PI
                          - 6.289 * sin(Mp) + 2.1 * sin(M)
                          - 1.274 * sin(2 * D - Mp) - 0.658 * sin(2 * D)
                          - 0.214 * sin(2 * Mp) - 0.11 * sin(D)) * toRad;

    const auto k = (1 + cos(i)) / 2;
    if (i > PI)
        return 100 - k * 50;
    return k * 50;
}

// ============================================================
// DefaultWatchface::draw
// ============================================================

void DefaultWatchface::draw(Display& gfx,
                             const WatchfaceState* prev,
                             const WatchfaceState& next) {
    TRACE("watchface_default_draw");

    auto& config = mWatchfaceSettings.mConfig;
    const bool redraw = (prev == nullptr);

    // Helper: did a field change between prev and next?
    auto changed = [&](auto accessor) -> bool {
        return redraw || (accessor(*prev) != accessor(next));
    };

    // ---- Minute separator (colon) ----
    if (redraw) {
        gfx.setFont(&DSEG7_Classic_Bold_53);
        gfx.setCursor(92, 73);
        gfx.print(':');
    }

    // ---- Hour ----
    if (changed([](const WatchfaceState& s) { return s.mTime.Hour; })) {
        gfx.setFont(&DSEG7_Classic_Bold_53);
        gfx.setCursor(4, 73);
        gfx.printf("%02d", next.mTime.Hour);
    }

    // ---- Minute (tens digit) ----
    if (changed([](const WatchfaceState& s) { return uint8_t(s.mTime.Minute / 10); })) {
        gfx.setFont(&DSEG7_Classic_Bold_53);
        gfx.setCursor(104, 73);
        gfx.print(next.mTime.Minute / 10);
    }

    // ---- Minute (units digit) ----
    if (changed([](const WatchfaceState& s) { return uint8_t(s.mTime.Minute % 10); })) {
        gfx.setFont(&DSEG7_Classic_Bold_53);
        gfx.setCursor(148, 73);
        gfx.print(next.mTime.Minute % 10);
    }

    // ---- Moon ----
    if (config.mMoon) {
        // Moon phase changes slowly; always redraw for simplicity
        bool color = mSettings.mDisplay.mInvert; // mSettings is the full Settings struct
        auto frac = getIlluminatedFractionOfMoon(mCore.mTime.getTimeval().tv_sec);
        gfx.drawMoonFast(frac, 50, 150, 30, !color, color);

        constexpr auto center = std::pair{50, 150};
        constexpr auto radius = 30;
        gfx.drawCircle(center.first, center.second, 30, 1);
        gfx.setFont(NULL);
        gfx.setCursor(40, 185);
        gfx.printf("%.2f", frac);
    }

    // ---- Sun ----
    if (config.mSun) {
        gfx.drawCircleHelper(145, 170, 35, 0b11, 1);

        auto& location = mCore.mGps.mData.mLocation;
        if (location) {
            SunSet sunset(location->mLat, location->mLon,
                          mCore.mTime.getMinutesWest() / 60.0);
            auto& elements = next.mTime;
            sunset.setCurrentDate(elements.Year + 1970, elements.Month, elements.Day);
            uint16_t rise = sunset.calcSunrise() + 0.5f;
            uint16_t set  = sunset.calcSunset()  + 0.5f;
            float perc = 1.f * ((elements.Hour * 60 + elements.Minute) - rise) / (set - rise);

            gfx.setFont(NULL);
            gfx.setCursor(110, 175);
            gfx.printf("%02d:%02d", rise / 60, rise % 60);
            gfx.setCursor(155, 175);
            gfx.printf("%02d:%02d", set / 60, set % 60);

            if (perc > 0 && perc < 1)
                gfx.fillCircle(145 - 35 * cos(perc * PI), 170 - 35 * sin(perc * PI), 5, 1);
        } else {
            gfx.setFont(NULL);
            gfx.setCursor(110, 175);
            gfx.printf("No GPS for Sun");
        }
    }

    // ---- Date ----
    auto dateChanged = [&]() -> bool {
        if (redraw) return true;
        return prev->mTime.Year  != next.mTime.Year  ||
               prev->mTime.Month != next.mTime.Month ||
               prev->mTime.Day   != next.mTime.Day;
    };
    if (dateChanged()) {
        gfx.setFont(&Seven_Segment10pt7b);
        constexpr auto x = 17, y = 97;

        String dayOfWeek = dayShortStr(next.mTime.Wday);
        gfx.setCursor(x, y);
        gfx.println(dayOfWeek);

        String month = monthShortStr(next.mTime.Month);
        gfx.setCursor(x + 70, y);
        gfx.println(month);

        gfx.setFont(&DSEG7_Classic_Regular_15);
        gfx.setCursor(x + 40, y + 1);
        if (next.mTime.Day < 10)
            gfx.print('0');
        gfx.println(next.mTime.Day);
        gfx.setCursor(x + 110, y + 1);
        gfx.println(tmYearToCalendar(next.mTime.Year));
    }

    // ---- Battery ----
    if (config.mBattery &&
        changed([](const WatchfaceState& s) { return s.mBattery; })) {
        gfx.setFont(NULL);
        gfx.setCursor(85, 105);
        gfx.setTextSize(1);
        gfx.printf("%.1f%%", next.mBattery * 0.1f);
    }

    // ---- LoRa packet ----
    if (auto& pck = mCore.mRadio.mPck) {
        gfx.writeFillRect(20, 90, 160, 70, 0);
        gfx.drawRect(22, 91, 158, 68, 1);
        gfx.setFont(NULL);
        gfx.setCursor(27, 98);
        gfx.printf("Message: %s\n", pck->mData.c_str());
        gfx.setCursor(27, gfx.getCursorY());
        gfx.printf("SNR: %f\n", pck->mSNR);
        gfx.setCursor(27, gfx.getCursorY());
        gfx.printf("RSSI: %f\n", pck->mRSSI);
        gfx.setCursor(27, gfx.getCursorY());
        gfx.printf("Freq: %f\n", pck->mFreqError);
    }
}
