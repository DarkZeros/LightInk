
#pragma once

#include "esp_attr.h"
#include "hal/gpio_types.h"
#include "soc/rtc_periph.h"

#include <atomic>

/* Helper class to handle extra power needed for WiFi/Display operations
 */
struct Power {
    enum Flag : uint8_t {
        Display = 0x01,
        Light = 0x02,
        Speaker = 0x04,
        Gps = 0x08,
        Wifi = 0x10,
        BT = 0x20,
        Lora = 0x40,
        Trace = 0x80, // Debug trace: rail high while awake (see trace.cpp)
    };

    struct Lock {
        Flag flag;
        Lock(Flag f) : flag(f) { Power::lock(flag); }
        ~Lock() { Power::unlock(flag); }
    };

    // Excludes other tasks and cores from the rail state and the RTC hold registers.
    // Uses no OS so it also works in the wake stub. It disables interrupts, so keep
    // it short, and it does not nest
    struct Critical {
        RTC_SLOW_ATTR Critical();
        RTC_SLOW_ATTR ~Critical();
    private:
        uint32_t mIntLevel;
    };

    // Use instead of rtc_gpio_hold_en/dis(), they share registers with the rail pin
    static void hold(gpio_num_t pin, bool enable);
    // Same for pins used in the wake stub, also handles the deep sleep hold.
    // The pad has to be in RTC memory there
    static void RTC_SLOW_ATTR hold(const rtc_io_desc_t& pad, bool enable);

    static void RTC_SLOW_ATTR lock(Flag f);
    static void RTC_SLOW_ATTR unlock(Flag f);

    static bool RTC_SLOW_ATTR current();
    static Flag RTC_SLOW_ATTR status();

    // These are private since should not be used directly
private:
    static void RTC_SLOW_ATTR high() { set(true); }
    static void RTC_SLOW_ATTR low() { set(false); }
    static void RTC_SLOW_ATTR set(bool high);
    static void RTC_SLOW_ATTR setHold(const rtc_io_desc_t& pad, bool enable);
};