#include "core.h"
#include "light.h"
#include "touch.h"
#include "peripherals.h"
#include "hardware.h"
#include "settings.h"
#include "ble.h"

std::vector<UI::Any> buildAlarms(Core& core) {
  std::vector<UI::Any> alarms;
  auto newAlarm = [&] -> std::optional<int> {
    for (auto i=0; i<kSettings.mAlarms.size(); i++) {
      auto& a = kSettings.mAlarms[i];
      if (!a.mEnabled)
        return i;
    }
    return std::nullopt;
  }();
  if (newAlarm) {
    alarms.emplace_back(UI::Action{"Add Alarm", [&, i=*newAlarm] {
      kSettings.mAlarms[i].mEnabled = true;
      core.regenerateMenus();
    }});
  }

  for (auto i=0; i<kSettings.mAlarms.size(); i++) {
    auto& a = kSettings.mAlarms[i];
    if (!a.mEnabled)
      continue;
    alarms.emplace_back(UI::Action{"Alarm "+std::to_string(i), [&] {
      a.mEnabled = false;
      core.regenerateMenus();
    }});
  }
  return alarms;
};

std::vector<UI::Any> buildRadios() {
  std::vector<UI::Any> radios;
  for (auto& group : kSignals) {
    UI::Menu menu{group.mName, {}};
    for (auto& [name, seq] : group.mSequences) {
      menu.items.emplace_back(UI::Action{name, [&]{ Radio::sendSignal(seq); }});
    }
    radios.push_back(menu);
  }
  return radios;
};

