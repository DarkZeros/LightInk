#pragma once

#include <cstdint>
#include <ctime>
#include <span>
#include <string_view>
#include <vector>

/* One description of the persistent settings in kSettings, and one encoding for them,
 * shared by the three places they live:
 *   - RTC memory (kSettings): what the firmware reads and writes
 *   - flash (NVS): saved at most once an hour when changed, loaded on a cold boot
 *   - the phone app: values over Bluetooth, and a schema it builds its screens from
 *
 * The encoding is a list of records: id u8, length u8, data. Numbers are signed little
 * endian integers of 1, 2 or 4 bytes; a few small fixed structs go as raw bytes. A
 * reader skips ids it doesn't know and records of the wrong length, and clamps numbers
 * to their range, so a blob stored by an older or newer firmware still loads.
 *
 * Ids are stable: never reuse or renumber one, only add new ones. Wire details:
 * docs/ble_protocol.md.
 */
namespace SettingsTable {

enum class Kind : uint8_t {
    Bool,
    Int,
    Enum,      // Value is the index in the options
    TzMinutes, // Int in minutes, the app shows it as a UTC offset
    Blob,      // Raw bytes of a small struct, stored only
};

enum Flags : uint8_t {
    kApp = 0x01, // In the schema, sent to the app and changeable from it. All are stored
};

enum class Scope : uint8_t {
    App,   // Only the entries the app sees
    Store, // Everything that goes to flash
};

struct Setting {
    uint8_t mId;
    Kind mKind;
    uint8_t mFlags;
    const char* mGroup;
    const char* mName;
    int32_t mMin, mMax;
    std::span<const std::string_view> mOptions; // Enum labels
    uint8_t mSize; // Record data bytes: 1, 2 or 4 for numbers, the struct size for a blob
    // Numbers
    int32_t (*mGet)();
    void (*mSet)(int32_t);
    // Blobs: the bytes in kSettings, and a check of incoming ones (may be null)
    std::span<uint8_t> (*mBytes)();
    bool (*mValid)(std::span<const uint8_t>);

    int32_t clamp(int32_t v) const { return v < mMin ? mMin : v > mMax ? mMax : v; }
};

std::span<const Setting> all();

// Schema of the app entries, and a hash of any blob (CRC-32)
std::vector<uint8_t> schema();
uint32_t hash(std::span<const uint8_t> data);

// Records of the current values
std::vector<uint8_t> values(Scope scope);

// Applies `changes` records to a values(App) blob as apply() would, without touching
// kSettings. App entries only
void overlay(std::vector<uint8_t>& values, std::span<const uint8_t> changes);

// Applies records to kSettings, entries outside `scope` ignored. True if anything changed
bool apply(std::span<const uint8_t> records, Scope scope);

// Cold boot: the settings stored in flash, if any, over the defaults
void load();
// Saves to flash if an hour passed since the last check and the values changed.
// Otherwise only compares a time: no flash access on normal wakes
void save(time_t now);

} // namespace SettingsTable
