
#include <fmt/format.h>

#include "driver/rtc_io.h"

#include "core.h"
#include "deep_sleep.h"
#include "power.h"
#include "light.h"
#include "touch.h"
#include "peripherals.h"
#include "hardware.h"
#include "settings.h"

#include "watchface_default.h"

RTC_DATA_ATTR Settings kSettings;

auto wakeup_reason = esp_sleep_get_wakeup_cause();

Core::Core()
: mTime{kSettings.mTime}
, mBattery{kSettings.mBattery}
, mFirstTimeBoot{[&]{
    static RTC_DATA_ATTR bool sDone = false;
    if (sDone)
        return false;
    sDone = true;

    if constexpr (HW::kVersion < 10) {
        // Set all GPIOs to input that we are not using to avoid leaking power
        // This is NEEDED
        const uint64_t ignore = 0b11110001000000110000100111000010; // Ignore some GPIOs due to resets
        for (int i = 0; i < GPIO_NUM_MAX; i++) {
            if ((ignore >> i) & 0b1)
                continue;
            // ESP_LOGE("", "%d input", i);
            pinMode(i, INPUT);
        }
    }

    // For some reason, seems to be enabled on first boot
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // We can´t select the Power value, just cycle trough it once to set it off
    {
        auto _ = Power::Lock(Power::Flag::Display);
        Light::off();
    }
    
    // reset calibration to the ESP32
    mTime.calReset();
    mTime.getMinutesWest() = 60; // Default zone +1
    mTime.readTime();

    // Recover Settings from Disk // TODO
    // load NVS and load settings

    // Queue the rest of the first boot for later (GPS, LORA, NTP, Touch)
    mTasks.emplace_back(std::async(std::launch::deferred, [&]{
        // Trigger NTP, if wifi is available, it will set time
        NTPSync();

        // Try to get GPS location, to setup time/location
        if constexpr (HW::kHasGps) {
            if (!mGps.mData.mLocation)
                mGps.on();
        } else {
            mGps.off();
            // HACK: Set a fixed location to start with
            mGps.mData.mLocation = Gps::Data::Location{.mLat=51.438412, .mLon=-0.511787};
        }

        // Set up the touch, not enable it yet
        mTouch.setUp(kSettings.mUi.mDepth < 0);
    }));

    return true;
}()}
, mSpi{}
, mDisplay{}
, mTouch{kSettings.mTouch}
, mNow{mTime.getElements()}
, mUi{generateMenus()}
{
    // Finish pending tasks added during boot (before inputs/events)
    finishTasks();

    // Check GPS on a background task
    if (HW::kHasGps && mGps.isOn() && !mGps.mData.mLocation) {
        mTasks.emplace_back(std::async(std::launch::async, [&]{
            mGps.read();
            if (auto datetime = mGps.mData.mDateTime) {
                mTime.setTime(datetime->mElements, true);
                // Roughtly adjust the centiseconds
                mTime.adjustTime(timeval{.tv_sec=0, .tv_usec=datetime->mCentiSeconds * 10'000});
            }
            if (mGps.mData.mLocation || mGps.mData.mTimeOn >= Gps::kMaxTimeGpsOn) {
                mGps.off();
            } else {
                // Check every 10s the GPS. Uses 20mA power.
                // Pprefer to waste a few ms the ESP32 on, to early exit
                // Never have the GPS on more than 1 minute if it does not Adquire
                setNextUpdate(10);
                mGps.mData.mTimeOn = mGps.mData.mTimeOn.value_or(std::chrono::seconds(0)) + Gps::kGpsUpdateRate;
            }
        }));
    }

    // Wake up reason affects how to proceed
    ESP_LOGE("boot","reason %d", wakeup_reason);
    switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TOUCHPAD: { // Touch!
        if (kSettings.mTouch.mHaptic)
            mTasks.emplace_back(std::async(std::launch::async, []{
                Peripherals::vibrator(std::vector<int>{25});
            }));
        handleTouch();
    } break;
    case ESP_SLEEP_WAKEUP_TIMER: // Internal Timer
        // Check watchdog -> reset to watchFace
        if (kSettings.mTouchWatchDog) {
            kSettings.mUi.mDepth = -1; // Reset to watchFace
        }
        kSettings.mTouchWatchDog = true;
        break;
    case ESP_SLEEP_WAKEUP_EXT1: // Used for LoRa reception
        ESP_LOGE("lora", "ext1 wakeup");
        mRadio.readLoraPck();
        break;
    case ESP_SLEEP_WAKEUP_EXT0: // Used for display busy wakeup
        ESP_LOGE("ext0", "wakeup ?"); // Should never be hit
        break;
    default: // First time boot!
        ESP_LOGE("", "boot %lu unkown wakeup reason %d", micros(), wakeup_reason);
        break;
    }

    // Re-set up the touch if settings have changed
    mTouch.setUp(kSettings.mUi.mDepth < 0);

    // Beep conditions
    const bool hourUpdate = mNow.Minute == 0 && mNow.Second == 0 && wakeup_reason == ESP_SLEEP_WAKEUP_TIMER;
    if (hourUpdate
        && mNow.Hour >= kSettings.mHourly.mFirst
        && mNow.Hour <= kSettings.mHourly.mLast)
    {
        if (kSettings.mHourly.mBeep) {
            mTasks.emplace_back(std::async(std::launch::async, []{
                Peripherals::speaker(
                    std::vector<std::pair<int,int>>{
                    {3200,100},{0,100},{3200,500}
                });
            }));
        }
        if (kSettings.mHourly.mVib) {
            mTasks.emplace_back(std::async(std::launch::async, []{
                Peripherals::vibrator(std::vector<int>{50,75,50});
            }));
        }
    }

    // Common display preparations, post UI Events processing
    auto& disp = kSettings.mDisplay;
    mDisplay.setRotation(disp.mRotation);
    mDisplay.setDarkBorder(disp.mDarkBorder ^ disp.mInvert);
    mDisplay.setInverted(!disp.mInvert);
    mDisplay.setTextColor(0xFF);

    // Show watch face or menu ?
    if (kSettings.mUi.mDepth < 0) {
        mDisplay.setRefreshMode(kSettings.mDisplay.mWatchLut);
        #define ARGS kSettings, kSettings.mWatchface, *this, mDisplay
        // Instantiate the watchface type we are using
        switch(kSettings.mWatchface.mType) {
            default: DefaultWatchface(ARGS).draw(); break;
            // case 0: break;
            // case 1: break;
            // case 2: break;
            // case 3: break;
        }
        #undef ARGS
        Light::off(); // Always turn off light exiting the Menus
    } else {
        mDisplay.setRefreshMode(kSettings.mDisplay.mMenuLut);
        kSettings.mWatchface.mLastDraw.mValid = false;
        std::visit([&](auto& e){
            if constexpr (has_render<decltype(e), Display&>::value) {
                e.render(mDisplay);
            } else {
                mDisplay.setTextSize(3);
                mDisplay.setTextColor(1, 0);
                mDisplay.println("UNIMPLEMENTED");
                mDisplay.println("PRESS BACK");
            }
        }, findUi());
    }

    // Finish display & pending tasks, then setup touch
    mDisplay.hibernate();
    finishTasks();
    mTouch.enable();

    // Calculate stepsize based on battery level or on battery save mode
    auto stepSize = [&] {
        if (kSettings.mPower.mNight && mNow.Hour < 7)
            return 5;
        if (!kSettings.mPower.mAuto)
            return 1;
        if (mBattery.mCurPercent < 100) {
            return 5;
        } else if (mBattery.mCurPercent < 200) {
            return 4;
        } else if (mBattery.mCurPercent < 500) {
            return 2;
        }
        return 1;
    }();

    // Calc next full wake (alarms/NextUpdates) and how much to sleep
    auto nextFullWake = 60;
    auto secondsWait = 60 - mNow.Second;
    if (mNextUpdate) {
        auto seconds = *mNextUpdate;
        if (seconds < 60) {
            secondsWait = seconds;
            nextFullWake = mNow.Minute + 1; // Wake instantly
        } else {
            nextFullWake = mNow.Minute + seconds / 60;
        }
        kDSState.minutes = (*mNextUpdate + secondsWait) / 60;
        ESP_LOGE("Early Wake", "minutes %d seconds %d", kDSState.minutes, secondsWait);
    }
    auto firstMinutesSleep = stepSize - mNow.Minute % stepSize;
    auto nextPartialWake = firstMinutesSleep + mNow.Minute;
    // In case the step overflows, we need to chop it, and wake up earlier
    // kDSState.minutes will be exactly 0 after this trim
    if (nextPartialWake > nextFullWake)
        firstMinutesSleep -= nextPartialWake - nextFullWake;

    // ESP_LOGE("", "nextFullWake %d firstMinutesSleep %d nextPartialWake %d", nextFullWake, firstMinutesSleep, nextPartialWake);
    if constexpr (HW::kHasLora) {
        esp_sleep_enable_ext1_wakeup(1ULL << HW::Lora::Dio1, ESP_EXT1_WAKEUP_ANY_HIGH);
        // Hold some pins to some values to avoid leaking current
        rtc_gpio_hold_en((gpio_num_t)HW::Lora::Cs);
    }

    // We can only run wakeupstub when on watchface mode
    if (kSettings.mUi.mDepth < 0) {
        kDSState.currentMinutes = mNow.Minute + firstMinutesSleep;
        kDSState.minutes = nextFullWake - mNow.Minute - firstMinutesSleep;
        // ESP_LOGE("", "min %d step %d wait %ld", kDSState.minutes, stepSize, kDSState.updateWait);
        kDSState.stepSize = stepSize;
        // Only trigger the wakeupstub if there is more than 1 minute left
        if (kDSState.minutes > 0) {
            if constexpr (HW::kHasDisplayBusyWake)
                esp_sleep_enable_ext0_wakeup((gpio_num_t)HW::Display::Busy, 0);
            esp_set_deep_sleep_wake_stub(&wake_stub_deepsleep);
        }
        auto nextMinute = secondsWait * 1'000'000 - mTime.getTimeval().tv_usec;
        esp_sleep_enable_timer_wakeup(nextMinute + (firstMinutesSleep - 1) * 60'000'000);
    } else {
        // Disable the wake stub and count a fix time
        esp_set_deep_sleep_wake_stub(NULL);
        esp_sleep_enable_timer_wakeup(10'000'000);
    }

    // Who is setting this to ON manually ?? Display + LightSleep?
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);

    mTouch.clear(); // Clear the mask just before entering the sleep

    esp_deep_sleep_disable_rom_logging();
    esp_deep_sleep_start();
    ESP_LOGE("deepSleep", "never reach!");
}

