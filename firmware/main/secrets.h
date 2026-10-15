#pragma once

#include "radio.h"

#include <string.h>
#include <span>
#include <vector>

#include <string_view>

struct WifiConfig {
    const char* mSsid;
    const char* mPswd;
};
// constinit in secrets.cpp: lives in flash, no construction at boot
using WifiNetworks = std::span<const WifiConfig>;

extern const WifiNetworks kWifiNetworks;
extern const std::span<const Signal::Group> kSignals;