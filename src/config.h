#pragma once

#include "graph.h"

#include <string>
#include <vector>

namespace wolfe {

// A graph as it is written in a file: plain data, nothing running.
//
// Node types: "input", "loopback" and "output" name a device, "app" names an
// application, anything else is an effect type (see effect.h).
struct NodeDescription {
    std::string id;
    std::string type;
    std::string target;  // "device" or "process" in the file; empty for effects
    std::string label;   // optional name to show instead of the id
    bool hasPosition = false;  // where the graphical editor drew it
    float x = 0, y = 0;
    bool parked = false;  // graphical editor: an application kept off every playback device
};

struct LinkDescription {
    std::string from, to;
    double gainDb = 0.0;
};

struct GraphDescription {
    double marginMs = 4.0;
    bool closeToTray = true;  // graphical editor: closing the window keeps it running
    // Graphical editor: the playback device nobody listens to. Empty: pick one
    // that looks right; "none": there is none; else a device id or part of a name.
    std::string silentDevice;
    std::vector<NodeDescription> nodes;
    std::vector<LinkDescription> links;
};

bool isDeviceType(const std::string& type);  // input, loopback, output
bool isAppType(const std::string& type);

// Comments are allowed in the file. Throws std::runtime_error with a message
// meant for the user.
GraphDescription loadDescription(const std::string& path);
void saveDescription(const std::string& path, const GraphDescription& description);

// Creates in `graph` the links and the nodes they use. A node that nothing is
// connected to is left out: it would open a device for no reason.
void buildGraph(const GraphDescription& description, Graph& graph);

// The one node-creating step, shared with the graphical editor.
void addNode(const NodeDescription& node, Graph& graph);

}  // namespace wolfe
