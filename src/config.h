#pragma once

#include "graph.h"

#include <string>
#include <vector>

namespace wolfe {

// A graph as it is written in a file: plain data, nothing running.
//
// Node types: "input", "loopback" and "output" name a device, "app" names an
// application, "virtual_speaker" and "virtual_mic" are Wolfe's own devices,
// anything else is an effect type (see effect.h).
struct NodeDescription {
    std::string id;
    std::string type;
    std::string target;  // "device", "process" or (Wolfe's devices) "name" in the file; empty for effects
    std::string serial;  // Wolfe's devices: their identity for Windows; made from the id when empty
    std::string label;   // optional name to show instead of the id
    double gainDb = 0.0; // "gain" nodes
    bool hasPosition = false;  // where the graphical editor drew it
    float x = 0, y = 0;
};

struct LinkDescription {
    std::string from, to;
    double gainDb = 0.0;
};

struct GraphDescription {
    double marginMs = 4.0;
    bool closeToTray = true;  // graphical editor: closing the window keeps it running
    std::vector<NodeDescription> nodes;
    std::vector<LinkDescription> links;
};

bool isDeviceType(const std::string& type);  // input, loopback, output
bool isAppType(const std::string& type);
bool isVirtualType(const std::string& type);  // virtual_speaker, virtual_mic

// Comments are allowed in the file. Throws std::runtime_error with a message
// meant for the user.
GraphDescription loadDescription(const std::string& path);
void saveDescription(const std::string& path, const GraphDescription& description);

// Creates in `graph` the links and the nodes they use. A node that nothing is
// connected to is left out: it would open a device for no reason. Wolfe's own
// devices are made whether linked or not, so that applications can be given
// them in Windows' settings.
void buildGraph(const GraphDescription& description, Graph& graph);

// The one node-creating step, shared with the graphical editor.
void addNode(const NodeDescription& node, Graph& graph);

}  // namespace wolfe
