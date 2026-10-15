#pragma once

#include <atomic>
#include <span>
#include <vector>
#include <inttypes.h>

// Set by the owner to ask a long running job to end early
using Cancel = std::atomic<bool>;

/* Helper class to handle extra peripherals that need active CPU control, buzz, speaker
 */
struct Peripherals {
    static void vibrator(const std::vector<int>&);
    static void speaker(const std::vector<std::pair<int, int>>& pattern);

    static void tetris(const Cancel& cancel);
};