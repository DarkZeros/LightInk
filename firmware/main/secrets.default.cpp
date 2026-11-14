// Default secrets: placeholders, so that a fresh clone builds.
//
// The build copies this file to secrets.cpp when there is none. secrets.cpp is yours
// and gitignored: put your WiFi networks and radio signals there, never in this file.
// When secrets.h changes, this file shows the shape secrets.cpp has to follow.

#include "secrets.h"

using namespace Signal::literals;

namespace {

// Radio signals, one Radio menu per group. These are placeholders, they open nothing
constexpr uint8_t kGarageUp[]   = {0x88, 0x8e};
constexpr uint8_t kGarageDown[] = {0x88, 0xe8};

constexpr Signal::Entry kGarage[] = {
    // Bytes sent as they are, MSB first: MHz, us per bit, bytes, repetitions, us between
    {"Up",   Signal::BasicOOK{433.98f, 266, kGarageUp, 4, 1700}},
    {"Down", Signal::BasicOOK{433.98f, 266, kGarageDown, 4, 1700}},
};
constexpr Signal::Entry kDoor[] = {
    // Each bit of mSequence is sent as mPattern0 or mPattern1
    {"Bell", Signal::FixedWidthPWM{.mFrequency=433.92f, .mBitMicros=350,
        .mSequence=1010'0101'1100_bits,
        .mPattern0=100_bits, .mPattern1=110_bits,
        .mRepetitions=4, .mDelayRepetitions=10000
    }},
};

constexpr Signal::Group kGroups[] = {
    {"Garage", kGarage},
    {"Door", kDoor},
};

// WiFi networks for the time sync, tried in this order
constexpr WifiConfig kNetworks[] = {
    {"ssid", "password"},
};

}

constinit const std::span<const Signal::Group> kSignals = kGroups;
constinit const WifiNetworks kWifiNetworks = kNetworks;