const UI::Any& Core::findUi() {
    // Find current UI element in view by recursively finding deeper elements
    const UI::Any* item = &*mUi;
    for (auto i=0; i < kSettings.mUi.mDepth; i++) {
        auto& index = kSettings.mUi.mState[i];
        const UI::Any* nextItem = nullptr;
        std::visit([&](auto& e) {
            if constexpr (has_sub<decltype(e), uint8_t>::value) {
                nextItem = e.sub(index);
            }
        }, *item);
        if (nextItem == nullptr)
            break;
        std::visit([&](auto& e) {
            if constexpr (has_ref<decltype(e)>::value) {
                nextItem = &e.ref();
            }
        }, *nextItem);
        item = nextItem;
    }
    return *item;
}

void Core::handleTouch() {
    // Clear WatchDog since we received a valid touch
    kSettings.mTouchWatchDog = false;

    // Convert to what button was it
    Touch::Btn btn = mTouch.readAndClear();
    ESP_LOGE("parsed", "%d, %s", btn, std::string(magic_enum::enum_name(btn)).c_str());

    auto& ui = kSettings.mUi;
    ESP_LOGE("ui", "depth%d st%d", ui.mDepth, ui.mState[ui.mDepth]);

    // Button press on the watchface
    if (ui.mDepth < 0) {
        if (btn == Touch::LIGHT) {
            Light::toggle();
        } else if (btn == Touch::MENU) {
            ui.mDepth = 0;
        }
        return;
    }

    // Button press on the UI is sent to the current selected item
    auto& item = findUi();

    // Send the touch event to the UI element to handle it, or fallback
    std::visit([&]<typename E>(const E& e) {
        if constexpr (has_button<E>::value) {
            e.button(btn);
        } else {
            // If a generic button handler is not implemented, try the specific ones
            if (btn & Touch::BACK) {
                if constexpr (has_button_back<E>::value) {
                    e.button_back();
                } else {
                    // Default option is to go back in the UI
                    ui.mDepth--;
                }
            } else if (btn & Touch::MENU) {
                if constexpr (has_button_menu<E>::value) {
                    e.button_menu();
                }
            } else {
                if constexpr (has_button_updown<E, int>::value) {
                    e.button_updown(btn == Touch::DOWN ? -1 : 1);
                }
            }
        }
    }, item);
}

