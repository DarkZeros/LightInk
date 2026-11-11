#include "ble.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <magic_enum.hpp>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "peripherals.h"
#include "power.h"
#include "settings_table.h"
#include "storage.h"
#include "settings.h"

extern "C" void ble_store_config_init(void);

// Arduino's initArduino() (the WiFi code runs it) frees the BLE controller memory unless
// a BLE library says it's in use
extern "C" bool bleInUse(void) { return true; }

namespace Ble {
namespace {

constexpr const char* TAG = "ble";
// Debug: one line per session with its result and timings, and the step that failed
// ("ble: Synced sync 292 found 2125 conn 2410 enc 13352 done 13547 ms")
constexpr bool kLogSessions = false;

// Phone advertisement, manufacturer specific data (docs/ble_protocol.md)
constexpr uint8_t kProto = 2;
constexpr uint8_t kAdvLen = 10; // Company id (2), "LK" (2), proto, flags, token (4)
enum AdvFlag : uint8_t {
    kPairing = 0x01,    // App is on its pairing screen, token is 0
    kPending = 0x02,    // Changes made in the app are waiting for the watch
    kWantState = 0x04,  // App has no values from the watch (fresh install, ...)
    kWantSchema = 0x08, // App has no settings description, or an older one
};
enum StateFlag : uint8_t {
    kJustPaired = 0x01,
};

// 4c494748-5449-4e4b-0000-0000000000xx, "LIGHTINK", byte order reversed
#define LIGHTINK_UUID(n) BLE_UUID128_INIT(n, 0, 0, 0, 0, 0, 0, 0, 0x4b, 0x4e, 0x49, 0x54, 0x48, 0x47, 0x49, 0x4c)
const ble_uuid128_t kServiceUuid = LIGHTINK_UUID(0x00);
const ble_uuid128_t kStateUuid = LIGHTINK_UUID(0x01);   // Watch writes its values
const ble_uuid128_t kPendingUuid = LIGHTINK_UUID(0x02); // Watch reads the app's changes
const ble_uuid128_t kSchemaUuid = LIGHTINK_UUID(0x03);  // Watch writes the description
#undef LIGHTINK_UUID

// Android advertises every 100 ms at best (ADVERTISE_MODE_LOW_LATENCY). Connecting
// takes the advertising event after the one that was heard
constexpr int kConnectMs = 300;
// Hard cap of a background sync, from radio on to radio off, for a phone heard late
// or a slow exchange
constexpr int64_t kAutoMaxUs = 1'200'000;
constexpr int64_t kManualMaxUs = 10'000'000;
constexpr int64_t kPairFindUs = 60'000'000;
constexpr int64_t kPairCodeUs = 90'000'000; // Typing the code on the phone
constexpr int64_t kStepUs = 2'000'000;      // Any single GATT step
// Connect even with nothing new, so the app sees the watch (battery) once a day
constexpr time_t kRefreshEvery = 24 * 3600;

constexpr const char* kPeerKey = "ble_peer";

enum : EventBits_t {
    kEvSynced = 1 << 0,
    kEvFound = 1 << 1,
    kEvConnected = 1 << 2,
    kEvDisconnected = 1 << 3,
    kEvEncrypted = 1 << 4,
    kEvMtu = 1 << 5,
    kEvGatt = 1 << 6,
    kEvDone = 1 << 7,
};

// Its own key, apart from the settings blob: a key, written only when pairing
void savePeer(const std::optional<BlePeer>& peer) {
    static_assert(std::is_trivially_copyable_v<BlePeer>);
    if (peer)
        Storage::write(kPeerKey, {reinterpret_cast<const uint8_t*>(&*peer), sizeof(BlePeer)});
    else
        Storage::erase(kPeerKey);
}

uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x); v.push_back(x >> 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x); put16(v, x >> 16); }

} // namespace

enum class Mode { Auto, Manual, Pair };

/* One radio session: the stack comes up, the phone is searched, and if there is
 * something to exchange the watch connects, encrypts, reads the app's changes and
 * writes its values. Runs in its own task, NimBLE calls back from the host task
 */