UI::Any Core::generateMenus() {
  return UI::Menu{"Main Menu", {
    UI::Menu{"Clock", {
      UI::DateTime{"Set DateTime", mTime},
      UI::Indent{1, {
        UI::Number{"UTC",
          []{ return kSettings.mTime.mMinutesWest / 60;},
          [](int v){ auto& west = kSettings.mTime.mMinutesWest;
                    west = std::clamp<int16_t>(west + v * 60, -12 * 60, 12 * 60);}
        },
      }},
      UI::Menu{"Calibration", {
        UI::Action{"Sync", [&]{ mTime.calUpdate(); }},
        UI::Action{"Reset", [&]{ mTime.calReset(); }},
        UI::Text{[&] -> std::string {
          if (!kSettings.mTime.mSync)
            return "\n Not calibrated\n Set Date/Time\n then press Sync";
          auto& sync = *kSettings.mTime.mSync;
          tmElements_t last;
          breakTime(sync.mTime.tv_sec, last);
          char lastTime[32];
          std::sprintf(lastTime, "\n  %02d:%02d:%02d\n  %02d/%02d/%04d", last.Hour, last.Minute, last.Second, last.Day, last.Month, last.Year + 1970);
          auto elapsed = mTime.getTimeval().tv_sec - sync.mTime.tv_sec;
          auto actuallyElapsed = elapsed - kSettings.mTime.mSync->mDrift.tv_sec;
          bool megasec = actuallyElapsed > 1'000'000;
          char ppm[10];
          std::sprintf(ppm, "%+.2f", mTime.getPpm());

          return "\n Last sync on:" + std::string(lastTime) +
            "\n Err: " + std::to_string(kSettings.mTime.mSync->mDrift.tv_sec) +
            "/" + std::to_string(actuallyElapsed / (megasec ? 1'000'000 : 1)) + (megasec ? "M" : "") +
            "\n PPM:" + std::string(ppm) +
            "\n Cal:" + std::to_string(kSettings.mTime.mCalibration);
        }},
      }},
      UI::Menu{"Hour Beep", {
        UI::Bool{"Beep", kSettings.mHourly.mBeep },
        UI::Bool{"Vibrate", kSettings.mHourly.mVib },
        UI::NumberRange<int8_t>{"First Hour", kSettings.mHourly.mFirst, {0, 24} },
        UI::NumberRange<int8_t>{"Last Hour", kSettings.mHourly.mLast, {0, 24} },
      }},
      UI::Menu{"Alarms", buildAlarms(*this)},
      UI::Action{"NTP", [&]{ NTPSync(); }},
      UI::Action{(HW::kHasGps ? (mGps.isOn() ? "GPS Off" : "GPS resync") : "No GPS"), [&]{
        if constexpr (!HW::kHasGps) {
          return;
        }
        if (mGps.isOn()) {
          mGps.off();
          mGps.mData.mLocation = Gps::Data::Location{.mLat=51.438412, .mLon=-0.511787};
        } else {
          mGps.mData.mLocation.reset();
          mGps.on();
        }
        regenerateMenus();
      }},
    }},
    UI::Menu{"Watchface", {
      // UI::Loop<int>{"Style",
      //     []() -> int { return kSettings.mWatchface.mType; },
      //     [](){ kSettings.mWatchface.mType = (kSettings.mWatchface.mType + 1) % 4; }
      // },
      UI::Bool{"DEBUG MODE", kSettings.mWatchface.mDebug},

      UI::Bool{"Show Battery %", kSettings.mWatchface.mConfig.mBattery},
      UI::Bool{"Moon Phases", kSettings.mWatchface.mConfig.mMoon},
      UI::Bool{"Sunset/Sunrise", kSettings.mWatchface.mConfig.mSun},
      UI::Bool{"Tides", kSettings.mWatchface.mConfig.mTides},
    }},
    UI::Menu{"Display", {
      UI::Bool{"Invert", kSettings.mDisplay.mInvert },
      UI::Bool{"Border", kSettings.mDisplay.mDarkBorder },
      UI::Loop<uint8_t>{"Rotation", kSettings.mDisplay.mRotation, 4},
      UI::Loop<DisplayMode>{"Menu Lut", kSettings.mDisplay.mMenuLut },
      UI::Loop<DisplayMode>{"Watch Lut", kSettings.mDisplay.mWatchLut },
    }},
    UI::Menu{"Power", {
      UI::Bool{"Night (0-6am)", kSettings.mPower.mNight },
      UI::Bool{"Auto (<25%)", kSettings.mPower.mAuto },
      UI::Bool{"Low Voltage", kSettings.mPower.mLowVoltage },
    }},
    UI::Menu{"Touch", {
      UI::Bool{"Haptic", kSettings.mTouch.mHaptic},
      UI::NumberRange<int8_t>{"Sensitivity", kSettings.mTouch.mSensitivity, {40, 80}},
      UI::Loop<MeasureRate>{"Menu Rate", kSettings.mTouch.mRate[0]},
      UI::Loop<MeasureRate>{"Watch Rate", kSettings.mTouch.mRate[1]},
    }},
    UI::Menu{"Radio", buildRadios()},
    UI::Menu{"Test", {
      UI::Action{"Vib 2x75ms", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Peripherals::vibrator(std::vector<int>{75,75,75});
        }));
      }},
      UI::Action{"Vib 200ms", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Peripherals::vibrator(std::vector<int>{200});
        }));
      }},
      UI::Action{"Beep", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Peripherals::speaker(
            std::vector<std::pair<int,int>>{
            {3200,100},{0,100},{3200,100}});
        }));
      }},
      UI::Action{"Tetris Sound", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Peripherals::tetris();
        }));
      }},
      UI::Action{"BLE Test", [&]{
        // extern void ble_main(void);
        mTasks.emplace_back(std::async(std::launch::async, []{
          // ble_main();
          delay(50'000);
        }));
      }},
      UI::Action{"Lora Start Rcv", [&]{
        mTasks.emplace_back(std::async(std::launch::async, [&]{
          mRadio.startReceive();
        }));
      }},
      UI::Action{"Light toggle", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Light::toggle();
        }));
      }},
      UI::Action{"Display Restore", [&]{
        auto oldMode = mDisplay.getRefreshMode();
        // mDisplay.setRefreshMode(DisplayMode::REPAIR);
        bool inverted = false;
        while (true) {
          mDisplay.setInverted(inverted = !inverted);
          // Write some random patterns
          for (int16_t y = 0; y < mDisplay.height(); y++) {
            for (int16_t x = 0; x < mDisplay.width(); x++) {
              // Random black or white
              mDisplay.drawPixel(x, y, random(2));
            }
          }
          mDisplay.writeAllAndRefresh();
          if (mTouch.readAndClear() != Touch::Btn::NONE)
            break;
        }
        mDisplay.setRefreshMode(oldMode);
      }},
      UI::Action{"Parallel All", [&]{
        mTasks.emplace_back(std::async(std::launch::async, []{
          Peripherals::tetris();
        }));
        mTasks.emplace_back(std::async(std::launch::async, []{
          for (auto i=0; i<10; i++) {
            delay(1300);
            Peripherals::vibrator(std::vector<int>{70});
          }
        }));
        mTasks.emplace_back(std::async(std::launch::async, []{
          for (auto i=0; i<30; i++) {
            delay(1000);
            Light::toggle();
          }
        }));
      }},
    }},
    UI::Menu{"Test2", {
      UI::Name{"Battery V "+std::to_string(mBattery.mCurVoltage)},
    }},
  }};
};
