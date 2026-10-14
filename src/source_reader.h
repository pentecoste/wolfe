#pragma once

#include "common.h"
#include "source_buffer.h"

#include <atomic>
#include <memory>

namespace wolfe {

// Reads one source on behalf of one output.
//
// A source and an output are driven by different hardware clocks, so "48000
// frames per second" means slightly different things to each. The reader
// resamples by a ratio very close to 1 and keeps adjusting it so that the
// audio it hands over is always the same age (the target). Without this the
// two sides would slowly drift apart until audio is dropped or runs out.
//
// The target starts at the least that can work and grows by itself when audio
// turns out to arrive later than that; it shrinks back slowly when there is
// room to spare.
class SourceReader {
public:
    SourceReader(std::shared_ptr<SourceBuffer> buffer, double marginSeconds);

    // Fills `stereo` with kQuantum frames for the graph cycle that the output
    // runs at `cycleTime` (on the output's smoothed clock; `now` is the real time).
    void read(double cycleTime, double now, float* stereo);

    struct Stats {
        double targetMs = 0;         // age the audio is kept at
        double errorMs = 0;          // current deviation from that age
        double ratioPpm = 0;         // resampling ratio, in parts per million off 1.0
        double spareMs = 0;          // least audio left over after a read since the last call
        uint64_t lateFrames = 0;     // frames replaced by silence because they arrived too late
        uint64_t jumps = 0;          // times the position had to jump instead of gliding
        bool active = false;         // the source is delivering audio
    };
    // Called from another thread; resets the "spare" minimum.
    Stats takeStats();

    const std::shared_ptr<SourceBuffer>& buffer() const { return buffer_; }

private:
    void seek(double position);
    void adapt(double now, int missing, double spare);

    std::shared_ptr<SourceBuffer> buffer_;
    double margin_;

    bool started_ = false;
    int64_t positionWhole_ = 0;    // next source frame to read...
    double positionFraction_ = 0;  // ...plus this fraction of a frame
    double integral_ = 0;          // slow part of the ratio correction

    double extra_ = 0;             // age added because audio arrived late
    int pendingMissing_ = 0;       // frames that were not there yet; late or just the end?
    uint32_t pendingGaps_ = 0;
    double pendingPushTime_ = 0;
    double relaxStart_ = 0;        // start of the current "room to spare" observation
    double relaxLowest_ = 1e9;

    std::atomic<double> statTarget_{0}, statError_{0}, statRatio_{1}, statSpare_{1e9};
    std::atomic<uint64_t> statLate_{0}, statJumps_{0};
    std::atomic<bool> statActive_{false};
};

}  // namespace wolfe
