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
// The same from a menu action: when the buttons beep it waits first, so that the
// beep of the press is not taken as a part of the chime
void preview(int hour);

// Short tick, the sound feedback of a button (kSettings.mTouch.mBeep)
void beep();

} // namespace Beep
