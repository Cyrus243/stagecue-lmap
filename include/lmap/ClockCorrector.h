// Clock recovery strategy (spec §7.5). The receiver's and sender's audio clocks
// are different crystals; left uncorrected every receiver eventually under/overruns.
//
// This milestone ships the simple drop/insert corrector (§7.5b): audible on
// sustained tones but enough to get the system running. It lives behind this
// interface precisely so the recommended adaptive resampler + PI loop (§7.5a) can
// replace it later without touching the receiver.
#pragma once

#include <cmath>

namespace lmap
{
    class ClockCorrector
    {
    public:
        virtual ~ClockCorrector() = default;

        // Called once per output callback with the current buffer error in frames:
        //   error = (write_high_water - read_pos) - target_delay_frames
        // Returns the whole-frame adjustment to add to read_pos advancement this
        // callback: +1 drops one input frame (buffer too full), -1 inserts/repeats
        // one input frame (buffer too empty), 0 leaves the rate unchanged.
        virtual int correct(double errorFrames) noexcept = 0;

        virtual void reset() noexcept = 0;
    };

    // Drop/insert corrector (spec §7.5b).
    class DropInsertCorrector final : public ClockCorrector
    {
    public:
        explicit DropInsertCorrector(double alpha = 0.005, double thresholdFrames = 32.0) noexcept
            : alpha_(alpha), threshold_(thresholdFrames) {}

        int correct(double errorFrames) noexcept override
        {
            errorSmooth_ += alpha_ * (errorFrames - errorSmooth_); // quantised by packet arrival
            if (errorSmooth_ > threshold_)
            {
                errorSmooth_ -= 1.0; // account for the frame we are about to drop
                return +1;
            }
            if (errorSmooth_ < -threshold_)
            {
                errorSmooth_ += 1.0; // account for the frame we are about to insert
                return -1;
            }
            return 0;
        }

        void reset() noexcept override { errorSmooth_ = 0.0; }

        [[nodiscard]] double smoothedError() const noexcept { return errorSmooth_; }

    private:
        double alpha_;
        double threshold_;
        double errorSmooth_ = 0.0;
    };
}