struct Session {
    Session(Mode mode, const Watch& watch)
    : mMode(mode), mNow(watch.mNow), mBatteryMv(watch.mBatteryMv)
    , mBatteryPermille(watch.mBatteryPermille), mEvents(xEventGroupCreate()) {}
    ~Session() { vEventGroupDelete(mEvents); }

    const Mode mMode;
    const time_t mNow;
    const uint16_t mBatteryMv, mBatteryPermille;
    EventGroupHandle_t mEvents;
    const Cancel* mCancel {nullptr};
    std::function<void(uint32_t)> mShowCode;
    std::atomic<bool> mGiveUp {false}; // Stop searching (display refresh ended)

    // Set up by the caller before start()
    std::optional<BlePeer> mPeer;
    BleSettings::Handles mHandles;
    std::vector<uint8_t> mValues, mSchema;
    uint32_t mSchemaHash {0};
    bool mSendValues {false}, mSendSchema {false}, mStale {false};

    // Results, read once the task is done
    BleResult mResult {BleResult::Failed};
    bool mFound {false};
    bool mValuesSent {false}, mSchemaSent {false};
    std::vector<uint8_t> mChanges; // From the app, applied by the caller
    std::optional<BlePeer> mNewPeer;
    // Timing, ms from start (for the log)
    int mTSynced {-1}, mTFound {-1}, mTConnected {-1}, mTEncrypted {-1}, mTDone {-1};

    void start() {
        xTaskCreatePinnedToCore([](void* arg) {
            auto& s = *static_cast<Session*>(arg);
            s.mResult = s.run();
            xEventGroupSetBits(s.mEvents, kEvDone);
            vTaskDelete(nullptr);
        }, "ble", 4096, this, 5, nullptr, tskNO_AFFINITY);
    }
    void join() { xEventGroupWaitBits(mEvents, kEvDone, pdFALSE, pdTRUE, portMAX_DELAY); }

private:
    int64_t mStartUs {esp_timer_get_time()};
    int64_t mDeadlineUs {0};
    bool mStackUp {false}, mHostUp {false};
    uint8_t mOwnAddrType {0};
    uint8_t mAdvFlags {0};
    bool mConnecting {false};
    uint16_t mConn {BLE_HS_CONN_HANDLE_NONE};
    uint16_t mMtu {BLE_ATT_MTU_DFLT};
    int mStatus {0}; // Of the last connect/encrypt/GATT step
    const char* mStep {"start"}; // Where the session is, for the log when it fails
    uint16_t mSvcStart {0}, mSvcEnd {0};
    std::vector<uint8_t> mRead;

    static inline Session* sActive {nullptr};

    int ms() const { return (esp_timer_get_time() - mStartUs) / 1000; }

