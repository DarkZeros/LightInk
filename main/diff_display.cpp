#include <span>
#include <optional>
#include <cstdint>
#include <cstring>

#include "diff_display.h"
#include "trace.h"

void DiffDisplay::snapshot() {
    // Snapshot the current buffer as the "previous" frame
    memcpy(mPrev, buffer, sizeof(buffer));
}

std::optional<int> DiffDisplay::getDelta(std::span<const uint8_t> current,
                           std::span<uint8_t> outBuffer) {
    // Format:
    // uint16_t len;
    // struct {
    //   uint8_t match;
    //   uint8_t miss;
    //   uint8_t data[miss];
    // }[len];
    TRACE("getDelta");
    if (outBuffer.size() < 2)
        return std::nullopt;

    uint8_t* start = outBuffer.data();
    outBuffer = outBuffer.subspan(2); // Leave 2 bytes for len

    uint16_t len = 0;

    size_t i = 0;
    while (i < current.size()) {
        // Count match (max 255)
        uint8_t match = 0;
        while (i < current.size() &&
               current[i] == mPrev[i] &&
               match < 255) {
            ++i;
            ++match;
        }

        // Count miss (max 255)
        size_t missStart = i;
        uint8_t miss = 0;
        while (i < current.size() &&
               current[i] != mPrev[i] &&
               miss < 255) {
            ++i;
            ++miss;
        }

        // If nothing to emit (can happen at end), stop
        if (match == 0 && miss == 0)
            break;

        if (outBuffer.size() < 2 + miss)
            return std::nullopt;

        // Write match + miss
        outBuffer[0] = match;
        outBuffer[1] = miss;
        outBuffer = outBuffer.subspan(2);

        // Write miss data
        if (miss > 0) {
            std::memcpy(outBuffer.data(),
                        current.data() + missStart,
                        miss);
            outBuffer = outBuffer.subspan(miss);
        }

        ++len;
    }

    // Write len (little endian)
    start[0] = static_cast<uint8_t>(len & 0xFF);
    start[1] = static_cast<uint8_t>((len >> 8) & 0xFF);

    ESP_LOGE("diff", "len %d, size %d", len, outBuffer.data() - start);

    return static_cast<int>(outBuffer.data() - start);
}