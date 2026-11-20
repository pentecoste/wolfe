#pragma once

#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace wolfe {

// Audio on the graph's own clock: frame f is the one that happens at f / 48000
// seconds of nowSeconds(). One side writes frames where they belong, the other
// reads them by position; nothing is resampled, since both count the same
// time. A frame that was not written for its position reads as silence.
//
// It joins the graph to Wolfe's own devices, which keep time on the same
// clock.
class FrameTimeline {
public:
    explicit FrameTimeline(int64_t capacityFrames = 1 << 16)
        : capacity_(capacityFrames), samples_((size_t)capacityFrames * kChannels, 0.0f), held_((size_t)capacityFrames, -1) {}

    void write(int64_t frame, const float* stereo, int frames) {
        float peak = 0.0f;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < frames; i++, frame++) {
            const size_t at = slot(frame);
            samples_[at * 2] = stereo[i * 2];
            samples_[at * 2 + 1] = stereo[i * 2 + 1];
            held_[at] = frame;
            peak = std::max(peak, std::max(std::fabs(stereo[i * 2]), std::fabs(stereo[i * 2 + 1])));
        }
        if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);
    }

    // Copies frames [first, first + frames) to `stereo`; returns how many were missing.
    int read(int64_t first, int frames, float* stereo) const {
        int missing = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < frames; i++) {
            const int64_t frame = first + i;
            const size_t at = slot(frame);
            if (held_[at] == frame) {
                stereo[i * 2] = samples_[at * 2];
                stereo[i * 2 + 1] = samples_[at * 2 + 1];
            } else {
                stereo[i * 2] = stereo[i * 2 + 1] = 0.0f;
                missing++;
            }
        }
        return missing;
    }

    // Highest sample magnitude written since the last call (for level meters).
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

private:
    size_t slot(int64_t frame) const { return (size_t)(((frame % capacity_) + capacity_) % capacity_); }

    const int64_t capacity_;
    mutable std::mutex mutex_;
    std::vector<float> samples_;
    std::vector<int64_t> held_;
    std::atomic<float> peak_{0.0f};
};

}  // namespace wolfe