    // Waits for any of `bits` until the deadline. Fails early on BACK, on a lost
    // link and, while searching, when the caller gives up
    bool wait(EventBits_t bits, int64_t deadlineUs, bool searching = false) {
        while (true) {
            const auto now = esp_timer_get_time();
            if (now >= deadlineUs)
                return false;
            const auto slice = std::min<int64_t>(deadlineUs - now, 50'000);
            const auto got = xEventGroupWaitBits(mEvents, bits | kEvDisconnected, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(slice / 1000) + 1);
            if (got & bits) {
                xEventGroupClearBits(mEvents, bits);
                return true;
            }
            if (got & kEvDisconnected)
                return false;
            if (mCancel && *mCancel) {
                mResult = BleResult::Cancelled;
                return false;
            }
            if (searching && mGiveUp)
                return false;
        }
    }
    int64_t step() const { return std::min(esp_timer_get_time() + kStepUs, mDeadlineUs); }

    bool startStack() {
        Storage::init(); // PHY calibration data; without it the radio still works, slower
        if (nimble_port_init() != ESP_OK)
            return false;
        mStackUp = true;

        sActive = this;
        ble_hs_cfg.sync_cb = [] { xEventGroupSetBits(sActive->mEvents, kEvSynced); };
        ble_hs_cfg.reset_cb = [](int reason) { ESP_LOGE(TAG, "host reset %d", reason); };
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        // The watch shows the code, the phone asks for it
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_store_config_init(); // RAM only: the key that matters is saved in BlePeer
        ble_store_clear();       // Nothing from an earlier session of this wake
        // Name ("LightInk") and appearance (watch) for Android, which reads them after
        // pairing and shows the MAC without them
        ble_svc_gap_init();

        nimble_port_freertos_init([](void*) {
            nimble_port_run(); // Until nimble_port_stop()
            nimble_port_freertos_deinit();
        });
        mHostUp = true;
        if (!wait(kEvSynced, esp_timer_get_time() + 1'000'000))
            return false;
        mTSynced = ms();
        return ble_hs_id_infer_auto(0, &mOwnAddrType) == 0;
    }

    void stopStack() {
        if (mHostUp)
            nimble_port_stop();
        if (mStackUp)
            nimble_port_deinit();
        sActive = nullptr;
    }

    bool needConnect() const {
        return mMode != Mode::Auto || mSendValues || mSendSchema || mStale
            || (mAdvFlags & (kPending | kWantState | kWantSchema));
    }

    static int onGap(ble_gap_event* e, void* arg) {
        auto& s = *static_cast<Session*>(arg);
        switch (e->type) {
        case BLE_GAP_EVENT_DISC:
            s.onAdvertisement(e->disc);
            break;
        case BLE_GAP_EVENT_CONNECT:
            s.mStatus = e->connect.status;
            if (e->connect.status == 0) {
                s.mConn = e->connect.conn_handle;
                s.knowKey();
            }
            xEventGroupSetBits(s.mEvents, kEvConnected);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s.mConn = BLE_HS_CONN_HANDLE_NONE;
            xEventGroupSetBits(s.mEvents, kEvDisconnected);
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            s.mStatus = e->enc_change.status;
            xEventGroupSetBits(s.mEvents, kEvEncrypted);
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (e->passkey.params.action == BLE_SM_IOACT_DISP && s.mMode == Mode::Pair) {
                ble_sm_io io{};
                io.action = BLE_SM_IOACT_DISP;
                io.passkey = esp_random() % 1'000'000;
                ble_sm_inject_io(e->passkey.conn_handle, &io);
                if (s.mShowCode)
                    s.mShowCode(io.passkey);
            }
            break;
        case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
            // Android asks for 45 ms soon after connecting: the exchange then took 6x
            // longer on air. The session is short, keep the 7.5-15 ms it was opened with
            return BLE_ERR_CONN_PARMS;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // Only reached when pairing again within a session: drop the old keys
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &desc) == 0)
                ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            break;
        }
        return 0;
    }

    void onAdvertisement(const ble_gap_disc_desc& d) {
        if (mFound || d.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND)
            return;
        ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) != 0 || f.mfg_data_len < kAdvLen)
            return;
        const auto* m = f.mfg_data;
        if (m[0] != 0xFF || m[1] != 0xFF || m[2] != 'L' || m[3] != 'K' || m[4] != kProto)
            return;
        const uint8_t flags = m[5];
        const uint32_t token = get32(m + 6);
        if (mMode == Mode::Pair ? !(flags & kPairing)
                                : (flags & kPairing) || !mPeer || token != mPeer->mToken)
            return;

        mFound = true;
        mAdvFlags = flags;
        mTFound = ms();
        ble_gap_disc_cancel();
        // Connect right from here, the next advertising event is already counting
        if (needConnect()) {
            ble_gap_conn_params p{};
            p.scan_itvl = p.scan_window = 48; // 30 ms, continuous
            p.itvl_min = 6;                   // 7.5 ms: few round trips, done quickly
            p.itvl_max = 12;
            p.latency = 0;
            p.supervision_timeout = 50; // 500 ms
            mStatus = ble_gap_connect(mOwnAddrType, &d.addr, kConnectMs, &p, onGap, this);
            mConnecting = mStatus == 0;
        }
        xEventGroupSetBits(mEvents, kEvFound);
    }

    // GATT callbacks: each procedure ends with one kEvGatt, status in mStatus
    void gattDone(int status) {
        mStatus = status == BLE_HS_EDONE ? 0 : status;
        xEventGroupSetBits(mEvents, kEvGatt);
    }
    bool gatt(int rc) {
        if (rc != 0) {
            mStatus = rc;
            return false;
        }
        return wait(kEvGatt, step()) && mStatus == 0;
    }

    bool findHandles() {
        mSvcStart = 0;
        auto svcCb = [](uint16_t, const ble_gatt_error* err, const ble_gatt_svc* svc, void* arg) {
            auto& s = *static_cast<Session*>(arg);
            if (err->status == 0 && svc && !s.mSvcStart) {
                s.mSvcStart = svc->start_handle;
                s.mSvcEnd = svc->end_handle;
            } else if (err->status != 0) {
                s.gattDone(err->status);
            }
            return 0;
        };
        if (!gatt(ble_gattc_disc_svc_by_uuid(mConn, &kServiceUuid.u, svcCb, this)) || !mSvcStart)
            return false;
        // Android numbers a service's attributes in a row: same start, same handles
        if (mHandles.mService == mSvcStart && mHandles.mState && mHandles.mPending && mHandles.mSchema)
            return true;

        mHandles = {.mService = mSvcStart};
        auto chrCb = [](uint16_t, const ble_gatt_error* err, const ble_gatt_chr* chr, void* arg) {
            auto& s = *static_cast<Session*>(arg);
            if (err->status == 0 && chr) {
                if (ble_uuid_cmp(&chr->uuid.u, &kStateUuid.u) == 0)
                    s.mHandles.mState = chr->val_handle;
                else if (ble_uuid_cmp(&chr->uuid.u, &kPendingUuid.u) == 0)
                    s.mHandles.mPending = chr->val_handle;
                else if (ble_uuid_cmp(&chr->uuid.u, &kSchemaUuid.u) == 0)
                    s.mHandles.mSchema = chr->val_handle;
            } else if (err->status != 0) {
                s.gattDone(err->status);
            }
            return 0;
        };
        return gatt(ble_gattc_disc_all_chrs(mConn, mSvcStart, mSvcEnd, chrCb, this))
            && mHandles.mState && mHandles.mPending && mHandles.mSchema;
    }

    bool read(uint16_t handle) {
        mRead.clear();
        auto cb = [](uint16_t, const ble_gatt_error* err, ble_gatt_attr* attr, void* arg) {
            auto& s = *static_cast<Session*>(arg);
            if (err->status == 0 && attr) {
                const auto len = OS_MBUF_PKTLEN(attr->om);
                const auto at = s.mRead.size();
                s.mRead.resize(at + len);
                os_mbuf_copydata(attr->om, 0, len, s.mRead.data() + at);
            } else if (err->status != 0) {
                s.gattDone(err->status);
            }
            return 0;
        };
        return gatt(ble_gattc_read_long(mConn, handle, 0, cb, this));
    }

    // Plain writes of (offset u16, total u16, data) pieces, the app joins them. ATT long
    // writes aborted on a Galaxy (Android 17): its echo of the second prepared piece
    // didn't pass NimBLE's check
    bool write(uint16_t handle, const std::vector<uint8_t>& data) {
        auto cb = [](uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void* arg) {
            static_cast<Session*>(arg)->gattDone(err->status);
            return 0;
        };
        const size_t room = mMtu - 3u - 4u;
        size_t at = 0;
        std::vector<uint8_t> piece;
        do {
            const size_t n = std::min(room, data.size() - at);
            piece.clear();
            put16(piece, at);
            put16(piece, data.size());
            piece.insert(piece.end(), data.begin() + at, data.begin() + at + n);
            if (!gatt(ble_gattc_write_flat(mConn, handle, piece.data(), piece.size(), cb, this)))
                return false;
            at += n;
        } while (at < data.size());
        return true;
    }

    // Hands the stored key to NimBLE for this connection, before any packet of it is
    // handled. With no key in its store NimBLE answers a security request of the phone
    // with a new pairing: Android then asks for a code and drops its bond when the
    // watch hangs up. Only the Pair menu may pair
    void knowKey() {
        ble_gap_conn_desc desc;
        if (mMode == Mode::Pair || !mPeer || ble_gap_conn_find(mConn, &desc) != 0)
            return;
        ble_store_value_sec sec{};
        sec.peer_addr = desc.peer_id_addr; // The phone's address of the day, not resolved
        sec.key_size = mPeer->mKeySize;
        sec.ediv = mPeer->mEdiv;
        sec.rand_num = mPeer->mRand;
        std::copy_n(mPeer->mLtk.begin(), 16, sec.ltk);
        sec.ltk_present = 1;
        sec.authenticated = mPeer->mAuthenticated;
        sec.sc = 1;
        ble_store_write_peer_sec(&sec);
    }

    // Keys of the bond that was just made, for the next sessions
    bool keepBond() {
        ble_gap_conn_desc desc;
        if (ble_gap_conn_find(mConn, &desc) != 0)
            return false;
        ble_store_key_sec key{};
        key.peer_addr = desc.peer_id_addr;
        ble_store_value_sec sec{};
        if ((ble_store_read_peer_sec(&key, &sec) != 0 || !sec.ltk_present)
            && (ble_store_read_our_sec(&key, &sec) != 0 || !sec.ltk_present))
            return false;
        auto& p = mNewPeer.emplace();
        do {
            p.mToken = esp_random();
        } while (p.mToken == 0);
        p.mAddr[0] = desc.peer_id_addr.type;
        std::copy_n(desc.peer_id_addr.val, 6, p.mAddr.begin() + 1);
        std::copy_n(sec.ltk, 16, p.mLtk.begin());
        p.mRand = sec.rand_num;
        p.mEdiv = sec.ediv;
        p.mKeySize = sec.key_size;
        p.mAuthenticated = sec.authenticated;
        mPeer = mNewPeer;
        return true;
    }

    std::vector<uint8_t> state(uint16_t ackRev) const {
        std::vector<uint8_t> out;
        out.reserve(20 + mValues.size());
        out.push_back(kProto);
        out.push_back(mMode == Mode::Pair ? kJustPaired : 0);
        put32(out, mPeer->mToken);
        put32(out, mSchemaHash);
        put16(out, ackRev);
        put16(out, mBatteryMv);
        put16(out, mBatteryPermille);
        put32(out, mNow);
        out.insert(out.end(), mValues.begin(), mValues.end());
        return out;
    }

    BleResult talk() {
        mDeadlineUs = mStartUs + (mMode == Mode::Auto ? kAutoMaxUs
                                  : mMode == Mode::Manual ? kManualMaxUs : kPairFindUs);

        // 1. Search the phone. Continuous passive scan: the phone advertises every 100 ms
        mStep = "scan";
        ble_gap_disc_params disc{};
        disc.itvl = disc.window = 48; // 30 ms
        disc.passive = 1;
        disc.filter_duplicates = 1;
        if (ble_gap_disc(mOwnAddrType, BLE_HS_FOREVER, &disc, onGap, this) != 0)
            return BleResult::Failed;
        if (!wait(kEvFound, mDeadlineUs, true)) {
            ble_gap_disc_cancel();
            return mResult == BleResult::Cancelled ? mResult : BleResult::NoPhone;
        }
        if (!mConnecting)
            return needConnect() ? BleResult::Failed : BleResult::Seen;

        // 2. Connect, the phone is there: the cap applies from now on, not the refresh
        mStep = "connect";
        if (!wait(kEvConnected, esp_timer_get_time() + kConnectMs * 1000 + 100'000) || mStatus != 0)
            return BleResult::Failed;
        mTConnected = ms();
        if (mMode == Mode::Pair)
            mDeadlineUs = esp_timer_get_time() + kPairCodeUs;

        // 3. MTU exchange runs during the encryption
        auto mtuCb = [](uint16_t, const ble_gatt_error* err, uint16_t mtu, void* arg) {
            auto& s = *static_cast<Session*>(arg);
            if (err->status == 0)
                s.mMtu = mtu;
            xEventGroupSetBits(s.mEvents, kEvMtu);
            return 0;
        };
        const bool mtu = ble_gattc_exchange_mtu(mConn, mtuCb, this) == 0;

        // 4. Encrypt: a pairing (the phone asks for the code) or the stored key
        mStep = "encrypt";
        const int rc = mMode == Mode::Pair
            ? ble_gap_security_initiate(mConn)
            : ble_gap_encryption_initiate(mConn, mPeer->mKeySize, mPeer->mLtk.data(), mPeer->mEdiv,
                                          mPeer->mRand, mPeer->mAuthenticated);
        if (rc != 0 || !wait(kEvEncrypted, mMode == Mode::Pair ? mDeadlineUs : step()))
            return mResult == BleResult::Cancelled ? mResult : BleResult::Failed;
        if (mStatus != 0) {
            // The phone lost its key (forgotten in Android's settings). Reported only
            // because NimBLE's own new pairing on this error is off (main/CMakeLists.txt)
            return mStatus == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) && mMode != Mode::Pair
                ? BleResult::NeedsPair : BleResult::Failed;
        }
        mTEncrypted = ms();
        if (mMode == Mode::Pair) {
            mStep = "bond";
            if (!keepBond())
                return BleResult::Failed;
            mDeadlineUs = esp_timer_get_time() + kManualMaxUs;
        }
        if (mtu)
            wait(kEvMtu, step());

        // 5. Exchange
        mStep = "discover";
        if (!findHandles())
            return BleResult::Failed;
        uint16_t ackRev = 0;
        if (mAdvFlags & kPending) {
            mStep = "read";
            if (!read(mHandles.mPending) || mRead.size() < 3 || mRead[0] != kProto)
                return BleResult::Failed;
            ackRev = mRead[1] | mRead[2] << 8;
            mChanges.assign(mRead.begin() + 3, mRead.end());
            // Send the values as they will be once applied
            SettingsTable::overlay(mValues, mChanges);
        }
        if (mSendSchema || (mAdvFlags & kWantSchema)) {
            mStep = "schema";
            if (!write(mHandles.mSchema, mSchema))
                return BleResult::Failed;
            mSchemaSent = true;
        }
        mStep = "state";
        if (!write(mHandles.mState, state(ackRev)))
            return BleResult::Failed;
        mValuesSent = true;
        return BleResult::Synced;
    }

    BleResult run() {
        Power::Lock power(Power::Flag::BT);
        auto result = startStack() ? talk() : BleResult::Failed;
        if (mConn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(mConn, BLE_ERR_REM_USER_CONN_TERM);
            wait(kEvDisconnected, esp_timer_get_time() + 200'000);
        }
        stopStack();
        mTDone = ms();
        if constexpr (kLogSessions) {
            ESP_LOGE(TAG, "%s sync %d found %d conn %d enc %d done %d ms",
                     std::string(magic_enum::enum_name(result)).c_str(),
                     mTSynced, mTFound, mTConnected, mTEncrypted, mTDone);
            if (result == BleResult::Failed || result == BleResult::NeedsPair)
                ESP_LOGE(TAG, "failed at %s, status %d", mStep, mStatus);
        }
        return result;
    }
};

namespace {

time_t interval(SyncEvery every) {
    switch (every) {
    case _5min: return 5 * 60;
    case _15min: return 15 * 60;
    case _1h: return 3600;
    case _6h: return 6 * 3600;
    case Off: default: return 0;
    }
}

// What the phone should get from this session
void prepare(Session& s) {
    auto& ble = kSettings.mBle;
    s.mPeer = ble.mPeer;
    s.mHandles = ble.mHandles;
    s.mValues = SettingsTable::values(SettingsTable::Scope::App);
    s.mSchema = SettingsTable::schema();
    s.mSchemaHash = SettingsTable::hash(s.mSchema);
    const bool pair = s.mMode == Mode::Pair;
    s.mSendValues = pair || SettingsTable::hash(s.mValues) != ble.mSentValues;
    s.mSendSchema = pair || s.mSchemaHash != ble.mSentSchema;
    s.mStale = s.mNow - ble.mLastSync >= kRefreshEvery;
}

// Back in the caller's task: record the outcome, apply the app's changes
bool commit(Session& s) {
    auto& ble = kSettings.mBle;
    if (s.mMode == Mode::Pair) {
        // A failed attempt leaves the old pairing as it was
        if (!s.mNewPeer || s.mResult != BleResult::Synced)
            return false;
        BleSettings fresh;
        fresh.mEvery = ble.mEvery;
        ble = fresh;
        ble.mPeer = s.mNewPeer;
        savePeer(ble.mPeer);
    }
    ble.mLastTry = s.mNow;
    if (s.mFound)
        ble.mLastSeen = s.mNow;
    ble.mResult = s.mResult;
    ble.mHandles = s.mHandles;
    const bool changed = SettingsTable::apply(s.mChanges, SettingsTable::Scope::App);
    if (s.mValuesSent)
        ble.mSentValues = SettingsTable::hash(s.mValues);
    if (s.mSchemaSent)
        ble.mSentSchema = s.mSchemaHash;
    if (s.mResult == BleResult::Synced)
        ble.mLastSync = s.mNow;
    return changed;
}

} // namespace

void load() {
    const auto data = Storage::read(kPeerKey);
    if (data.size() == sizeof(BlePeer)) {
        BlePeer peer;
        std::memcpy(&peer, data.data(), sizeof(peer));
        kSettings.mBle.mPeer = peer;
    }
}

bool due(time_t now) {
    const auto& ble = kSettings.mBle;
    const auto every = interval(ble.mEvery);
    if (!ble.mPeer || !every)
        return false;
    // A minute of slack: the hourly wake lands a few ms short of the hour
    return now < ble.mLastTry || now - ble.mLastTry >= every - 60;
}

std::optional<uint32_t> nextSyncIn(time_t now) {
    const auto& ble = kSettings.mBle;
    const auto every = interval(ble.mEvery);
    if (!ble.mPeer || !every || every >= 3600)
        return std::nullopt;
    const auto left = ble.mLastTry + every - now;
    return std::max<time_t>(60, std::min<time_t>(left, every));
}

Sync::Sync(const Watch& watch) : mSession(std::make_unique<Session>(Mode::Auto, watch)) {
    prepare(*mSession);
    mSession->start();
}

Sync::~Sync() {
    if (!mFinished)
        finish();
}

bool Sync::finish() {
    mFinished = true;
    mSession->mGiveUp = true;
    mSession->join();
    return commit(*mSession);
}

BleResult pair(const Watch& watch, const Cancel& cancel, std::function<void(uint32_t)> showCode) {
    Session s(Mode::Pair, watch);
    s.mCancel = &cancel;
    s.mShowCode = std::move(showCode);
    prepare(s);
    s.mHandles = {};
    s.start();
    s.join();
    commit(s);
    return s.mResult;
}

BleResult syncNow(const Watch& watch, const Cancel& cancel, bool& changed) {
    if (!kSettings.mBle.mPeer)
        return BleResult::NeedsPair;
    Session s(Mode::Manual, watch);
    s.mCancel = &cancel;
    prepare(s);
    s.start();
    s.join();
    changed = commit(s);
    return s.mResult;
}

void forget() {
    BleSettings fresh;
    fresh.mEvery = kSettings.mBle.mEvery;
    kSettings.mBle = fresh;
    savePeer(std::nullopt);
}

} // namespace Ble
