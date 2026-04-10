#include "watchface.h"

// TODO: Put here the code to render and advance the counters and diff to RTC memory


/*
       // Build the current state
        WatchfaceState currentState = makeState(*this);

        // Determine if this is a first-ever draw (no valid previous buffer)
        // We use mDeltaCount == 0 as a proxy for "never drawn before".
        // On first draw, prev == nullptr → full redraw.
        const bool firstDraw = (kDSState.mDeltaCount == 0 && kDSState.mDeltaIndex == 0);

        // Draw the current frame into the display buffer
        mDisplay.fillScreen(0);
        if (firstDraw) {
            wf->draw(mDisplay, nullptr, currentState);
        } else {
            // We have a previous buffer in kDSState.mDisplayBuffer.
            // Reconstruct a "prev" state — we don't store the full prev WatchfaceState,
            // but we can pass nullptr here and let the watchface do a full redraw
            // into the existing buffer.  The diff will still be minimal because
            // the buffer already has the previous frame content.
            // For a proper incremental draw, we'd need to store the prev state too.
            // For now, do a full redraw — the delta will capture only what changed.
            wf->draw(mDisplay, nullptr, currentState);
        }

        // Write the current frame to the display and refresh
        mDisplay.writeAllAndRefresh();
        mDisplay.writeAll(); // Write to back buffer too for partial updates

        // ----------------------------------------------------------------
        // Pre-compute delta frames for the next `stepSize` minutes
        // ----------------------------------------------------------------

        // Calculate stepSize (same logic as below, duplicated here for clarity)
        auto stepSize = [&] {
            if (kSettings.mPower.mNight && mNow.Hour < 7)
                return 5;
            if (!kSettings.mPower.mAuto)
                return 1;
            if (mBattery.mCurPercent < 100) {
                return 5;
            } else if (mBattery.mCurPercent < 200) {
                return 4;
            } else if (mBattery.mCurPercent < 500) {
                return 2;
            }
            return 1;
        }();

        const uint8_t framesToCompute = std::min(stepSize, (int)kMaxDeltaFrames);

        // Reset delta state
        kDSState.mDeltaIndex = 0;
        kDSState.mDeltaCount = 0;

        // Invalidate all frames
        for (auto& f : kDSState.mDeltas)
            f.invalidate();

        // Simulate future minutes and compute deltas
        WatchfaceState prevState = currentState;
        SimpleByteCompressor compressor;

        for (uint8_t i = 0; i < framesToCompute; ++i) {
            // Advance simulated time by one minute
            WatchfaceState nextState = prevState;
            advanceOneMinute(nextState.mTime);

            // Snapshot the current buffer as "previous"
            mDisplay.enableDiff();

            // Draw the next frame (incremental — only changed parts)
            wf->draw(mDisplay, &prevState, nextState);

            // Compute and store the delta
            bool ok = mDisplay.getDelta(compressor, kDSState.mDeltas[i]);
            if (!ok) {
                // Delta too large — stop pre-computing, wake stub will fall back to CPU
                ESP_LOGE("delta", "frame %d overflow, stopping pre-compute", i);
                break;
            }

            ESP_LOGI("delta", "frame %d: %d bytes (%d entries)",
                     i, kDSState.mDeltas[i].mPayloadSize,
                     kDSState.mDeltas[i].mPayloadSize / 3);

            kDSState.mDeltaCount = i + 1;
            prevState = nextState;
        }
*/