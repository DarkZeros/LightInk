#pragma once

#include <inttypes.h>

// The sounds of the watch, played with Peripherals::speaker()
namespace Beep {

// Hourly chime. The names are shown on the menu as they are written here.
// Roman, Binary and Bells tell the hour, see hourPattern() in beep.cpp
enum class Sound : uint8_t {
    Casio,
    Chirp,
    Cuckoo,
    Chime,
    Roman,
    Binary,
    Bells,
};

// Plays the chime selected in the settings, hour (0-23) is used by the coded ones
void hourlyBeep(int hour);

} // namespace Beep
