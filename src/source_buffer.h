#pragma once

#include "common.h"
#include "stream_clock.h"

#include <atomic>
#include <vector>

namespace wolfe {

// The audio captured from one source, kept as a timeline of stereo frames
// numbered from zero. One thread writes (the capture thread); any number of
// output threads read, each at its own position, without consuming anything.
//
// The timeline never has holes: when the source stops producing audio (an app
// that goes quiet, a device unplugged), the missing stretch is filled with
// silence as soon as audio comes back, so frame numbers keep following time.
class SourceBuffer {
public:
    // A pause longer than this means the source stopped, not that a block is late.
    static constexpr double kGapSeconds = 0.05;

    explicit SourceBuffer(int capacityFrames = 1 << 16);

    // --- writer side ---

    // Appends a block that became available at `arrivalTime`.
    void push(double arrivalTime, const float* stereo, int frames);
    // How many frames the source normally delivers at once.
    void setPacketFrames(int frames) { packetFrames_.store(frames, std::memory_order_relaxed); }

    // --- reader side ---

    bool clockValid() const { return clockValid_.load(std::memory_order_acquire); }
    // Time at which frame 0 happened; frame n happened at clockOffset() + n / 48000.
    double clockOffset() const { return clockOffset_.load(std::memory_order_relaxed); }
    int64_t framesWritten() const { return written_.load(std::memory_order_acquire); }
    double lastPushTime() const { return lastPushTime_.load(std::memory_order_relaxed); }
    int packetFrames() const { return packetFrames_.load(std::memory_order_relaxed); }
    // How many times a pause in the source was filled with silence.
    uint32_t gapCount() const { return gaps_.load(std::memory_order_relaxed); }

    // Copies frames [first, first + count) to `stereo`. Frames that are not in
    // the buffer read as silence. Returns how many of them were not written yet.
    int read(int64_t first, int count, float* stereo) const;

    // Highest sample magnitude since the last call (for level meters).
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

private:
    void append(const float* stereo, int64_t frames);  // nullptr appends silence

    std::vector<float> ring_;
    int64_t capacity_;

    std::atomic<int64_t> written_{0};
    std::atomic<double> clockOffset_{0.0};
    std::atomic<bool> clockValid_{false};
    std::atomic<double> lastPushTime_{-1e9};
    std::atomic<int> packetFrames_{kQuantum};
    std::atomic<uint32_t> gaps_{0};
    std::atomic<float> peak_{0.0f};

    StreamClock clock_;  // writer thread only
};

}  // namespace wolfe
