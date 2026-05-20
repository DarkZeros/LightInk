#pragma once

#include <cstdint>
#include <array>
#include <sdkconfig.h>

#define HW_WATCHY   0
#define HW_LIGHTINK 1

enum class InputType { TOUCH, BUTTONS };

// User config
#define HW_TYPE HW_WATCHY   // HW_LIGHTINK or HW_WATCHY
// #define HW_VERSION 1   // version number

// Defaults
#ifndef HW_VERSION
    #if HW_TYPE == HW_WATCHY
        #define HW_VERSION 1
    #elif HW_TYPE == HW_LIGHTINK
        #if CONFIG_IDF_TARGET_ESP32S3
            #define HW_VERSION 10 // First WIP board
        #else
            #define HW_VERSION 4 // Last board fully working
        #endif
    #endif
#endif

enum class HW_chips {ESP_32, ESP_32_S3};

// Original ESP32-PICO-D4 board based off from Watchy
// there were 2 previous revisions with wrong wiring/touch/chip
// that I am not even going to ever cover with code
struct LightInk_1 {
    constexpr static uint8_t kVersion = 1;
    constexpr static HW_chips kChipType = HW_chips::ESP_32;
    constexpr static InputType kInputType = InputType::TOUCH;
    constexpr static bool kHasLowVoltage = true;
    constexpr static bool kVoltageSelectInverted = false;
    constexpr static bool kHasLora = false;
    constexpr static bool kHasGps = false;
    constexpr static bool kHasDisplayBusyWake = false;

    constexpr static uint8_t kDefaultDisplayRotation = 2;

    struct Touch {
        enum PadNames {BotR /*Down*/, TopR /*Up*/, TopL /*Menu*/, BotL /*Back*/};
        constexpr static std::array<uint8_t, 4> Pad = {{6,0,2,5}};
    };

    struct Buttons {
        enum PadNames {BotR /*Down*/, TopR /*Up*/, TopL /*Menu*/, BotL /*Back*/};
        constexpr static std::array<uint8_t, 4> Pins = {{4,32,25,26}};
        constexpr static bool kActiveLevel = 1;
    };

    struct Spi {
        constexpr static uint8_t Sck = 18;
        constexpr static uint8_t Miso = -1; 
        constexpr static uint8_t Mosi = 23;
    };

    struct Display {
        constexpr static uint8_t Cs = 5;
        constexpr static uint8_t Res = 9;
        constexpr static uint8_t Dc = 10; // 8 bit transfer mode
        constexpr static uint8_t Busy = 19; // 35 better, never manufactured
    };
    
    struct Gps {
        // constexpr static uint8_t Tx = -1;
        constexpr static uint8_t Rx = -1;
        constexpr static uint8_t Vcc = -1; // RTC reqired
        constexpr static auto BaudRate = 115200;
    };
    struct Lora {
        constexpr static uint8_t Cs = -1; // RTC reqired
        constexpr static uint8_t Res = -1;
        constexpr static uint8_t Busy = -1; // Input only pin
        constexpr static uint8_t Dio1 = -1; // IRQ // Input only pin
    };

    constexpr static uint8_t kAdcPin = 34;
    constexpr static uint8_t kLightPin = 25;
    constexpr static uint8_t kSpeakerPin = 26;
    constexpr static uint8_t kVibratorPin = 27;
    constexpr static uint8_t kVoltageSelectPin = 13;

    // Unused but available in V1 and V2
    constexpr static uint8_t kRtcIntPin = 32;
    constexpr static uint8_t kSdaPin = -1;
    constexpr static uint8_t kSclPin = -1;
};

// Minor changes done to the board
struct LightInk_2 : public LightInk_1 {
    constexpr static uint8_t kVersion = 2;
    constexpr static bool kVoltageSelectInverted = true;
};

// Added Lora/GPS to PICOD4, reshuffle pins
// Use as much as possible INPUT ONLY pins 34-39
// NOTE: I never build a final version out of this HW
//       since it didn´t fit on a small case
struct LightInk_3 : public LightInk_2 {
    constexpr static uint8_t kVersion = 3;
    constexpr static bool kHasLora = true;
    constexpr static bool kHasGps = true;
    constexpr static bool kHasDisplayBusyWake = true;

    struct Spi {
        constexpr static uint8_t Sck = 10;
        constexpr static uint8_t Miso = 38; 
        constexpr static uint8_t Mosi = 9;
    };

    struct Display {
        constexpr static uint8_t Cs = 5;
        constexpr static uint8_t Res = 23;
        constexpr static uint8_t Dc = 18; // Could be 9 bit transfer and avoid this pin
        constexpr static uint8_t Busy = 35; // RTC required // Input only pin
    };

    struct Lora {
        constexpr static uint8_t Cs = 26; // RTC reqired
        constexpr static uint8_t Res = 19;
        constexpr static uint8_t Busy = 37; // Input only pin
        constexpr static uint8_t Dio1 = 36; // IRQ // Input only pin
    };

    struct Gps {
        constexpr static uint8_t Rx = 39; // Input only pin
        constexpr static uint8_t Vcc = 27; // RTC required
        constexpr static auto BaudRate = 115200;
    };

    constexpr static uint8_t kAdcPin = 34;
    constexpr static uint8_t kLightPin = 25; // RTC required
    constexpr static uint8_t kSpeakerPin = 22;
    constexpr static uint8_t kVibratorPin = 21;
    constexpr static uint8_t kVoltageSelectPin = 13; // RTC required

