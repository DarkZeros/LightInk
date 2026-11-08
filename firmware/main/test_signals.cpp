#include "radio.h"

#if CONFIG_LIGHTINK_FAKE_SIGNALS
using namespace Signal::literals;

namespace {
// Meaningless bursts: they key the transmitter like a remote does, one per signal
// type, but carry no code any receiver knows. 433.92 MHz ISM band, sent once
constexpr uint8_t kNoise[] = {0x5a, 0x3c, 0x96, 0x0f, 0xe1, 0x77, 0x28, 0xb4};

constexpr Signal::Entry kGarbage[] = {
    {"Garbage OOK", Signal::BasicOOK{433.92f, 500, kNoise, 1, 0}},
    {"Garbage PWM", Signal::FixedWidthPWM{.mFrequency=433.92f, .mBitMicros=400,
        .mSequence=1011'0010'0111'0001_bits,
        .mPattern0=1000_bits, .mPattern1=1110_bits,
        .mRepetitions=1, .mDelayRepetitions=0
    }},
};

constexpr Signal::Group kGroups[] = {
    {"Test signals", kGarbage},
};
} // namespace

constinit const std::span<const Signal::Group> kTestSignals = kGroups;
#endif
