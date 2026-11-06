#include "storage.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "power.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace Storage {
namespace {

constexpr const char* kNamespace = "lightink";
constexpr const char* TAG = "storage";
// Debug: every call with its result and the NVS entry counts
constexpr bool kLog = false;

void stats(const char* when) {
    nvs_stats_t st;
    if (nvs_get_stats(nullptr, &st) == ESP_OK)
        ESP_LOGE(TAG, "%s: used %u free %u total %u namespaces %u", when, st.used_entries,
                 st.free_entries, st.total_entries, st.namespace_count);
}

// Real errors only: a missing key or namespace is normal before the first write.
// With the code as well: the names are left out of the build (ESP_ERR_TO_NAME_LOOKUP)
esp_err_t check(esp_err_t err, const char* what, const char* key) {
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || kLog)
        ESP_LOGE(TAG, "%s %s: %s (0x%x)", what, key, esp_err_to_name(err), err);
    return err;
}

// Flash program and erase fail at the low rail (HW v4, 1.9 V): the write returns OK and
// reads back as missing. Anything that may write holds the rail high, after its ramp
// (2.1 ms worst case, measured; Power::set() doesn't wait)
struct HighRail {
    const bool mWasHigh = Power::current();
    Power::Lock mLock{Power::Flag::Flash};
    HighRail() {
        if (!mWasHigh)
            esp_rom_delay_us(3000);
    }
};

struct Handle {
    nvs_handle_t mHandle {0};
    bool mOpen {false};
    explicit Handle(nvs_open_mode_t mode) {
        mOpen = init() && check(nvs_open(kNamespace, mode, &mHandle), "open", kNamespace) == ESP_OK;
    }
    ~Handle() {
        if (mOpen)
            nvs_close(mHandle);
    }
};

} // namespace

bool init() {
    static bool sUp = false;
    if (!sUp) {
        HighRail rail; // Init may fix up or erase pages
        const auto err = nvs_flash_init();
        if (err != ESP_OK)
            ESP_LOGE(TAG, "nvs %s (0x%x)", esp_err_to_name(err), err);
        sUp = err == ESP_OK;
    }
    return sUp;
}

std::vector<uint8_t> read(const char* key) {
    if constexpr (kLog)
        stats("read");
    Handle h(NVS_READONLY);
    size_t size = 0;
    if (!h.mOpen || check(nvs_get_blob(h.mHandle, key, nullptr, &size), "size", key) != ESP_OK)
        return {};
    std::vector<uint8_t> data(size);
    if (check(nvs_get_blob(h.mHandle, key, data.data(), &size), "read", key) != ESP_OK)
        return {};
    return data;
}

bool write(const char* key, std::span<const uint8_t> data) {
    if constexpr (kLog)
        stats("before write");
    HighRail rail;
    Handle h(NVS_READWRITE);
    return h.mOpen && check(nvs_set_blob(h.mHandle, key, data.data(), data.size()), "write", key) == ESP_OK
        && check(nvs_commit(h.mHandle), "commit", key) == ESP_OK;
}

void erase(const char* key) {
    HighRail rail;
    Handle h(NVS_READWRITE);
    if (h.mOpen && nvs_erase_key(h.mHandle, key) == ESP_OK)
        nvs_commit(h.mHandle);
}

} // namespace Storage