void Core::finishTasks() {
    for(auto& f : mTasks)
        f.get();
    mTasks.clear();
}

void Core::setNextUpdate(uint32_t seconds)
{
    mNextUpdate = std::min(mNextUpdate.value_or(-1), seconds);
}

#include <Arduino.h>
#include <WiFiManager.h>

bool connectWifi(uint32_t timeoutMs = 2000)
{
    WiFi.mode(WIFI_STA);
    for (auto& net : kWifiNetworks) {
        ESP_LOGI("WiFi", "Connecting to %s", net.mSsid.c_str());
        WiFi.begin(net.mSsid.c_str(), net.mPswd.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            ESP_LOGE("WiFi", "Connected: %s", net.mSsid.c_str());
            return true;
        }
        WiFi.disconnect(true);
    }
    return false;
}

#include "esp_sntp.h"

static bool sntpSynced = false;
void timeSyncCallback(struct timeval *tv)
{
    sntpSynced = true;
}

bool syncTime(uint32_t timeoutMs = 5000)
{
    sntpSynced = false;
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(timeSyncCallback);
    esp_sntp_init();

    uint32_t start = millis();

    while (!sntpSynced && millis() - start < timeoutMs)
        delay(10);

    esp_sntp_stop();

    return sntpSynced;
}

