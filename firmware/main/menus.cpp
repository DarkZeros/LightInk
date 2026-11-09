#include "core.h"
#include "light.h"
#include "touch.h"
#include "peripherals.h"
#include "hardware.h"
#include "settings.h"
#include "ble.h"

#include <atomic>
#include <cstdio>

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

namespace {

// "hh:mm dd/mm" in local time, or "never"
std::string stamp(time_t utc, Time& time) {
  if (!utc)
    return "never";
  tmElements_t t;
  breakTime(utc + time.getMinutesWest() * 60, t);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d", t.Hour, t.Minute, t.Day, t.Month);
  return buf;
}

std::string bleStatus(Time& time) {
  auto& ble = kSettings.mBle;
  if (!ble.mPeer)
    return "\nNot paired";
  // 16 characters fit in a line
  return "\nPaired\nLast: " + std::string(magic_enum::enum_name(ble.mResult)) +
    "\nSync " + stamp(ble.mLastSync, time) +
    "\nSeen " + stamp(ble.mLastSeen, time);
}

const char* bleMessage(BleResult result) {
  switch (result) {
  case BleResult::Synced: return "Synced";
  case BleResult::Seen: return "Phone seen\nnothing new";
  case BleResult::NoPhone: return "Phone not\nfound\nIs the app\nrunning?";
  case BleResult::NeedsPair: return "Phone lost\nthe pairing\nPair again";
  case BleResult::Cancelled: return "Cancelled";
  default: return "Bluetooth\nfailed";
  }
}

} // namespace

Ble::Watch Core::bleWatch() {
  return {mTime.getTimeval().tv_sec, mBattery.mCurVoltage, mBattery.mCurPercent};
}

void Core::pairPhone() {
  mBleUsed = true;
  Cancel cancel{[this]{ return (mTouch.poll() & Touch::BACK) != 0; }};
  std::atomic<int32_t> code{-1};
  auto job = std::async(std::launch::async, [&]{
    return Ble::pair(bleWatch(), cancel, [&](uint32_t c){ code = c; });
  });
  progress("Pairing\nIn the app\ntap Pair\nBACK cancels");
  // The code comes from the radio task once the phone is found, the phone then asks
  // for it right away
  bool shown = false;
  while (job.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
    if (!shown && code >= 0) {
      shown = true;
      char msg[48];
      std::snprintf(msg, sizeof(msg), "Pair code\n%03ld %03ld\nType it on\nthe phone",
                    (long)code / 1000, (long)code % 1000);
      progress(msg);
    }
  }
  const auto result = job.get();
  if (result == BleResult::Cancelled)
    kSettings.mUi.mPopup.reset();
  else if (result == BleResult::Synced)
    kSettings.mUi.popup("Paired!");
  else
    kSettings.mUi.popup(shown ? "Pairing\nfailed" : bleMessage(result));
  regenerateMenus();
}

void Core::syncPhone() {
  mBleUsed = true;
  auto result = BleResult::Failed;
  bool changed = false;
  runCancellable("Syncing\nBACK cancels", [&](const Cancel& cancel) {
    result = Ble::syncNow(bleWatch(), cancel, changed);
  });
  if (result != BleResult::Cancelled)
    kSettings.mUi.popup(bleMessage(result));
  if (changed)
    regenerateMenus();
}

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
        // Each press takes the next sound and plays it, as it would sound at this hour
        UI::Optional{kSettings.mHourly.mBeep, UI::Indent{1, {
          UI::Action{std::string(magic_enum::enum_name(kSettings.mHourly.mSound)), [&]{
            auto& sound = kSettings.mHourly.mSound;
            const auto count = magic_enum::enum_count<Beep::Sound>();
            sound = magic_enum::enum_value<Beep::Sound>((magic_enum::enum_index(sound).value_or(0) + 1) % count);
            mTasks.emplace_back(std::async(std::launch::async, Beep::preview, mNow.Hour));
            regenerateMenus(); // For the name of the entry
          }},
        }}},
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
      UI::Loop<uint8_t>{"Style", kSettings.mWatchface.mType, 2},

      UI::Bool{"DEBUG MODE", kSettings.mWatchface.mDebug},

      UI::Bool{"Show Battery %", kSettings.mWatchface.mConfig.mBattery},
      UI::Bool{"Moon Phases", kSettings.mWatchface.mConfig.mMoon},
      UI::Bool{"Sunset/Sunrise", kSettings.mWatchface.mConfig.mSun},
      UI::Bool{"Tides", kSettings.mWatchface.mConfig.mTides},
    }},
    UI::Menu{"Display", {
      UI::Bool{"Invert", kSettings.mDisplay.mInvert },
      UI::Bool{"Border", kSettings.mDisplay.mDarkBorder },
      UI::Bool{"Flip 180", kSettings.mDisplay.mFlip},
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
      UI::Bool{"Beep", kSettings.mTouch.mBeep},
      UI::NumberRange<int8_t>{"Sensitivity", kSettings.mTouch.mSensitivity, {40, 80}},
      UI::Loop<MeasureRate>{"Menu Rate", kSettings.mTouch.mRate[0]},
      UI::Loop<MeasureRate>{"Watch Rate", kSettings.mTouch.mRate[1]},
    }},
    UI::Menu{"Bluetooth", {
      UI::Action{kSettings.mBle.mPeer ? "Pair again" : "Pair phone", [&]{ pairPhone(); }},
      UI::Action{"Sync now", [&]{ syncPhone(); }},
      UI::Loop<SyncEvery>{"Auto", kSettings.mBle.mEvery},
      UI::Action{"Forget phone", [&]{
        Ble::forget();
        kSettings.mUi.popup("Phone\nforgotten");
        regenerateMenus();
      }},
      UI::Text{[&]{ return bleStatus(mTime); }},
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
        mTasks.emplace_back(std::async(std::launch::async, Beep::preview, mNow.Hour));
      }},
      UI::Action{"Tetris Sound", [&]{
        runCancellable("Playing Tetris\nBACK to cancel", Peripherals::tetris);
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
          if (mTouch.poll() != Touch::Btn::NONE)
            break;
        }
        mDisplay.setRefreshMode(oldMode);
      }},
      UI::Action{"Parallel All", [&]{
        runCancellable("Parallel test\nBACK to cancel", [](const Cancel& cancel){
          auto vibrate = std::async(std::launch::async, [&]{
            for (auto i=0; i<10 && !cancel; i++) {
              delay(1300);
              Peripherals::vibrator(std::vector<int>{70});
            }
          });
          auto light = std::async(std::launch::async, [&]{
            for (auto i=0; i<30 && !cancel; i++) {
              delay(1000);
              Light::toggle();
            }
            Light::off();
          });
          Peripherals::tetris(cancel);
        });
      }},
    }},
    UI::Menu{"Test2", {
      UI::Name{"Battery V "+std::to_string(mBattery.mCurVoltage)},
    }},
  }};
};
