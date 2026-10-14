#pragma once

#include "input.h"
#include "output.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wolfe {

// The whole patchbay: inputs, effects and outputs, and the links between them.
//
// Nodes are named by an id chosen by whoever builds the graph. The add*
// functions throw std::runtime_error on a mistake (duplicate id, unknown node,
// a link that makes a loop...). Links may be added and removed while audio is
// running.
class Graph {
public:
    Graph();
    ~Graph();

    void addInput(const std::string& id, InputSpec spec);
    void addEffect(const std::string& id, const std::string& type);
    void addOutput(const std::string& id, const std::string& device);

    // Removes a node together with its links. A device is closed, an
    // application is no longer captured.
    void removeNode(const std::string& id);

    void addLink(const std::string& from, const std::string& to, float gain = 1.0f);
    void removeLink(const std::string& from, const std::string& to);
    void setLinkGain(const std::string& from, const std::string& to, float gain);

    // Extra age given to the audio on top of the minimum, to absorb timing
    // irregularities. More is safer, less is faster. Applies to links made later.
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
        struct Source {
            std::string id;
            SourceReader::Stats stats;
        };
        std::vector<Source> sources;
    };
    OutputReport outputReport(const std::string& id);

private:
    enum class Kind { None, Input, Effect, Output };
    Kind kindOf(const std::string& id) const;
    void requireNewId(const std::string& id) const;
    std::shared_ptr<Link> findLink(const std::string& from, const std::string& to) const;
    std::shared_ptr<Plan> buildPlan(const std::string& outputId, const Plan* previous) const;
    void rebuildPlans();

    std::map<std::string, std::unique_ptr<Input>> inputs_;
    std::map<std::string, std::string> effects_;  // id -> type
    std::map<std::string, std::unique_ptr<Output>> outputs_;
    std::vector<std::shared_ptr<Link>> links_;

    double marginSeconds_ = 0.004;
    bool running_ = false;
};

}  // namespace wolfe
