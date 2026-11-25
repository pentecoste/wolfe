#pragma once

#include "effect.h"
#include "graph_clock.h"
#include "input.h"
#include "output.h"
#include "virtual_devices.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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

// A node's mute switch. It may be thrown from any thread while audio runs;
// the cycle fades the node's audio out or in over one cycle, so that it does
// not click.
struct Mute {
    std::atomic<bool> on{false};
    float applied = 1.0f;  // the gain at the end of the last cycle (used by the cycle only)

    // One cycle of a node's audio, as the switch has it.
    void apply(float* stereo) {
        const float target = on.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
        if (target == applied) {
            if (target == 0.0f) std::fill(stereo, stereo + kQuantum * 2, 0.0f);
            return;
        }
        for (int i = 0; i < kQuantum; i++) {
            const float gain = applied + (target - applied) * (i + 1) / kQuantum;
            stereo[i * 2] *= gain;
            stereo[i * 2 + 1] *= gain;
        }
        applied = target;
    }
};

// The loudest sample that went into and came out of an effect since a front
// end last asked. Written by the cycle, read from anywhere.
struct NodePeaks {
    std::atomic<float> in{0.0f}, out{0.0f};

    static void raise(std::atomic<float>& peak, const float* stereo) {
        float loudest = 0.0f;
        for (int i = 0; i < kQuantum * 2; i++) loudest = std::max(loudest, std::fabs(stereo[i]));
        if (loudest > peak.load(std::memory_order_relaxed)) peak.store(loudest, std::memory_order_relaxed);
    }
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
        std::shared_ptr<VirtualDevice> device;  // or one of Wolfe's speakers (with `reader` on `tap`);
        const Input* tap = nullptr;
        std::shared_ptr<Effect> effect;         // otherwise this is, with `feeds`
        std::vector<Feed> feeds;
        std::vector<float> audio;               // this step's result for the current cycle
        std::shared_ptr<Mute> mute;             // silences what comes out of it
        std::shared_ptr<NodePeaks> peaks;       // effects: their levels, for the front end
    };
    struct Sink {
        std::string nodeId;
        std::shared_ptr<SourceBuffer> feed;     // where its result goes, for a device to play,
        std::shared_ptr<VirtualDevice> device;  // or for one of Wolfe's devices to take
        std::vector<Feed> feeds;
        std::vector<int> inputs;                // the reader steps that reach it, for reports
        std::shared_ptr<Mute> mute;             // silences what goes into it (unless its step does)
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
    // Windows at once and taken away with the node. What goes into a
    // microphone can also be linked onward, as it is: a microphone is the one
    // node that is both an output of the graph and a source.
    void addVirtualDevice(const std::string& id, const VirtualDeviceSpec& spec);

    // Removes a node together with its links. A device is closed, an
    // application is no longer captured.
    void removeNode(const std::string& id);

    void addLink(const std::string& from, const std::string& to, float gain = 1.0f);
    void removeLink(const std::string& from, const std::string& to);
    void setLinkGain(const std::string& from, const std::string& to, float gain);
    float linkGain(const std::string& from, const std::string& to) const;  // -1 with no such link
    // A setting of an effect, by its index in the type's parameters (see EffectType).
    void setEffectParameter(const std::string& id, int index, float value);
    // A muted node passes on silence: a source or an effect gives silence to
    // what it is linked to, a sink is given silence. Devices are not touched.
    void setMuted(const std::string& id, bool muted);

    // Extra age given to the audio on top of the minimum, to absorb timing
    // irregularities. More is safer, less is faster. Applies to nodes added later.
    void setMarginMs(double milliseconds) { marginSeconds_ = milliseconds / 1000.0; }
    // False: Wolfe's own devices are never given to Windows (for tests).
    // Call before adding the first one.
    void setOfferVirtualDevices(bool offer) { offerVirtual_ = offer; }

    void start();
    void stop();

    // One line per node and per connection in use, for the console.
    std::string report();

    // --- for a front end that shows the nodes one by one ---

    // What the node is doing, in words; empty for an unknown id.
    std::string nodeStatus(const std::string& id) const;
    // Loudest sample of a node since the last call: what an input brings, what
    // an output plays, what comes out of an effect.
    float takeNodePeak(const std::string& id);
    // What went into an effect, the same way (0 for anything else).
    float takeNodeInputPeak(const std::string& id);
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
    bool isWolfeMic(const std::string& id) const;
    void requireNewId(const std::string& id) const;
    std::shared_ptr<Link> findLink(const std::string& from, const std::string& to) const;
    std::shared_ptr<Plan> buildPlan(const Plan* previous) const;
    void rebuildPlan();
    std::shared_ptr<Plan> plan() const;

    void cycleMain();
    // `leaderOut`: where the leading output's share goes, if one leads.
    void runCycle(Plan& plan, int64_t cycle, const std::string& leaderId, float* leaderOut);

    std::map<std::string, std::unique_ptr<Input>> inputs_;
    // Each effect node's one instance, kept from plan to plan.
    std::map<std::string, std::shared_ptr<Effect>> effects_;
    std::map<std::string, std::shared_ptr<Mute>> mutes_;  // every node's, kept from plan to plan
    std::map<std::string, std::shared_ptr<NodePeaks>> peaks_;  // every effect's
    // A Wolfe microphone is a step that passes on what goes into it, and a sink
    // fed by that step through one of these.
    std::map<std::string, std::shared_ptr<Link>> throughLinks_;
    std::shared_ptr<Effect> through_ = createEffect("mix");
    std::map<std::string, OutputNode> outputs_;
    std::map<std::string, std::shared_ptr<VirtualDevice>> virtuals_;
    // Wolfe's speakers as captured from their endpoints: Windows hands over
    // what is played on them sooner this way than through the USB stream.
    std::map<std::string, std::unique_ptr<Input>> taps_;
    std::unique_ptr<VirtualDevices> virtualDevices_;  // made when the first one is added
    std::vector<std::shared_ptr<Link>> links_;

    double marginSeconds_ = 0.004;
    bool offerVirtual_ = true;
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
