#include "remote_settings.h"

#include <type_traits>

#include "esp_rom_crc.h"
#include <magic_enum.hpp>

#include "settings.h"

namespace RemoteSettings {
namespace {

constexpr uint8_t kSchemaVersion = 1;

constexpr std::string_view kWatchfaces[] = {"Default", "Eclipse"};

template<typename T>
using Field = std::remove_cvref_t<T>;

#define BOOL(id, group, name, field) \
    Setting{id, Kind::Bool, group, name, 0, 1, {}, \
        []() -> int16_t { return kSettings.field; }, \
        [](int16_t v) { kSettings.field = v != 0; }}

#define INT(id, kind, group, name, field, lo, hi) \
    Setting{id, kind, group, name, lo, hi, {}, \
        []() -> int16_t { return kSettings.field; }, \
        [](int16_t v) { kSettings.field = v; }}

#define LIST(id, group, name, field, labels) \
    Setting{id, Kind::Enum, group, name, 0, std::size(labels) - 1, labels, \
        []() -> int16_t { return kSettings.field; }, \
        [](int16_t v) { kSettings.field = v; }}

// Wire value is the index in magic_enum's order, the labels are the enum names
#define ENUM(id, group, name, field) \
    Setting{id, Kind::Enum, group, name, 0, \
        magic_enum::enum_count<Field<decltype(kSettings.field)>>() - 1, \
        magic_enum::enum_names<Field<decltype(kSettings.field)>>(), \
        []() -> int16_t { return magic_enum::enum_index(kSettings.field).value_or(0); }, \
        [](int16_t v) { kSettings.field = magic_enum::enum_value<Field<decltype(kSettings.field)>>(v); }}

const Setting kAll[] = {
    INT(1, Kind::TzMinutes, "Clock", "Time zone", mTime.mMinutesWest, -12 * 60, 14 * 60),
    BOOL(2, "Clock", "Hour beep", mHourly.mBeep),
    ENUM(6, "Clock", "Beep sound", mHourly.mSound),
    BOOL(3, "Clock", "Hour vibration", mHourly.mVib),
    INT(4, Kind::Int, "Clock", "First hour", mHourly.mFirst, 0, 24),
    INT(5, Kind::Int, "Clock", "Last hour", mHourly.mLast, 0, 24),

    LIST(10, "Watchface", "Style", mWatchface.mType, kWatchfaces),
    BOOL(11, "Watchface", "Battery %", mWatchface.mConfig.mBattery),
    BOOL(12, "Watchface", "Moon phases", mWatchface.mConfig.mMoon),
    BOOL(13, "Watchface", "Sunrise/sunset", mWatchface.mConfig.mSun),
    BOOL(14, "Watchface", "Tides", mWatchface.mConfig.mTides),
    BOOL(15, "Watchface", "Debug mode", mWatchface.mDebug),

    BOOL(20, "Display", "Invert", mDisplay.mInvert),
    BOOL(21, "Display", "Dark border", mDisplay.mDarkBorder),
    BOOL(22, "Display", "Flip 180", mDisplay.mFlip),
    ENUM(23, "Display", "Menu refresh", mDisplay.mMenuLut),
    ENUM(24, "Display", "Watchface refresh", mDisplay.mWatchLut),

    BOOL(30, "Power", "Night mode (0-6am)", mPower.mNight),
    BOOL(31, "Power", "Auto saving (<25%)", mPower.mAuto),
    BOOL(32, "Power", "Low voltage", mPower.mLowVoltage),

    BOOL(40, "Touch", "Haptic", mTouch.mHaptic),
    BOOL(44, "Touch", "Button beep", mTouch.mBeep),
    INT(41, Kind::Int, "Touch", "Sensitivity", mTouch.mSensitivity, 40, 80),
    ENUM(42, "Touch", "Menu rate", mTouch.mRate[0]),
    ENUM(43, "Touch", "Watchface rate", mTouch.mRate[1]),

    ENUM(50, "Bluetooth", "Auto sync", mBle.mEvery),
};

#undef BOOL
#undef INT
#undef LIST
#undef ENUM

const Setting* find(uint8_t id) {
    for (auto& s : kAll)
        if (s.mId == id)
            return &s;
    return nullptr;
}

void put16(std::vector<uint8_t>& out, int16_t v) {
    out.push_back(v & 0xFF);
    out.push_back((uint16_t)v >> 8);
}

void putString(std::vector<uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

int16_t get16(const uint8_t* p) { return (int16_t)(p[0] | p[1] << 8); }

} // namespace

std::span<const Setting> all() { return kAll; }

std::vector<uint8_t> schema() {
    std::vector<uint8_t> out;
    out.reserve(1024);
    out.push_back(kSchemaVersion);
    out.push_back(std::size(kAll));
    for (auto& s : kAll) {
        out.push_back(s.mId);
        out.push_back((uint8_t)s.mKind);
        put16(out, s.mMin);
        put16(out, s.mMax);
        putString(out, s.mGroup);
        putString(out, s.mName);
        // Options joined by '|', enum names lose magic_enum's leading '_'
        for (auto i = 0; i < s.mOptions.size(); i++) {
            auto label = s.mOptions[i];
            if (label.starts_with('_'))
                label.remove_prefix(1);
            if (i)
                out.push_back('|');
            out.insert(out.end(), label.begin(), label.end());
        }
        out.push_back(0);
    }
    return out;
}

uint32_t hash(std::span<const uint8_t> data) {
    return esp_rom_crc32_le(0, data.data(), data.size());
}

std::vector<uint8_t> values() {
    std::vector<uint8_t> out;
    out.reserve(std::size(kAll) * 3);
    for (auto& s : kAll) {
        out.push_back(s.mId);
        put16(out, s.mGet());
    }
    return out;
}

void overlay(std::vector<uint8_t>& values, std::span<const uint8_t> changes) {
    for (size_t i = 0; i + 3 <= changes.size(); i += 3) {
        auto* s = find(changes[i]);
        if (!s)
            continue;
        const auto v = s->clamp(get16(&changes[i + 1]));
        for (size_t j = 0; j + 3 <= values.size(); j += 3) {
            if (values[j] == s->mId) {
                values[j + 1] = v & 0xFF;
                values[j + 2] = (uint16_t)v >> 8;
            }
        }
    }
}

bool apply(std::span<const uint8_t> changes) {
    bool changed = false;
    for (size_t i = 0; i + 3 <= changes.size(); i += 3) {
        auto* s = find(changes[i]);
        if (!s)
            continue;
        const auto v = s->clamp(get16(&changes[i + 1]));
        if (s->mGet() != v) {
            s->mSet(v);
            changed = true;
        }
    }
    return changed;
}

} // namespace RemoteSettings
