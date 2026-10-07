
#pragma once

#include "esp_attr.h"

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
    };

    struct Lock {
        Flag flag;
        Lock(Flag f) : flag(f) { Power::lock(flag); }
        ~Lock() { Power::unlock(flag); }
    };

    static void RTC_SLOW_ATTR lock(Flag f);
    static void RTC_SLOW_ATTR unlock(Flag f);

    static bool RTC_SLOW_ATTR current();
    static Flag RTC_SLOW_ATTR status();

    // These are private since should not be used directly
private:
    static void RTC_SLOW_ATTR apply();
    static void RTC_SLOW_ATTR set(bool high);
};