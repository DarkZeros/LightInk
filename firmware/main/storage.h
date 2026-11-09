#pragma once

#include <cstdint>
#include <span>
#include <vector>

/* Small blobs in flash (NVS namespace "lightink"), kept across resets and `idf.py flash`.
 * Flash is slow and costs power: only cold boot reads, and writers only write what
 * changed (the settings at most once an hour, the pairing when it changes).
 */
namespace Storage {

// Brings NVS up, also needed by the radio's PHY calibration data. Cheap once up
bool init();

// Empty when missing
std::vector<uint8_t> read(const char* key);
bool write(const char* key, std::span<const uint8_t> data);
void erase(const char* key);

} // namespace Storage
