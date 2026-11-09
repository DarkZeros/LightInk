
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
#include "trace.h"
#include "settings_table.h"

#include "watchface_default.h"
#include "watchface_eclipse.h"

RTC_DATA_ATTR Settings kSettings;

// Below 10% the radio's current peaks aren't worth the risk, the phone can wait
constexpr uint16_t kBleMinBattery = 100;

// IDF reports every pending cause as a mask, we act on one: same priority as the
// esp_sleep_get_wakeup_cause() this replaces, for the sources we arm
static esp_sleep_wakeup_cause_t wakeupCause() {
    const auto causes = esp_sleep_get_wakeup_causes();
    for (auto cause : {ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_EXT1, ESP_SLEEP_WAKEUP_TOUCHPAD}) {
        if (causes & BIT(cause))
            return cause;
    }
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}

auto wakeup_reason = Trace::wakeCause(wakeupCause());

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

    // Settings and the phone pairing saved in flash survive resets and flashing
    SettingsTable::load();
    mTime.calSet(); // The stored calibration, if any
    mTime.readTime();
    Ble::load();

    // Queue the rest of the first boot for later (GPS, LORA, NTP, Touch)
    mGps.off();
    mTasks.emplace_back(std::async(std::launch::deferred, [&]{
        // Trigger NTP, if wifi is available, it will set time
        NTPSync();

        // Try to get GPS location, to setup time/location
        if constexpr (HW::kHasGps) {
            if (!mGps.mData.mLocation)
                mGps.on();
        } else {
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

    // Location may have been obtained by other means (NTP), GPS is not needed anymore
    if (HW::kHasGps && mGps.isOn() && mGps.mData.mLocation)
        mGps.off();

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
        handleTouch();
    } break;
    case ESP_SLEEP_WAKEUP_TIMER: // Internal Timer
        // Check watchdog -> reset to watchFace. The menu's popup goes with it, one on
        // the watchface stays until a button dismisses it
        if (kSettings.mTouchWatchDog && kSettings.mUi.mDepth >= 0) {
            kSettings.mUi.mDepth = -1; // Reset to watchFace
            kSettings.mUi.mPopup.reset();
        }
        kSettings.mTouchWatchDog = true;
        break;
    case ESP_SLEEP_WAKEUP_EXT1: // Used for LoRa reception
        ESP_LOGE("lora", "ext1 wakeup");
        mRadio.readLoraPck();
        break;
    default: // First time boot!
        ESP_LOGE("", "boot %lu unkown wakeup reason %d", micros(), wakeup_reason);
        break;
    }

    // Bluetooth sync with the phone, only on wakes that boot anyway. It runs while the
    // display refreshes and gives up when the refresh ends unless the phone answered
    std::optional<Ble::Sync> bleSync;
    const auto now = mTime.getTimeval().tv_sec;
    if ((wakeup_reason == ESP_SLEEP_WAKEUP_TIMER || wakeup_reason == ESP_SLEEP_WAKEUP_TOUCHPAD)
        && !mBleUsed && mBattery.mCurPercent >= kBleMinBattery && Ble::due(now))
        bleSync.emplace(bleWatch());

    // Re-set up the touch if settings have changed
    mTouch.setUp(kSettings.mUi.mDepth < 0);

    // Beep conditions
    const bool hourUpdate = mNow.Minute == 0 && mNow.Second == 0 && wakeup_reason == ESP_SLEEP_WAKEUP_TIMER;
    if (hourUpdate
        && mNow.Hour >= kSettings.mHourly.mFirst
        && mNow.Hour <= kSettings.mHourly.mLast)
    {
        if (kSettings.mHourly.mBeep) {
            mTasks.emplace_back(std::async(std::launch::async, Beep::hourlyBeep, mNow.Hour));
        }
        if (kSettings.mHourly.mVib) {
            mTasks.emplace_back(std::async(std::launch::async, []{
                Peripherals::vibrator(std::vector<int>{50,75,50});
            }));
        }
    }

    draw();

    if (bleSync && bleSync->finish()) {
        // The app changed settings: show them now, not at the next full wake (the wake
        // stub would keep drawing the old watchface until then)
        mTouch.setUp(kSettings.mUi.mDepth < 0);
        kSettings.mWatchface.mLastDraw.mValid = false;
        draw();
    }
    // Auto sync under an hour needs full wakes of its own, the wake stub doesn't sync.
    // Not at night or on a low battery, where the sync wouldn't run anyway
    if (mBattery.mCurPercent >= kBleMinBattery && !(kSettings.mPower.mNight && mNow.Hour < 7)) {
        if (auto in = Ble::nextSyncIn(now))
            setNextUpdate(*in);
    }

    // Finish pending tasks, then setup touch
    finishTasks();
    mTouch.enable();

    // Settings to flash, at most hourly and only if changed: a time compare otherwise
    SettingsTable::save(mTime.getTimeval().tv_sec);

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
            // Never past the hourly full wake (hour beep, ...)
            nextFullWake = std::min<int>(60, mNow.Minute + seconds / 60);
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
        if (Radio::isListening())
            esp_sleep_enable_ext1_wakeup(1ULL << HW::Lora::Dio1, ESP_EXT1_WAKEUP_ANY_HIGH);
        // Hold some pins to some values to avoid leaking current
        Power::hold((gpio_num_t)HW::Lora::Cs, true);
    }

    // We can only run wakeupstub when on watchface mode
    if (kSettings.mUi.mDepth < 0) {
        // The stub would draw the minutes over a popup: full boots until it's dismissed
        const bool stub = !kSettings.mUi.mPopup;
        kDSState.currentMinutes = mNow.Minute + firstMinutesSleep;
        kDSState.minutes = nextFullWake - mNow.Minute - firstMinutesSleep;
        // ESP_LOGE("", "min %d step %d wait %ld", kDSState.minutes, stepSize, kDSState.updateWait);
        kDSState.stepSize = stepSize;
        // Only trigger the wakeupstub if there is more than 1 minute left
        if (!stub) {
            esp_set_deep_sleep_wake_stub(NULL);
        } else if (kDSState.minutes > 0) {
            // BUSY wakes us through ext1, it works with RTC_PERIPH off unlike ext0. Only
            // set up the pin here, the wake stub arms ext1 itself while the display
            // refreshes. RTC_PERIPH is off, so the hold is what keeps the pad configured.
            if constexpr (HW::kHasDisplayBusyWake) {
                constexpr auto busy = (gpio_num_t)HW::Display::Busy;
                rtc_gpio_init(busy);
                rtc_gpio_set_direction(busy, RTC_GPIO_MODE_INPUT_ONLY);
                Power::hold(busy, true);
                kDSState.busyWakePin = BIT(rtc_io_number_get(busy));
                kDSState.radioWakePin = HW::kHasLora && Radio::isListening()
                    ? BIT(rtc_io_number_get((gpio_num_t)HW::Lora::Dio1)) : 0;
            }
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

    Trace::sleep();
    esp_deep_sleep_disable_rom_logging();
    esp_deep_sleep_start();
    ESP_LOGE("deepSleep", "never reach!");
}

// The only place that puts a frame on the panel: elements just draw into the buffer.
// Hibernates after the refresh, the analog must not stay on while tasks run. Can run
// more than once per wake, display settings may have changed since the last one
void Core::draw() {
    mDisplay.wake();

    auto& disp = kSettings.mDisplay;
    mDisplay.setFlip(disp.mFlip);
    mDisplay.setDarkBorder(disp.mDarkBorder ^ disp.mInvert);
    mDisplay.setInverted(!disp.mInvert);
    mDisplay.setTextColor(0xFF);

    // Show watch face or menu ?
    const bool watchface = kSettings.mUi.mDepth < 0;
    auto& last = kSettings.mWatchface.mLastDraw;
    // A popup needs a full frame, and so does the frame after it to remove it
    if (kSettings.mUi.mPopup)
        last.mValid = false;
    const bool full = !watchface || !last.mValid;
    // A full frame draws everything again, but the buffer may hold an earlier frame of
    // this wake (progress()): the watchface digits have no background and would pile up
    if (full)
        mDisplay.fillScreen(0);

    if (watchface) {
        mDisplay.setRefreshMode(kSettings.mDisplay.mWatchLut);
        #define ARGS kSettings, kSettings.mWatchface, *this, mDisplay
        // Instantiate the watchface type we are using. A full frame is only drawn into
        // the buffer, a partial update it writes and refreshes by itself: it depends on
        // what it left in the display RAM
        switch(kSettings.mWatchface.mType) {
            case 0: default: DefaultWatchface(ARGS).draw(); break;
            case 1: EclipseWatchface(ARGS).draw(); break;
            // case 2: break;
            // case 3: break;
        }
        #undef ARGS
        Light::off(); // Always turn off light exiting the Menus
    } else {
        mDisplay.setRefreshMode(kSettings.mDisplay.mMenuLut);
        mDisplay.setCursor(0, 0);
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

    if (full) {
        if (auto& popup = kSettings.mUi.mPopup)
            UI::renderPopup(mDisplay, popup->data());
        mDisplay.writeAllAndRefresh();
        // The watchface's next partial needs this frame in the front buffer too: after a
        // partial it holds the frame before. A full refresh doesn't swap (T80)
        if (watchface && !mDisplay.lastRefreshFull())
            mDisplay.writeAll();
    }
    // The watchface redraws everything after the menus or a popup
    if (!watchface || kSettings.mUi.mPopup)
        last.mValid = false;

    mDisplay.hibernate();
}

void Core::progress(std::string_view msg) {
    kSettings.mUi.popup(msg);
    draw();
}

void Core::runCancellable(std::string_view msg, std::function<void(const Cancel&)> job) {
    // No polling loop here: waking every 50 ms costs power and each light sleep exit
    // and entry glitches a playing tone. The touch status latches, so the job's own
    // reads of cancel find a BACK press made while it slept
    Cancel cancel{[this]{ return (mTouch.poll() & Touch::BACK) != 0; }};
    auto task = std::async(std::launch::async, [&]{ job(cancel); });
    progress(msg);
    task.get();
    kSettings.mUi.mPopup.reset();
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
    if (kSettings.mTouch.mHaptic)
        mTasks.emplace_back(std::async(std::launch::async, []{
            Peripherals::vibrator(std::vector<int>{25});
    }));

    // Clear WatchDog since we received a valid touch
    kSettings.mTouchWatchDog = false;

    // Convert to what button was it
    Touch::Btn btn = Trace::touch(mTouch.readAndClear());
    ESP_LOGE("parsed", "%d, %s", btn, std::string(magic_enum::enum_name(btn)).c_str());

    auto& ui = kSettings.mUi;
    ESP_LOGE("ui", "depth%d st%d", ui.mDepth, ui.mState[ui.mDepth]);

    // Sound feedback on the menus, and for the press that enters them
    if (kSettings.mTouch.mBeep && (ui.mDepth >= 0 ? btn != Touch::NONE : btn == Touch::MENU))
        mTasks.emplace_back(std::async(std::launch::async, Beep::beep));

    // A popup swallows the press, any button dismisses it (on the watchface as well)
    if (ui.mPopup) {
        ui.mPopup.reset();
        return;
    }

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
    Trace::wifiStarted();
    for (auto& net : kWifiNetworks) {
        ESP_LOGI("WiFi", "Connecting to %s", net.mSsid);
        WiFi.begin(net.mSsid, net.mPswd);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            ESP_LOGE("WiFi", "Connected: %s", net.mSsid);
            return true;
        }
        WiFi.disconnect(true);
        delay(100);
    }
    return false;
}

#include "esp_sntp.h"

static bool sntpSynced = false;
void timeSyncCallback(struct timeval *tv)
{
    sntpSynced = true;
    Trace::ntpSynced();
}

bool syncTime(uint32_t timeoutMs = 5000)
{
    sntpSynced = false;
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(timeSyncCallback);
    esp_sntp_init();

    uint32_t start = millis();

    while (!sntpSynced && (millis() - start < timeoutMs))
        delay(100);

    esp_sntp_stop();

    return sntpSynced;
}

#include <HTTPClient.h>
#include <ArduinoJson.h>

bool fetchLocationTz(int16_t& minutesWest, std::optional<Gps::Data::Location>& loc)
{
    HTTPClient http;
    http.begin("http://ip-api.com/json/?fields=lat,lon,offset");

    int code = http.GET();
    if (code != 200)
        return false;

    JsonDocument doc;
    deserializeJson(doc, http.getString());

    loc.emplace();
    loc->mLat = doc["lat"].as<float>();
    loc->mLon = doc["lon"].as<float>();
    ESP_LOGE("Geo", "Loc %f / %f", loc->mLat, loc->mLon);

    minutesWest = doc["offset"].as<int>() / 60;
    ESP_LOGE("Geo", "minutesWest: %d", minutesWest);

    return true;
}

bool Core::NTPSync()
{
    // Refresh before the radio is on, it is the expensive part
    progress("WiFi\nConnecting...");

    auto powerLock = Power::Lock(Power::Flag::Wifi);
    initArduino();
    // Turn the radio off on every exit path, before powerLock drops to low voltage
    struct WifiOff { ~WifiOff() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); } } wifiOff;

    if (!connectWifi()) {
        ESP_LOGE("NTP", "WiFi failed");
        kSettings.mUi.popup("WiFi\nFailed");
        return false;
    }

    progress("WiFi connected\nNTP syncing...");

    if (!syncTime()) {
        ESP_LOGE("NTP", "NTP sync failed");
        kSettings.mUi.popup("NTP\nFailed");
        return false;
    }

    // Fetch and update GPS / Location and Timezone
    fetchLocationTz(mTime.getMinutesWest(), mGps.mData.mLocation);

    // Update time
    mTime.readTime();

    ESP_LOGI("NTP", "Sync done");
    char msg[32];
    std::snprintf(msg, sizeof(msg), "NTP synced\n%02d:%02d:%02d\nUTC%+d", mNow.Hour, mNow.Minute, mNow.Second,
                  kSettings.mTime.mMinutesWest / 60);
    kSettings.mUi.popup(msg);
    return true;
}