#include "buttons.h"
#include "touch.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

namespace {
    RTC_DATA_ATTR bool mInitialized {false};
    RTC_DATA_ATTR uint64_t mEnabledButtonMask {0};  // Bitmask of enabled button pins
};

ButtonInput::ButtonInput(TouchSettings& settings) : mSettings{settings} {
}

void ButtonInput::initialize() {
    // Configure GPIO pins as RTC inputs with pull-ups
    // These pins will be used to wake from deep sleep
    for (int i = 0; i < HW::Buttons::Pins.size(); i++) {
        gpio_num_t pin = (gpio_num_t)HW::Buttons::Pins[i];

        // Configure as input with pull-up
        gpio_config_t io_conf = {};
        io_conf.intr_type = HW::Buttons::kActiveLevel ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
        io_conf.mode = GPIO_MODE_INPUT;
        // io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        // io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pin_bit_mask = (1ULL << pin);
        gpio_config(&io_conf);
        
        // Enable RTC IO for this pin (needed for deep sleep wakeup)
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        // rtc_gpio_pulldown_en(pin);
    }
    
    mInitialized = true;
}

void ButtonInput::setUp(bool onlyMenuLight) {
    if (!mInitialized) {
        initialize();
    }

    // Calculate which button pins should be enabled
    mEnabledButtonMask = 0;
    if (onlyMenuLight) {
        mEnabledButtonMask |= 1ULL << HW::Buttons::Pins[mSettings.mMap[HW::Buttons::TopL]];
        mEnabledButtonMask |= 1ULL << HW::Buttons::Pins[mSettings.mMap[HW::Buttons::BotL]];
    } else {
        // Enable all buttons
        for (auto pin : HW::Buttons::Pins) {
            mEnabledButtonMask |= (1ULL << pin);
        }
    }
}

void ButtonInput::enable() const {
    // Enable EXT1 wakeup for enabled button pins
    // Use the pre-calculated mask from setUp()
    esp_sleep_enable_ext1_wakeup(mEnabledButtonMask, ESP_EXT1_WAKEUP_ANY_HIGH);
}

void ButtonInput::disable() const {
    // Disable EXT1 wakeup
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
}

InputInterface::Btn ButtonInput::read() const {
    // Read current button state without clearing
    uint8_t bitmask = 0;
    for (int i = 0; i < HW::Buttons::Pins.size(); i++) {
        gpio_num_t pin = (gpio_num_t)HW::Buttons::Pins[mSettings.mMap[i]];
        int level = gpio_get_level(pin);
        // Button is pressed when GPIO level matches kActiveLevel
        if (level == HW::Buttons::kActiveLevel) {
            bitmask |= (1 << i);
        }
    }
    return (InputInterface::Btn)bitmask;
}

std::vector<uint16_t> ButtonInput::readAll() const {
    // Return raw button states for all buttons
    std::vector<uint16_t> res;
    for (int i = 0; i < HW::Buttons::Pins.size(); i++) {
        gpio_num_t pin = (gpio_num_t)HW::Buttons::Pins[mSettings.mMap[i]];
        int level = gpio_get_level(pin);
        // Button is pressed when GPIO level matches kActiveLevel
        res.emplace_back(level == HW::Buttons::kActiveLevel ? 1 : 0);
    }
    return res;
}

void ButtonInput::clear() const {
}

InputInterface::Btn ButtonInput::readAndClear() const {
    // Read button state and clear it atomically
    auto btn = read();
    clear();
    return btn;
}
