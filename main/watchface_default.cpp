#include "core.h"
#include "settings.h"
#include "watchface_default.h"

#include <Fonts/FreeMonoBold9pt7b.h>
#include "fonts/DSEG7_Classic_Bold_53.h"
#include "fonts/Seven_Segment10pt7b.h"
#include "fonts/DSEG7_Classic_Regular_15.h"
#include "fonts/DSEG7_Classic_Bold_25.h"
#include "fonts/DSEG7_Classic_Regular_39.h"
#include "icons.h"

#include <sunset.h>

double JulianDateFromUnixTime(uint64_t t){
	//Not valid for dates before Oct 15, 1582
	return (t / 86'400.0) + 2440587.5;
}

double mod360(double d){
    double t = std::fmod(d, 360.0);
    if(t<0){t+=360;}
    return t;
}

double getIlluminatedFractionOfMoon(uint64_t t){
    const double jd = JulianDateFromUnixTime(t);
    const double toRad = PI/180.0;
    const double T=(jd-2451545)/36525.0;

    const auto D = mod360(297.8501921 + 445267.1114034*T - 0.0018819*T*T + 1.0/545868.0*T*T*T - 1.0/113065000.0*T*T*T*T)*toRad; //47.2
    const auto M = mod360(357.5291092 + 35999.0502909*T - 0.0001536*T*T + 1.0/24490000.0*T*T*T)*toRad; //47.3
    const auto Mp = mod360(134.9633964 + 477198.8675055*T + 0.0087414*T*T + 1.0/69699.0*T*T*T - 1.0/14712000.0*T*T*T*T)*toRad; //47.4

    //48.4
    const auto i=mod360(180 - D*180/PI - 6.289 * sin(Mp) + 2.1 * sin(M) -1.274 * sin(2*D - Mp) -0.658 * sin(2*D) -0.214 * sin(2*Mp) -0.11 * sin(D))*toRad;

    const auto k=(1 + cos(i))/2;
    if (i > PI)
      return 100-k*50;
    // ESP_LOGE("moon", "r %f, i %f k %f, t %lld, jd %f", r, i, k, t, jd);
    return k * 50;
}

std::vector<Rect> DefaultWatchface::render() {
  std::vector<Rect> rects;

  auto& config = mSettings.mWatchface.mConfig;
  auto& mTime = mCore.mTime;
  auto& mNow = mCore.mNow;
  auto& mBattery = mCore.mBattery;

  auto& last = mSettings.mWatchface.mLastDraw;
  const auto& redraw = !last.mValid;
  struct Store {
    uint8_t mHour{};
    struct Date {
      uint8_t mYear{}, mMonth{}, mDay{};
      auto operator<=>(const Date&) const = default;
    } mDate;
    uint16_t mBattery{};
  };
  Store& store = *reinterpret_cast<Store*>(last.mStore);
  assert(sizeof(Store) < sizeof(last.mStore));

  // Minute separator
  if (redraw) {
    mDisplay.setFont(&DSEG7_Classic_Bold_53);
    mDisplay.setCursor(92,73);
    mDisplay.print(':');
    rects.emplace_back(92, 30, 12, 35);
  }

  // Hour
  if (redraw || store.mHour != mNow.Hour) {
    store.mHour = mNow.Hour;
    mDisplay.setFont(&DSEG7_Classic_Bold_53);
    mDisplay.setCursor(4, 73);
    mDisplay.printf("%02d", mNow.Hour);
    rects.emplace_back(7, 20, 80, 53);
  }

  // Moon
  if (config.mMoon) { // FIXME: Always update? Rarely changes
    constexpr auto center = std::pair{50, 150};
    constexpr auto radius = 30;

    bool color = mSettings.mConst.mDisplay.mInvert;
    auto frac = getIlluminatedFractionOfMoon(mTime.getTimeval().tv_sec);
    mDisplay.drawMoonFast(frac, center.first, center.second, radius, !color, color);
    // ESP_LOGE("frac", "%f", frac);

    mDisplay.drawCircle(center.first, center.second, radius, 1);
    mDisplay.setFont(NULL);
    mDisplay.setCursor(40, 185);
    mDisplay.printf("%.2f", frac);

    rects.emplace_back(center.first - radius - 5, center.second - radius - 5, (radius + 1) * 2 + 10, radius * 2 + 25);
  }

  // Sun
  if (config.mSun) { // FIXME: Always update? Rarely changes
    mDisplay.drawCircleHelper(145, 170, 35, 0b11, 1);

    auto& location = mCore.mGps.mData.mLocation;
    if (location)
    {
      SunSet sunset(location->mLat, location->mLon, mSettings.mConst.mTime.mMinutesWest / 60.0);
      auto& elements = mTime.getElements();
      sunset.setCurrentDate(elements.Year + 1970, elements.Month, elements.Day);
      uint16_t rise = sunset.calcSunrise() + 0.5; // For the rounding
      uint16_t set = sunset.calcSunset() + 0.5; // For the rounding
      // Current % in the day
      float perc = 1.f * ((elements.Hour * 60 + elements.Minute) - rise) / (set - rise);
      // ESP_LOGE("perc", "%f", perc);

      mDisplay.setFont(NULL);
      mDisplay.setCursor(110, 175);
      mDisplay.printf("%02d:%02d", rise / 60, rise % 60);
      mDisplay.setCursor(155, 175);
      mDisplay.printf("%02d:%02d", set / 60, set % 60);

      if (perc > 0 && perc < 1)
        mDisplay.fillCircle(145 - 35 * cos(perc * PI), 170 - 35 * sin(perc * PI), 5, 1);
    } else {
      mDisplay.setFont(NULL);
      mDisplay.setCursor(110, 175);
      mDisplay.printf("No GPS for Sun");
    }

    rects.emplace_back(100, 120, 95, 70);
  }

  // Date
  if (redraw || store.mDate != Store::Date{mNow.Year, mNow.Month, mNow.Day}) {
    store.mDate = Store::Date{mNow.Year, mNow.Month, mNow.Day};
    
    mDisplay.setFont(&Seven_Segment10pt7b);
    constexpr auto x = 17, y = 97;
    mDisplay.setCursor(x, y);

    String dayOfWeek = dayShortStr(mNow.Wday);
    mDisplay.setCursor(x, y);
    mDisplay.println(dayOfWeek);

    String month = monthShortStr(mNow.Month);
    mDisplay.setCursor(x + 70, y);
    mDisplay.println(month);

    mDisplay.setFont(&DSEG7_Classic_Regular_15);
    mDisplay.setCursor(x + 40, y+1);
    if(mNow.Day < 10){
        mDisplay.print('0');
    }
    mDisplay.println(mNow.Day);
    mDisplay.setCursor(x + 110, y+1);
    mDisplay.println(tmYearToCalendar(mNow.Year));// offset from 1970, since year is stored in uint8_t

    rects.emplace_back(0, 80, 200, 20);
  }

  // Battery
  if (config.mBattery && (redraw || store.mBattery != mBattery.mCurPercent)) {
    store.mBattery = mBattery.mCurPercent;
    mDisplay.setFont(NULL);
    mDisplay.setCursor(85, 105);
    mDisplay.setTextSize(1);
    mDisplay.printf("%.1f%%", mBattery.mCurPercent * 0.1);
    rects.emplace_back(75, 102, 45, 14);
  }

  // Lora!
  if (auto& pck = mCore.mRadio.mPck) {
    // Draw a white box
    mDisplay.writeFillRect(20, 90, 160, 70, 0);
    mDisplay.drawRect(22, 91, 158, 68, 1);
    mDisplay.setFont(NULL);
    mDisplay.setCursor(27, 98);
    mDisplay.printf("Message: %s\n", pck->mData.c_str());
    mDisplay.setCursor(27, mDisplay.getCursorY());
    mDisplay.printf("SNR: %f\n", pck->mSNR);
    mDisplay.setCursor(27, mDisplay.getCursorY());
    mDisplay.printf("RSSI: %f\n", pck->mRSSI);
    mDisplay.setCursor(27, mDisplay.getCursorY());
    mDisplay.printf("Freq: %f\n", pck->mFreqError);
    rects.emplace_back(20, 90, 160, 70);
  }

  return rects;
}

void DefaultWatchface::drawD(uint8_t d){
  mDisplay.setFont(&DSEG7_Classic_Bold_53);
  mDisplay.setCursor(104, 73);
  mDisplay.println(d);
}

void DefaultWatchface::drawU(uint8_t d){
  mDisplay.setFont(&DSEG7_Classic_Bold_53);
  mDisplay.setCursor(148, 73);
  mDisplay.println(d);
}