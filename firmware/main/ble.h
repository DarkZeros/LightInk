#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>

class Cancel;

/* Bluetooth link with the Android companion app (android/ in the repo root).
 *
 * The watch is the BLE central and the phone the peripheral: the phone advertises all
 * the time and hosts the GATT server, the watch only turns its radio on for a moment
 * and decides when to talk. A wake that syncs scans while the display refreshes; if the
 * phone isn't heard by the time the refresh ends the radio goes off again. The phone's
 * advertisement says whether it has changes, so when neither side has anything new
 * there is no connection at all. Wire format: docs/ble_protocol.md.
 */

// How often the watch syncs on its own. From 1h up it rides on wakes that boot anyway
// (touch or the hourly full wake); shorter intervals schedule full wakes of their own
enum SyncEvery : uint8_t {
    Off,
    _5min,
    _15min,
    _1h,
    _6h,
};

enum class BleResult : uint8_t {
    Never,     // No attempt yet
    NoPhone,   // Phone not heard
    Seen,      // Phone heard, nothing to exchange
    Synced,    // Exchange done
    NeedsPair, // The phone dropped the bond, pair again
    Failed,    // Radio or protocol error
    Cancelled,
};

// The paired phone. Saved in flash as well (pairing survives a reset), the RTC copy
// avoids reading flash on each wake
struct BlePeer {
    uint32_t mToken;         // Random, picked by the watch. The phone advertises it
    std::array<uint8_t, 7> mAddr; // Phone identity address (type + 6 bytes), info only
    // Link key: the phone advertises from a rotating private address, this lets the
    // watch encrypt without resolving it (the ESP32 controller has no LL privacy)
    std::array<uint8_t, 16> mLtk;
    uint64_t mRand;
    uint16_t mEdiv;
    uint8_t mKeySize;
    bool mAuthenticated;
};

struct BleSettings {
    SyncEvery mEvery {_1h};
    std::optional<BlePeer> mPeer;

    // UTC seconds, 0 = never
    time_t mLastTry {0};
    time_t mLastSeen {0};
    time_t mLastSync {0};
    BleResult mResult {BleResult::Never};

    // What the phone already holds, so unchanged data isn't sent again. 0 = unknown
    uint32_t mSentValues {0}; // CRC of the values
    uint32_t mSentSchema {0}; // Hash of the settings description

    // GATT handles of the phone's service, saves the discovery while they still match
    struct Handles {
        uint16_t mService {0}, mState {0}, mPending {0}, mSchema {0};
    } mHandles;
};

namespace Ble {

// Reads the pairing from flash. Cold boot only: the RTC copy is kept after that
void load();

// Whether a normal wake should sync on its own now (paired, interval elapsed)
bool due(time_t now);

// Seconds to the next automatic sync, when the interval is under an hour: the caller
// schedules a full wake for it, the hourly one covers the longer intervals
std::optional<uint32_t> nextSyncIn(time_t now);

// What the app shows about the watch, from this wake
struct Watch {
    time_t mNow;           // UTC seconds
    uint16_t mBatteryMv;
    uint16_t mBatteryPermille;
};

struct Session;

// Background sync meant to overlap the display refresh. Created before the draw,
// finish() right after it
class Sync {
public:
    explicit Sync(const Watch& watch);
    ~Sync();
    // Gives up at once if the phone wasn't heard yet, otherwise lets the exchange end
    // (bounded). Returns true if the phone changed any setting: they are applied then
    bool finish();
private:
    std::unique_ptr<Session> mSession;
    bool mFinished {false};
};

// Blocking flows for the menu, BACK cancels them. showCode is called (from another
// task) with the 6 digits the phone asks for
BleResult pair(const Watch& watch, const Cancel& cancel, std::function<void(uint32_t)> showCode);
BleResult syncNow(const Watch& watch, const Cancel& cancel, bool& changed);
// Drops the pairing (the phone keeps its side until it is forgotten there)
void forget();

} // namespace Ble
