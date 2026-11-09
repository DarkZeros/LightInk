#include "settings_table.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include <magic_enum.hpp>

#include "settings.h"
#include "storage.h"

namespace SettingsTable {
namespace {

constexpr uint8_t kSchemaVersion = 2;
constexpr uint8_t kStoreFormat = 1; // First byte of the flash blob
constexpr const char* kStoreKey = "settings";
constexpr time_t kSaveEvery = 3600;
// Debug: log loads (with a round trip check) and saves
constexpr bool kLogStore = false;

constexpr std::string_view kWatchfaces[] = {"Default", "Eclipse"};

template<typename T>
using Field = std::remove_cvref_t<T>;

// Numbers: wire size is the field's size (bool and int8 1, int16 2, 32 bit 4)
#define NUM(id, kind, flags, group, name, field, lo, hi) \
    Setting{id, kind, flags, group, name, lo, hi, {}, sizeof(kSettings.field), \
        []() -> int32_t { return kSettings.field; }, \
        [](int32_t v) { kSettings.field = v; }, nullptr, nullptr}

#define BOOL(id, flags, group, name, field) NUM(id, Kind::Bool, flags, group, name, field, 0, 1)

#define LIST(id, flags, group, name, field, labels) \
    Setting{id, Kind::Enum, flags, group, name, 0, std::size(labels) - 1, labels, 1, \
        []() -> int32_t { return kSettings.field; }, \
        [](int32_t v) { kSettings.field = v; }, nullptr, nullptr}

// Wire value is the index in magic_enum's order, the labels are the enum names
#define ENUM(id, flags, group, name, field) \
    Setting{id, Kind::Enum, flags, group, name, 0, \
        magic_enum::enum_count<Field<decltype(kSettings.field)>>() - 1, \
        magic_enum::enum_names<Field<decltype(kSettings.field)>>(), 1, \
        []() -> int32_t { return magic_enum::enum_index(kSettings.field).value_or(0); }, \
        [](int32_t v) { kSettings.field = magic_enum::enum_value<Field<decltype(kSettings.field)>>(v); }, \
        nullptr, nullptr}

// Raw bytes of a small trivially copyable struct. A changed layout changes the size,
// and a record of the wrong size is skipped
#define BLOB(id, name, field, valid) \
    Setting{id, Kind::Blob, 0, "", name, 0, 0, {}, sizeof(kSettings.field), nullptr, nullptr, \
        []() { return std::span<uint8_t>(reinterpret_cast<uint8_t*>(&kSettings.field), \
                                         sizeof(kSettings.field)); }, valid}

static_assert(std::is_trivially_copyable_v<decltype(Settings::mAlarms)>);
static_assert(std::is_trivially_copyable_v<decltype(TouchSettings::mMap)>);
static_assert(std::is_trivially_copyable_v<decltype(BatterySettings::mScales)>);

bool allBelow(std::span<const uint8_t> d, uint8_t limit) {
    return std::all_of(d.begin(), d.end(), [&](uint8_t b) { return b < limit; });
}

const Setting kAll[] = {
    // Shown in the app
    NUM(1, Kind::TzMinutes, kApp, "Clock", "Time zone", mTime.mMinutesWest, -12 * 60, 14 * 60),
    BOOL(2, kApp, "Clock", "Hour beep", mHourly.mBeep),
    ENUM(6, kApp, "Clock", "Beep sound", mHourly.mSound),
    BOOL(3, kApp, "Clock", "Hour vibration", mHourly.mVib),
    NUM(4, Kind::Int, kApp, "Clock", "First hour", mHourly.mFirst, 0, 24),
    NUM(5, Kind::Int, kApp, "Clock", "Last hour", mHourly.mLast, 0, 24),

    LIST(10, kApp, "Watchface", "Style", mWatchface.mType, kWatchfaces),
    BOOL(11, kApp, "Watchface", "Battery %", mWatchface.mConfig.mBattery),
    BOOL(12, kApp, "Watchface", "Moon phases", mWatchface.mConfig.mMoon),
    BOOL(13, kApp, "Watchface", "Sunrise/sunset", mWatchface.mConfig.mSun),
    BOOL(14, kApp, "Watchface", "Tides", mWatchface.mConfig.mTides),
    BOOL(15, kApp, "Watchface", "Debug mode", mWatchface.mDebug),

    BOOL(20, kApp, "Display", "Invert", mDisplay.mInvert),
    BOOL(21, kApp, "Display", "Dark border", mDisplay.mDarkBorder),
    BOOL(22, kApp, "Display", "Flip 180", mDisplay.mFlip),
    ENUM(23, kApp, "Display", "Menu refresh", mDisplay.mMenuLut),
    ENUM(24, kApp, "Display", "Watchface refresh", mDisplay.mWatchLut),

    BOOL(30, kApp, "Power", "Night mode (0-6am)", mPower.mNight),
    BOOL(31, kApp, "Power", "Auto saving (<25%)", mPower.mAuto),
    BOOL(32, kApp, "Power", "Low voltage", mPower.mLowVoltage),

    BOOL(40, kApp, "Touch", "Haptic", mTouch.mHaptic),
    BOOL(44, kApp, "Touch", "Button beep", mTouch.mBeep),
    NUM(41, Kind::Int, kApp, "Touch", "Sensitivity", mTouch.mSensitivity, 40, 80),
    ENUM(42, kApp, "Touch", "Menu rate", mTouch.mRate[0]),
    ENUM(43, kApp, "Touch", "Watchface rate", mTouch.mRate[1]),

    ENUM(50, kApp, "Bluetooth", "Auto sync", mBle.mEvery),

    // Stored only: calibration and tuning the app has no screen for
    NUM(60, Kind::Int, 0, "", "RTC calibration", mTime.mCalibration,
        kDefaultCalibration / 10, kDefaultCalibration * 10),
    ENUM(61, 0, "", "Menu touch cycles", mTouch.mCycles[0]),
    ENUM(62, 0, "", "Watchface touch cycles", mTouch.mCycles[1]),
    BLOB(63, "Touch pad map", mTouch.mMap, [](std::span<const uint8_t> d) { return allBelow(d, 4); }),
    BLOB(64, "Battery scales", mBattery.mScales, nullptr),
    NUM(65, Kind::Int, 0, "", "Battery samples", mBattery.mSamples, 1, 64),
    NUM(66, Kind::Int, 0, "", "Battery averages", mBattery.mAverages, 1, 255),
    BLOB(67, "Alarms", mAlarms, [](std::span<const uint8_t> d) { return allBelow(d, 2); }),
};

#undef NUM
#undef BOOL
#undef LIST
#undef ENUM
#undef BLOB

bool inScope(const Setting& s, Scope scope) {
    return scope == Scope::Store || (s.mFlags & kApp);
}

const Setting* find(uint8_t id) {
    for (auto& s : kAll)
        if (s.mId == id)
            return &s;
    return nullptr;
}

void putNumber(std::vector<uint8_t>& out, int32_t v, uint8_t size) {
    for (uint8_t i = 0; i < size; i++)
        out.push_back((uint32_t)v >> (8 * i));
}

void putString(std::vector<uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
}

// Signed little endian of 1, 2 or 4 bytes
bool readNumber(std::span<const uint8_t> d, int32_t& v) {
    if (d.size() != 1 && d.size() != 2 && d.size() != 4)
        return false;
    uint32_t u = 0;
    for (size_t i = 0; i < d.size(); i++)
        u |= (uint32_t)d[i] << (8 * i);
    const auto shift = 32 - 8 * d.size();
    v = (int32_t)(u << shift) >> shift;
    return true;
}

// Calls f(id, data) for each well formed record, stops at a truncated one
template<typename F>
void forEach(std::span<const uint8_t> records, F f) {
    size_t i = 0;
    while (i + 2 <= records.size()) {
        const uint8_t id = records[i], len = records[i + 1];
        if (i + 2 + len > records.size())
            return;
        f(id, records.subspan(i + 2, len));
        i += 2 + len;
    }
}

std::vector<uint8_t> stored() {
    auto out = values(Scope::Store);
    out.insert(out.begin(), kStoreFormat);
    return out;
}

} // namespace

std::span<const Setting> all() { return kAll; }

uint32_t hash(std::span<const uint8_t> data) {
    return esp_rom_crc32_le(0, data.data(), data.size());
}

std::vector<uint8_t> schema() {
    std::vector<uint8_t> out;
    out.reserve(1024);
    out.push_back(kSchemaVersion);
    out.push_back(0); // Count, filled below
    for (auto& s : kAll) {
        if (!(s.mFlags & kApp))
            continue;
        out[1]++;
        out.push_back(s.mId);
        out.push_back((uint8_t)s.mKind);
        putNumber(out, s.mMin, 4);
        putNumber(out, s.mMax, 4);
        putString(out, s.mGroup);
        putString(out, s.mName);
        // Options joined by '|', enum names lose magic_enum's leading '_'
        for (size_t i = 0; i < s.mOptions.size(); i++) {
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

std::vector<uint8_t> values(Scope scope) {
    std::vector<uint8_t> out;
    out.reserve(std::size(kAll) * 4);
    for (auto& s : kAll) {
        if (!inScope(s, scope))
            continue;
        out.push_back(s.mId);
        out.push_back(s.mSize);
        if (s.mKind == Kind::Blob) {
            auto bytes = s.mBytes();
            out.insert(out.end(), bytes.begin(), bytes.end());
        } else {
            putNumber(out, s.mGet(), s.mSize);
        }
    }
    return out;
}

void overlay(std::vector<uint8_t>& values, std::span<const uint8_t> changes) {
    forEach(changes, [&](uint8_t id, std::span<const uint8_t> data) {
        auto* s = find(id);
        int32_t v;
        if (!s || !(s->mFlags & kApp) || s->mKind == Kind::Blob || !readNumber(data, v))
            return;
        std::vector<uint8_t> bytes;
        putNumber(bytes, s->clamp(v), s->mSize);
        // values() wrote this id with the same size
        for (size_t i = 0; i + 2 <= values.size(); i += 2 + values[i + 1]) {
            if (values[i] == id && values[i + 1] == s->mSize && i + 2 + s->mSize <= values.size())
                std::copy(bytes.begin(), bytes.end(), values.begin() + i + 2);
        }
    });
}

bool apply(std::span<const uint8_t> records, Scope scope) {
    bool changed = false;
    forEach(records, [&](uint8_t id, std::span<const uint8_t> data) {
        auto* s = find(id);
        if (!s || !inScope(*s, scope))
            return;
        if (s->mKind == Kind::Blob) {
            auto bytes = s->mBytes();
            if (data.size() != bytes.size() || (s->mValid && !s->mValid(data)))
                return;
            if (!std::equal(data.begin(), data.end(), bytes.begin())) {
                std::copy(data.begin(), data.end(), bytes.begin());
                changed = true;
            }
            return;
        }
        int32_t v;
        if (!readNumber(data, v))
            return;
        v = s->clamp(v);
        if (s->mGet() != v) {
            s->mSet(v);
            changed = true;
        }
    });
    return changed;
}

void load() {
    const auto data = Storage::read(kStoreKey);
    if (data.size() > 1 && data[0] == kStoreFormat)
        apply(std::span(data).subspan(1), Scope::Store);
    // What flash holds now, as this firmware would write it. Nothing stored: 0, so the
    // first save writes
    kSettings.mStore.mSavedHash = data.empty() ? 0 : hash(stored());
    // Same hash: the loaded settings write back to the exact bytes in flash
    if constexpr (kLogStore)
        ESP_LOGE("settings", "load %u B, flash %08lx, now %08lx", data.size(),
                 (unsigned long)hash(data), (unsigned long)kSettings.mStore.mSavedHash);
}

void save(time_t now) {
    auto& store = kSettings.mStore;
    // The hourly full wake lands a few ms short of the hour: a minute of slack
    if (store.mLastCheck && now >= store.mLastCheck && now - store.mLastCheck < kSaveEvery - 60)
        return;
    store.mLastCheck = now;
    const auto blob = stored();
    const auto h = hash(blob);
    const bool write = h != store.mSavedHash;
    const bool ok = write && Storage::write(kStoreKey, blob);
    if (ok)
        store.mSavedHash = h;
    if constexpr (kLogStore)
        ESP_LOGE("settings", "save check: %s %u B %08lx, read back %u B", !write ? "unchanged"
                 : ok ? "written" : "write failed", blob.size(), (unsigned long)h,
                 Storage::read(kStoreKey).size());
}

} // namespace SettingsTable
