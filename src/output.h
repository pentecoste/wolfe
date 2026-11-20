#pragma once

#include "com.h"
#include "source_buffer.h"
#include "source_reader.h"
#include "stream_clock.h"

#include <audioclient.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wolfe {

class Output;

// What lets a playback device set the graph's pace (the graph is one).
class Pacer {
public:
    virtual ~Pacer() = default;
    // Called by a device for every quantum it is about to queue. If the
    // device leads the graph, the graph's next cycle is computed right away
    // and this output's share of it written to `stereo`; false otherwise.
    // `deviceOffset` is when the device played (or will play) its frame 0,
    // `accounted` how many frames it has been given or made up so far, and
    // `targetFill` how many it keeps queued.
    virtual bool lead(const Output* output, double deviceOffset, int targetFill, int64_t accounted, float* stereo) = 0;
};

// Plays what the graph made for it on a playback device, on its own thread.
//
// One playback device leads the graph: it has each cycle computed when it
// needs audio, and plays the result as it is. Any other runs on a clock of its
// own; the graph leaves its result in `feed` and a reader takes it across,
// resampling it to the device's pace. A small queue of audio is kept in the
// device so that it never runs dry between two refills; how much is needed
// depends on how regularly the device asks, and is found out while running.
class Output {
public:
    Output(std::string device, std::shared_ptr<SourceBuffer> feed, double marginSeconds, Pacer* pacer = nullptr);
    ~Output();

    void start();
    void stop();

    std::string status() const;
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

    struct Stats {
        uint64_t underruns = 0;    // times the device ran out of audio
        double queuedMs = 0;       // audio kept queued in the device
        double lowestMs = 0;       // least audio found queued since the last call
        double longestWaitMs = 0;  // longest pause between two requests from the device
    };
    Stats takeStats();
    // How the graph's audio reaches the device.
    SourceReader::Stats takeFeedStats() { return reader_->takeStats(); }

private:
    void threadMain();
    void open();
    void run();
    void close();
    void setStatus(const std::string& text);

    std::string device_;
    std::shared_ptr<SourceReader> reader_;
    Pacer* pacer_;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE dataEvent_ = nullptr;

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    int bufferFrames_ = 0;
    int targetFill_ = 0;     // the queue is topped up whenever it is below this

    StreamClock clock_;      // device time vs. frames the device has played
    int64_t submitted_ = 0;  // frames handed to the device
    int64_t madeUp_ = 0;     // frames of silence the device played on its own when it ran dry

    std::atomic<uint64_t> underruns_{0};
    std::atomic<int> statTargetFill_{0};
    std::atomic<int> statLowest_{1 << 30};
    std::atomic<double> statLongestWait_{0.0};
    std::atomic<float> peak_{0.0f};

    mutable std::mutex statusMutex_;
    std::string status_ = "stopped";
};

}  // namespace wolfe
