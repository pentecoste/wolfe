#pragma once

#include "common.h"

#include <atomic>

namespace wolfe {

// Where the graph is at any moment, for whoever has to keep up with it: by
// time t the graph has computed no frame at or after frameAt(t), and every
// frame before frameAt(t) - slackFrames().
//
// The graph keeps the pace of the playback device that leads it (so that
// device needs no resampling) or, when there is none, of the performance
// counter. Wolfe's own devices follow this clock, as a sound card follows the
// clock it is given, so the graph exchanges frames with them by position.
class GraphClock {
public:
    double frameAt(double time) const { return (time - offset_.load(std::memory_order_relaxed)) * kSampleRate; }
    double timeOf(double frame) const { return offset_.load(std::memory_order_relaxed) + frame / kSampleRate; }

    // The time of frame 0. Set by whoever leads the graph; it moves slowly as
    // that clock drifts against the performance counter.
    double offset() const { return offset_.load(std::memory_order_relaxed); }
    void setOffset(double offset) { offset_.store(offset, std::memory_order_relaxed); }

    // How far frameAt() can be ahead of the frames actually computed: what
    // reads the graph's results has to stay at least that far behind.
    double slackFrames() const { return slack_.load(std::memory_order_relaxed); }
    void setSlackFrames(double frames) { slack_.store(frames, std::memory_order_relaxed); }

private:
    std::atomic<double> offset_{0.0};
    std::atomic<double> slack_{kQuantum};
};

}  // namespace wolfe
