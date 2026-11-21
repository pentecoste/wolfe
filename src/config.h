#pragma once

#include "graph.h"

#include <map>
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
    // Effects: their settings, by the keys of the type's parameters (see
    // EffectType); one left out keeps its initial value.
    std::map<std::string, double> parameters;
    bool muted = false;  // passes on silence (see Graph::setMuted)
    std::string selected;  // a selector: the node whose link into it it lets through (empty: the first)
    std::map<std::string, std::string> names;  // a selector: names given to its inputs, by the id of the node each comes from
    bool hasPosition = false;  // where the graphical editor drew it
    float x = 0, y = 0;
};

struct LinkDescription {
    std::string from, to;
    double gainDb = 0.0;
    bool muted = false;  // carries silence
};

struct GraphDescription {
    double marginMs = 4.0;
    bool closeToTray = true;  // graphical editor: closing the window keeps it running
    bool showApps = true;     // graphical editor: boxes list the applications that use their device
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

// The node a selector lets through: the one it names if a link from it comes
// in, otherwise the first that does (empty with none). `inputs` in link order.
std::string selectedInput(const std::string& named, const std::vector<std::string>& inputs);

}  // namespace wolfe
