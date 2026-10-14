#include "source_buffer.h"

#include <cmath>
#include <cstring>

namespace wolfe {

SourceBuffer::SourceBuffer(int capacityFrames) : capacity_(capacityFrames) {
    ring_.assign((size_t)capacity_ * kChannels, 0.0f);
}

void SourceBuffer::push(double arrivalTime, const float* stereo, int frames) {
    if (frames <= 0) return;
    int64_t written = written_.load(std::memory_order_relaxed);

    if (clock_.valid()) {
        // Where the timeline should have reached by now, had the source kept going.
        double expected = clock_.frameAt(arrivalTime);
        double gap = expected - (double)(written + frames);
        if (gap > kGapSeconds * kSampleRate) {
            // The block then ends exactly where the clock says, so the clock is left alone.
            append(nullptr, (int64_t)std::llround(gap));
            gaps_.fetch_add(1, std::memory_order_relaxed);
        } else {
            clock_.update(arrivalTime, written + frames);
        }
    } else {
        clock_.update(arrivalTime, written + frames);
    }

    float peak = 0.0f;
    for (int i = 0; i < frames * kChannels; i++) peak = std::max(peak, std::fabs(stereo[i]));
    if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);

    append(stereo, frames);
    clockOffset_.store(clock_.offset(), std::memory_order_relaxed);
    lastPushTime_.store(arrivalTime, std::memory_order_relaxed);
    clockValid_.store(true, std::memory_order_release);
}

void SourceBuffer::append(const float* stereo, int64_t frames) {
    int64_t written = written_.load(std::memory_order_relaxed);
    // Silence longer than the buffer only needs the last `capacity_` frames cleared.
    int64_t skip = frames > capacity_ ? frames - capacity_ : 0;
    int64_t position = written + skip;
    int64_t remaining = frames - skip;
    const float* source = stereo ? stereo + skip * kChannels : nullptr;

    while (remaining > 0) {
        int64_t slot = position % capacity_;
        int64_t chunk = std::min(remaining, capacity_ - slot);
        float* destination = ring_.data() + slot * kChannels;
        if (source) {
            std::memcpy(destination, source, (size_t)chunk * kChannels * sizeof(float));
            source += chunk * kChannels;
        } else {
            std::memset(destination, 0, (size_t)chunk * kChannels * sizeof(float));
        }
        position += chunk;
        remaining -= chunk;
    }
    written_.store(written + frames, std::memory_order_release);
}

int SourceBuffer::read(int64_t first, int count, float* stereo) const {
    int64_t written = written_.load(std::memory_order_acquire);
    int64_t oldest = std::max<int64_t>(written - capacity_, 0);
    int64_t end = first + count;

    std::memset(stereo, 0, (size_t)count * kChannels * sizeof(float));

    int64_t from = std::max(first, oldest);
    int64_t to = std::min(end, written);
    while (from < to) {
        int64_t slot = from % capacity_;
        int64_t chunk = std::min(to - from, capacity_ - slot);
        std::memcpy(stereo + (from - first) * kChannels, ring_.data() + slot * kChannels,
                    (size_t)chunk * kChannels * sizeof(float));
        from += chunk;
    }

    int64_t notYetWritten = end - std::max(first, written);
    return notYetWritten > 0 ? (int)notYetWritten : 0;
}

}  // namespace wolfe
