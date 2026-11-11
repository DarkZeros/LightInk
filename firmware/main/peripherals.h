#pragma once

#include <atomic>
#include <functional>
#include <span>
#include <vector>
#include <inttypes.h>

// Asks a long running job to end early. The job reads it only when it is awake anyway,
// and each read also runs the owner's check (e.g. a touch poll), so nothing has to wake
// the chip just to look for a cancel. Once true it stays true
class Cancel {
public:
    Cancel() = default;
    explicit Cancel(std::function<bool()> check) : mCheck(std::move(check)) {}

    explicit operator bool() const {
        if (!mCancelled && mCheck && mCheck())
            mCancelled = true;
        return mCancelled;
    }
    void set() { mCancelled = true; }

private:
    mutable std::atomic<bool> mCancelled{false};
    std::function<bool()> mCheck;
};

/* Helper class to handle extra peripherals that need active CPU control, buzz, speaker
 */
struct Peripherals {
    static void vibrator(const std::vector<int>&);
    static void speaker(const std::vector<std::pair<int, int>>& pattern);
    static void beep();

    static void tetris(const Cancel& cancel);
};