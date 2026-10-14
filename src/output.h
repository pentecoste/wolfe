#pragma once

#include "com.h"
#include "effect.h"
#include "source_reader.h"
#include "stream_clock.h"

#include <audioclient.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wolfe {

// A connection between two nodes. The gain may be changed while audio runs.
struct Link {
    std::string from, to;
    std::atomic<float> gain{1.0f};
};

// What one output has to compute every cycle: its part of the graph, flattened
// into steps ordered so that each step only depends on earlier ones.
struct Plan {
    struct Feed {
        int step;                    // index of the step this audio comes from
        std::shared_ptr<Link> link;
        float appliedGain;           // gain used at the end of the previous cycle
    };
    struct Step {
        std::string nodeId;
        std::shared_ptr<SourceReader> reader;  // set when the node is an input...
        std::shared_ptr<Effect> effect;        // ...otherwise this is, with `feeds`
        std::vector<Feed> feeds;
        std::vector<float> audio;              // this step's result for the current cycle
    };
    std::vector<Step> steps;
    std::vector<Feed> outputFeeds;  // what is summed into the device
};

// Plays the result of the graph on a playback device, on its own thread.
//
// The device sets the pace: every time it has taken audio, one graph cycle is
// run for each quantum it took. A small queue of audio is kept in the device
// so that it never runs dry between two refills; how much is needed depends on
// how regularly the device asks, and is found out while running.
class Output {
public:
    explicit Output(std::string device);
    ~Output();

    void start();
    void stop();

    void setPlan(std::shared_ptr<Plan> plan);
    std::shared_ptr<Plan> plan() const;

    std::string status() const;
    float takePeak() { return peak_.exchange(0.0f, std::memory_order_relaxed); }

    struct Stats {
        uint64_t underruns = 0;    // times the device ran out of audio
        double queuedMs = 0;       // audio kept queued in the device
        double lowestMs = 0;       // least audio found queued since the last call
        double longestWaitMs = 0;  // longest pause between two requests from the device
    };
    Stats takeStats();

private:
    void threadMain();
    void open();
    void run();
    void close();
    void runCycle(double cycleTime, float* stereo);
    void setStatus(const std::string& text);

    std::string device_;

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

    mutable std::mutex planMutex_;
    std::shared_ptr<Plan> plan_;

    std::atomic<uint64_t> underruns_{0};
    std::atomic<int> statTargetFill_{0};
    std::atomic<int> statLowest_{1 << 30};
    std::atomic<double> statLongestWait_{0.0};
    std::atomic<float> peak_{0.0f};

    mutable std::mutex statusMutex_;
    std::string status_ = "stopped";
};

}  // namespace wolfe
