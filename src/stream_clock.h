#pragma once

#include "common.h"

#include <algorithm>

namespace wolfe {

// Smooth mapping between a stream's frame counter and wall-clock time.
//
// Audio arrives and leaves in blocks at slightly irregular moments. Feeding
// each "at time T the stream had moved N frames in total" observation here
// gives a steady estimate of when any frame index happens, which is what the
// drift compensation compares between a source and an output.
class StreamClock {
public:
    void reset() { samples_ = 0; }
    bool valid() const { return samples_ > 0; }

    void update(double time, int64_t totalFrames) {
        // If the stream ran at exactly 48 kHz this value would never change.
        double observed = time - (double)totalFrames / kSampleRate;
        if (samples_ == 0) {
            offset_ = observed;
        } else {
            // A plain average at first for a fast start, then a slow follower:
            // the irregularities are far larger than the drift being tracked,
            // and any wobble left in here ends up as pitch wobble.
            double weight = std::max(1.0 / (samples_ + 1), (time - lastTime_) / kSmoothingSeconds);
            offset_ += std::min(weight, 1.0) * (observed - offset_);
        }
        lastTime_ = time;
        if (samples_ < 1000000) samples_++;
    }

    double offset() const { return offset_; }
    double timeOf(double frame) const { return offset_ + frame / kSampleRate; }
    double frameAt(double time) const { return (time - offset_) * kSampleRate; }

private:
    static constexpr double kSmoothingSeconds = 5.0;

    double offset_ = 0.0;
    double lastTime_ = 0.0;
    int samples_ = 0;
};

}  // namespace wolfe
