#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/* The settings the phone app can see and change, and their wire format
 * (docs/ble_protocol.md). The app has no list of its own: the watch sends it a
 * description (the schema) and the app builds its screens from it, so a new entry
 * here shows up in the app without changing the app.
 *
 * Ids are stable: never reuse or renumber one, only add new ones.
 */
namespace RemoteSettings {

enum class Kind : uint8_t {
    Bool,
    Int,
    Enum,      // Value is the index in the options
    TzMinutes, // Int in minutes, the app shows it as a UTC offset
};

struct Setting {
    uint8_t mId;
    Kind mKind;
    const char* mGroup;
    const char* mName;
    int16_t mMin, mMax;
    std::span<const std::string_view> mOptions; // Enum labels
    int16_t (*mGet)();
    void (*mSet)(int16_t);

    int16_t clamp(int16_t v) const { return v < mMin ? mMin : v > mMax ? mMax : v; }
};

std::span<const Setting> all();

// Schema blob and its hash (the phone gets it again when the hash changes)
std::vector<uint8_t> schema();
uint32_t hash(std::span<const uint8_t> data);

// Every value as (id u8, value i16 LE) triples
std::vector<uint8_t> values();

// Applies (id, value) triples to `values` (a values() blob), out of range clamped and
// unknown ids skipped, without touching kSettings
void overlay(std::vector<uint8_t>& values, std::span<const uint8_t> changes);

// Applies (id, value) triples to kSettings. Returns true if anything changed
bool apply(std::span<const uint8_t> changes);

} // namespace RemoteSettings
