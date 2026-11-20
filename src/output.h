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

// Plays what the graph made for it on a playback device, on its own thread.
//
// The graph runs on its own clock and leaves its result in `feed`; the device
// runs on another. A reader takes the audio across, resampling it to the
// device's pace. A small queue of audio is kept in the device so that it never
// runs dry between two refills; how much is needed depends on how regularly
// the device asks, and is found out while running.
class Output {
public:
    Output(std::string device, std::shared_ptr<SourceBuffer> feed, double marginSeconds);
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
    void runCycle(double cycleTime, float* stereo);
    void setStatus(const std::string& text);

    std::string device_;
    std::shared_ptr<SourceReader> reader_;

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
