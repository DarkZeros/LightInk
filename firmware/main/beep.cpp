#include "beep.h"
#include "notes.h"
#include "peripherals.h"
#include "settings.h"

#include "Arduino.h"

namespace Beep {

namespace {
using Pattern = std::vector<std::pair<int, int>>;

// The piezo is loudest around 4 kHz, so the coded chimes use 4096 Hz and its octave below
constexpr int kHigh = 4096, kLow = 2048;

// Adds the note, with a silence before it if it is not the first one
void addNote(Pattern& p, int freq, int ms, int gap) {
  if (!p.empty())
    p.emplace_back(0, gap);
  p.emplace_back(freq, ms);
}

// Roman numeral of the hour (1-12): I is a short high beep, V a long high beep and
// X a long low one. 4 is "bip beeep" (IV), 9 "bip booop" (IX), 12 "booop bip bip" (XII)
Pattern romanPattern(int hour12) {
  constexpr const char* kRoman[] = {"", "I", "II", "III", "IV", "V", "VI",
                                    "VII", "VIII", "IX", "X", "XI", "XII"};
  Pattern p;
  for (auto c = kRoman[hour12]; *c; c++) {
    if (*c == 'I')
      addNote(p, kHigh, 70, 110);
    else
      addNote(p, *c == 'V' ? kHigh : kLow, 300, 110);
  }
  return p;
}

// The hour (1-12) in 4 bits, first the highest: a high beep is a 1, a low beep a 0.
// Always 4 beeps, 3 is "low low high high" (0011)
Pattern binaryPattern(int hour12) {
  Pattern p;
  for (auto bit = 3; bit >= 0; bit--)
    addNote(p, (hour12 >> bit) & 1 ? kHigh : kLow, 90, 90);
  return p;
}

// Ship's bells: one pair of strokes per hour of the 4 hour watch, so 2, 4, 6 and 8
// bells, 8 at 12, 4 and 8 o'clock
Pattern bellsPattern(int hour) {
  const int pairs = (hour + 3) % 4 + 1;
  Pattern p;
  for (auto i = 0; i < pairs; i++) {
    addNote(p, NOTE_G7, 50, 320);
    addNote(p, NOTE_G7, 50, 90);
  }
  return p;
}

Pattern hourPattern(Sound sound, int hour) {
  const int hour12 = (hour + 11) % 12 + 1;
  switch (sound) {
  // Short arpeggio rising fast
  case Sound::Chirp:
    return {{NOTE_C7, 35}, {NOTE_E7, 35}, {NOTE_G7, 35}, {NOTE_C8, 60}};
  // Falling third of the cuckoo clock
  case Sound::Cuckoo:
    return {{NOTE_G6, 140}, {0, 70}, {NOTE_E6, 260}};
  // First quarter of the Westminster chimes
  case Sound::Chime:
    return {{NOTE_GS6, 220}, {0, 30}, {NOTE_FS6, 220}, {0, 30},
            {NOTE_E6, 220}, {0, 30}, {NOTE_B5, 450}};
  case Sound::Roman:
    return romanPattern(hour12);
  case Sound::Binary:
    return binaryPattern(hour12);
  case Sound::Bells:
    return bellsPattern(hour);
  // Hourly chime of the old Casio watches: two equal short beeps at 4096 Hz (32768 / 8)
  case Sound::Casio:
  default:
    return {{kHigh, 62}, {0, 62}, {kHigh, 62}};
  }
}
} // namespace

void hourlyBeep(int hour) {
  Peripherals::speaker(hourPattern(kSettings.mHourly.mSound, hour));
}

void preview(int hour) {
  if (kSettings.mTouch.mBeep)
    delay(300);
  hourlyBeep(hour);
}

void beep() {
  Peripherals::speaker({{kHigh, 20}});
}

} // namespace Beep

