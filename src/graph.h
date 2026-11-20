#pragma once

#include "effect.h"
#include "graph_clock.h"
#include "input.h"
#include "output.h"
#include "virtual_devices.h"

#include <atomic>
#include <map>
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

// What the graph computes every cycle, flattened into steps ordered so that
// each step only depends on earlier ones. Every node that leads to an output
// is one step, computed once per cycle whatever the number of outputs it feeds.
struct Plan {
    struct Feed {
        int step;                    // index of the step this audio comes from
        std::shared_ptr<Link> link;
        float appliedGain;           // gain used at the end of the previous cycle
    };
    struct Step {
        std::string nodeId;
        std::shared_ptr<SourceReader> reader;   // an input from a device on its own clock,
        std::shared_ptr<VirtualDevice> device;  // or one of Wolfe's devices, on the graph's clock;
        std::shared_ptr<Effect> effect;         // otherwise this is, with `feeds`
        std::vector<Feed> feeds;
        std::vector<float> audio;               // this step's result for the current cycle
    };
    struct Sink {
        std::string nodeId;
        std::shared_ptr<SourceBuffer> feed;     // where its result goes, for a device to play,
        std::shared_ptr<VirtualDevice> device;  // or for one of Wolfe's devices to take
        std::vector<Feed> feeds;
        std::vector<int> inputs;                // the reader steps that reach it, for reports
    };
    std::vector<Step> steps;
    std::vector<Sink> sinks;
};

// The whole patchbay: inputs, effects and outputs, and the links between them.
//
// Nodes are named by an id chosen by whoever builds the graph. The add*
// functions throw std::runtime_error on a mistake (duplicate id, unknown node,
// a link that makes a loop...). Links may be added and removed while audio is
// running.
//
// Every cycle computes the next 10 ms of every node. One playback device leads
// the graph: it has each cycle computed when it needs audio and plays the
// result as it is. While none does (none is linked, or the one that led stopped
// asking), a timer keeps the pace instead. Wolfe's own devices follow the
// graph's clock (see GraphClock); every other device runs on its own, and a
// reader resamples between the two at the edge.
class Graph : public Pacer {
public:
    Graph();
    ~Graph();

    void addInput(const std::string& id, InputSpec spec);
    void addEffect(const std::string& id, const std::string& type);
    void addOutput(const std::string& id, const std::string& device);
    // One of Wolfe's own devices, which Windows shows like any other: a
    // speaker is an input of the graph, a microphone an output. It is added to
    // Windows at once and taken away with the node.
    void addVirtualDevice(const std::string& id, const VirtualDeviceSpec& spec);

    // Removes a node together with its links. A device is closed, an
    // application is no longer captured.
    void removeNode(const std::string& id);

    void addLink(const std::string& from, const std::string& to, float gain = 1.0f);
    void removeLink(const std::string& from, const std::string& to);
    void setLinkGain(const std::string& from, const std::string& to, float gain);

    // Extra age given to the audio on top of the minimum, to absorb timing
    // irregularities. More is safer, less is faster. Applies to nodes added later.
    void setMarginMs(double milliseconds) { marginSeconds_ = milliseconds / 1000.0; }

    void start();
    void stop();

    // One line per node and per connection in use, for the console.
    std::string report();

    // --- for a front end that shows the nodes one by one ---

    // What the node is doing, in words; empty for an unknown id.
    std::string nodeStatus(const std::string& id) const;
    // Loudest sample of an input or output since the last call (0 for anything else).
    float takeNodePeak(const std::string& id);
    // Queue and underruns of an output, and how each source reaches it.
    struct OutputReport {
        Output::Stats output;
        SourceReader::Stats feed;  // from the graph to the device
        struct Source {
            std::string id;
            SourceReader::Stats stats;  // from the source to the graph
        };
        std::vector<Source> sources;
    };
    OutputReport outputReport(const std::string& id);

    // Cycles the graph could not run in time and skipped, since the start.
    uint64_t skippedCycles() const { return skipped_.load(std::memory_order_relaxed); }
    // The id of the output that leads the graph; empty while the timer does.
    std::string leader() const;

    // Pacer
    bool lead(const Output* output, double deviceOffset, int targetFill, int64_t accounted, float* stereo) override;

private:
    enum class Kind { None, Input, Effect, Output };
    struct OutputNode {
        std::shared_ptr<SourceBuffer> feed;
        std::unique_ptr<Output> output;
    };

    Kind kindOf(const std::string& id) const;
    void requireNewId(const std::string& id) const;
    std::shared_ptr<Link> findLink(const std::string& from, const std::string& to) const;
    std::shared_ptr<Plan> buildPlan(const Plan* previous) const;
    void rebuildPlan();
    std::shared_ptr<Plan> plan() const;

    void cycleMain();
    // `leaderOut`: where the leading output's share goes, if one leads.
    void runCycle(Plan& plan, int64_t cycle, const std::string& leaderId, float* leaderOut);

    std::map<std::string, std::unique_ptr<Input>> inputs_;
    std::map<std::string, std::string> effects_;  // id -> type
    std::map<std::string, OutputNode> outputs_;
    std::map<std::string, std::shared_ptr<VirtualDevice>> virtuals_;
    std::unique_ptr<VirtualDevices> virtualDevices_;  // made when the first one is added
    std::vector<std::shared_ptr<Link>> links_;

    double marginSeconds_ = 0.004;
    bool running_ = false;

    mutable std::mutex planMutex_;  // held by the cycle while it runs; also guards what follows
    std::shared_ptr<Plan> plan_;
    int64_t nextCycle_ = 0;            // the cycle to compute next
    const Output* leader_ = nullptr;   // the output leading the graph, if any
    std::string leaderId_;
    std::map<const Output*, std::string> outputIds_;  // for lead(), from the outputs' threads
    double lastLead_ = 0;              // when it last had a cycle computed
    std::shared_ptr<GraphClock> clock_ = std::make_shared<GraphClock>();

    std::thread cycleThread_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<uint64_t> skipped_{0};
};

}  // namespace wolfe