    constexpr static uint8_t kRtcIntPin = -1;
};

// Minor changes to accomodate HW_3 issues into a smaller board
struct LightInk_4 : public LightInk_3 {
    constexpr static uint8_t kVersion = 4;

    struct Touch {
        enum PadNames {BotR /*Down*/, TopR /*Up*/, TopL /*Menu*/, BotL /*Back*/};
        constexpr static std::array<uint8_t, 4> Pad = {{7, 2, 4, 6}};
    };

    struct Gps {
        constexpr static uint8_t Rx = 39; // Input only pin
        constexpr static uint8_t Vcc = 12; // RTC required
        constexpr static auto BaudRate = 115200;
    };

    constexpr static uint8_t kVoltageSelectPin = 4; // RTC required
};

// New board based on ESP32-S3-FN8,
// but uses too much power compared to PicoD4 in standby, and fails to work with 2.7V
struct LightInk_10 {
    constexpr static uint8_t kVersion = 10;
    constexpr static HW_chips kChipType = HW_chips::ESP_32_S3;
    constexpr static InputType kInputType = InputType::TOUCH;
    // Sadly the feature makes the SoC off...
    constexpr static bool kHasLowVoltage = false;
    constexpr static bool kVoltageSelectInverted = true /*false*/; // TEMPORARY
    constexpr static bool kHasLora = true;
    constexpr static bool kHasGps = true;
    constexpr static bool kHasDisplayBusyWake = true;

    struct Touch {
        enum PadNames {BotR /*Down*/, TopR /*Up*/, TopL /*Menu*/, BotL /*Back*/};
        constexpr static std::array<uint8_t, 4> Pad = {{1,4,8,6}};
    };

    struct Spi {
        constexpr static uint8_t Sck = 19;
        constexpr static uint8_t Miso = 34; //DIO1 // 17;
        constexpr static uint8_t Mosi = 18;
    };

    struct Display {
        constexpr static uint8_t Cs = 20;
        constexpr static uint8_t Res = 10; // NOT ALLOWED 22
        constexpr static uint8_t Dc = 21; // Could be 9 bit transfer
        constexpr static uint8_t Busy = 7;
    };

    struct Lora {
        // constexpr static uint8_t Cs = 35; // PULL UP REQUIRED
        // TEMPORARY TEST
        constexpr static uint8_t Cs = 2; // PULL UP REQUIRED
        

        constexpr static uint8_t Res = 38; //13;
        constexpr static uint8_t Busy = 36;
        constexpr static uint8_t Dio1 = 17; // MISO // 34; // IRQ?
        
        // constexpr static uint8_t Dio2 = 33; // Needed? NO
        //constexpr static uint8_t Vcc = ?; // Can it share with RES? 0.3uA/3.3V, not needed!
    };

    struct Gps {
        constexpr static uint8_t Tx = 38;  // Not need to TX to the GPS
        constexpr static uint8_t Rx = 37;
        constexpr static uint8_t Vcc = 11;
        constexpr static auto BaudRate = 115200;
    };

    constexpr static uint8_t kAdcPin = 9;
    constexpr static uint8_t kRtcIntPin = 1;
    constexpr static uint8_t kLightPin = 12;
    constexpr static uint8_t kSpeakerPin = 2;
    constexpr static uint8_t kVibratorPin = 14;
    constexpr static uint8_t kVoltageSelectPin = 5;
};

// Try to support watchy device with this firmware, WIP!
struct Watchy_1 : public LightInk_1 {
    constexpr static uint8_t kVersion = 0;
    constexpr static HW_chips kChipType = HW_chips::ESP_32;
    constexpr static bool kHasLowVoltage = false;
    constexpr static InputType kInputType = InputType::BUTTONS;

    constexpr static uint8_t kDefaultDisplayRotation = 0; // Watchy uses the old rotation

    struct Buttons {
        enum PadNames {BotR /*Down*/, TopR /*Up*/, TopL /*Menu*/, BotL /*Back*/};
        constexpr static std::array<uint8_t, 4> Pins = {{4,32,25,26}};
        constexpr static bool kActiveLevel = 1;
    };

    struct Spi {
        constexpr static uint8_t Sck = 18;
        constexpr static uint8_t Miso = -1;
        constexpr static uint8_t Mosi = 23;
    };

    struct Display {
        constexpr static uint8_t Cs = 5;
        constexpr static uint8_t Res = 9;
        constexpr static uint8_t Dc = 10; // 8 bit transfer mode
        constexpr static uint8_t Busy = 19; // 35 better, never manufactured
    };

    constexpr static uint8_t kAdcPin = 33;
    constexpr static uint8_t kLightPin = -1;
    constexpr static uint8_t kSpeakerPin = -1;
    constexpr static uint8_t kVibratorPin = 13;
    constexpr static uint8_t kVoltageSelectPin = -1;

    // Unused but available in V1 and V2
    constexpr static uint8_t kRtcIntPin = 27;
    constexpr static uint8_t kSdaPin = 21;
    constexpr static uint8_t kSclPin = 22;
};

#define CONCAT(a, b) a##b
#if HW_TYPE == HW_LIGHTINK
    #define LIGHTINK_HW(version) CONCAT(LightInk_, version)
    using HW = LIGHTINK_HW(HW_VERSION);
#elif HW_TYPE == HW_WATCHY
    #define WATCHY_HW(version) CONCAT(Watchy_, version)
    using HW = WATCHY_HW(HW_VERSION);
#else
    #error "Invalid HW_TYPE selected"
#endif