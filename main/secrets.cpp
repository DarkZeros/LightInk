// THIS FILE CONTAINS SECRETS LIKE DEFAULT WIFI PASSWORDS AND GARAGE SEQUENCES
// DO NOT SUBMIT IT TO THE REPOSITORY

#include <vector>
#include "secrets.h"
#include "radio.h"


// DUMMY PLACEHOLDERS TO ALLOW BUILDING
const std::vector<Signal::Group> kSignals = {
    {"Garage", {
        {"Up", Signal::BasicOOK{433.98f, 266, 
            {0x88, 0x8e},
            4, 1700
        }},
        {"Down", Signal::BasicOOK{433.98f, 266, 
            {0x88, 0xe8},
            4, 1700
        }},
    }},
};

const WifiNetworks kWifiNetworks = {{"asd", "asd"}};