#include <HTTPClient.h>
#include <ArduinoJson.h>

bool fetchTimezone(String& tz, std::optional<Gps::Data::Location>& loc)
{
    HTTPClient http;
    http.begin("http://ip-api.com/json");

    int code = http.GET();
    if (code != 200)
        return false;

    JsonDocument doc;
    deserializeJson(doc, http.getString());

    tz = doc["timezone"].as<String>();

    loc.emplace();
    loc->mLat = doc["lat"].as<float>();
    loc->mLon = doc["lon"].as<float>();

    ESP_LOGE("Geo", "TZ: %s", tz.c_str());
    ESP_LOGE("Geo", "Loc %f / %f", doc["lat"].as<float>(), doc["lon"].as<float>());

    return true;
}

bool Core::NTPSync()
{
    auto powerLock = Power::Lock(Power::Flag::Wifi);
    initArduino();

    if (!connectWifi()) {
        ESP_LOGE("NTP", "WiFi failed");
        return false;
    }

    if (!syncTime()) {
        ESP_LOGE("NTP", "NTP sync failed");
        return false;
    }
    // Update time
    mTime.readTime();

    String timezone;
    if (fetchTimezone(timezone, mGps.mData.mLocation)) {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();
    }

    WiFi.disconnect(true);
    ESP_LOGI("NTP", "Sync done");
    return true;
}

void Core::NTPSync2() {
  // Select default voltage 2.9V/3.3V for WiFi
  // We need Arduino for this (WiFi + NTP)
  auto powerLock = Power::Lock(Power::Flag::Wifi);
  initArduino();

  if (kWifiNetworks.empty())
    return;

  // ESP_LOGE("Wifi", "%s %s", kWifiNetworks[0].mSsid.c_str(), kWifiNetworks[0].mPswd.c_str());
  WiFi.begin(kWifiNetworks[0].mSsid.c_str(), kWifiNetworks[0].mPswd.c_str());
  WiFi.waitForConnectResult();

//   settimeofday_cb([]() { // set callback to execute after time is retrieved
//     time_t now = time(nullptr);
//     setTime(now); // update time to TimeLib
//   });
  configTime(0, 0, "pool.ntp.org");

  // get network time
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    ESP_LOGE("NTP", "Failed to obtain time");
    return;
  }
  delay(5'000);
  WiFi.disconnect();

  ESP_LOGE("NTP", "done");
}