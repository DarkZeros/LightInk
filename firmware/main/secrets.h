#pragma once

#include "radio.h"

#include <string.h>
#include <vector>

#include <string_view>

struct WifiConfig {
    std::string mSsid, mPswd;
};
using WifiNetworks = std::vector<WifiConfig>;

extern const WifiNetworks kWifiNetworks;
extern const std::vector<Signal::Group> kSignals;