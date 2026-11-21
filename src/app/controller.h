#pragma once

#include "config.h"
#include "devices.h"
#include "graph.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace wolfe {

// What a box on the canvas is. Audio comes out of sources (pin on the right)
// and goes into sinks (pin on the left); an effect has both.
enum class NodeKind {
    Input,         // source: a recording device of the computer (a microphone, the far end of a cable...)
    WolfeSpeaker,  // source: one of Wolfe's own playback devices; what applications play on it comes in
    Effect,        // changes what comes into it and passes it on: a gain, a filter, a reverb... (see effect.h)
    Output,        // sink: a playback device of the computer (headphones, speakers...)
    WolfeMic,      // sink: one of Wolfe's own recording devices; applications that record from it hear it
                   // (what goes into it can also be linked onward, as from a source)
};

// An application that has a stream open on a node's device: one that plays
// on it, or (a recording device) one that records from it.
struct AppUse {
    std::string name;     // as the program calls itself: "Spotify", "Google Chrome"
    bool active = false;  // audio is flowing, as opposed to a stream that is merely open
};

// A box on the canvas. Every box is one the user made.
struct Node {
    int uid = 0;          // number used by the canvas; changes from one run to the next
    std::string id;       // name inside the engine and in the saved file
    NodeKind kind = NodeKind::Effect;
    std::string type;     // as in the file: "input", "output", "virtual_speaker", "virtual_mic", or an effect type
    // Devices: the device's id, or part of its name until it is found.
    // Wolfe's devices: their name.
    std::string target;
    std::string serial;    // Wolfe's devices: their identity for Windows
    std::string endpoint;  // Wolfe's devices: the id Windows gave the device, once it has it
    std::string label;
    std::string detail;    // second line under the title
    // Effects: the value of each of the type's parameters, in their order.
    std::vector<float> parameters;
    bool muted = false;   // passes on silence, inside Wolfe only
    // A selector: the id of the node whose link into it goes through, as the
    // user chose it (see Controller::selectedInput()).
    std::string selected;
    // A selector: the names the user gave its inputs, by the id of the node
    // each comes from (see Controller::inputName()).
    std::map<std::string, std::string> inputNames;

    bool resolved = false;  // devices: `target` is the device's own id, not part of its name
    bool present = false;   // the device is plugged in (Wolfe's: Windows has it)
    bool active = false;    // it exists in the engine
    bool healthy = false;   // in the engine and doing its job
    std::string status;     // what the engine says about it
    float level = 0.0f;     // recent peak, 0..1, falling back on its own: what comes out of it (a sink: what it plays)
    float inputLevel = 0.0f;  // effects: the same for what goes into it
    // Devices: the applications using it, those with audio flowing first
    // (Wolfe itself left out). Known only while somebody looks at the canvas.
    std::vector<AppUse> apps;

    bool hasPosition = false;
    float x = 0, y = 0;

    bool isWolfeDevice() const { return kind == NodeKind::WolfeSpeaker || kind == NodeKind::WolfeMic; }
    bool isDevice() const { return kind == NodeKind::Input || kind == NodeKind::Output; }
    bool isSelector() const { return kind == NodeKind::Effect && type == "select"; }
    // Whether it has a pin audio comes out of / goes into.
    bool givesLinks() const {
        return kind == NodeKind::Input || kind == NodeKind::WolfeSpeaker || kind == NodeKind::Effect || kind == NodeKind::WolfeMic;
    }
    bool takesLinks() const { return kind == NodeKind::Output || kind == NodeKind::WolfeMic || kind == NodeKind::Effect; }
};

// A link: audio that the engine carries from one node to another, as it is.
struct Connection {
    int uid = 0;
    int from = 0, to = 0;  // node uids
    bool muted = false;    // carries silence
};

// The devices on the computer at one moment, Wolfe's own included (they
// have a wolfeSerial), and the applications using them.
struct Discovery {
    std::vector<DeviceInfo> inputs, outputs;

    // A stream an application has open on a device.
    struct Session {
        std::string app;     // the program's name
        std::string device;  // the device's id
        bool recording = false;
        bool active = false;
    };
    std::vector<Session> sessions;
};

// Everything the graphical editor does, without any drawing: the boxes and
// links on the canvas, kept in step with the engine, with the computer's
// devices and with the file.
//
// A device of the computer is only opened while something is linked to it.
// Wolfe's own devices always exist, so that applications can be given them
// in Windows' settings.
//
// Use from one thread (the user-interface thread).
class Controller {
public:
    // With `watch` false nothing looks at the system in the background,
    // discoveries have to be handed in through apply(), and Wolfe's own
    // devices are never given to Windows (used by the tests).
    explicit Controller(std::string documentPath, bool watch = true);
    ~Controller();

    // Loads the document if there is one. Returns what could not be used, if anything.
    std::string load();
    void save();

    // The graph in a file of the user's choosing, to take to another
    // computer. Returns an empty string, or what went wrong.
    std::string exportTo(const std::string& path);
    // Replaces the graph with one from a file. The computer's devices are
    // found by their names when their ids are unknown here (another
    // computer); Wolfe's devices keep their identity, so a graph brought back
    // to the computer it came from finds its applications again. This
    // computer's preferences stay as they are. Returns what could not be
    // used, if anything; nothing is changed when the file cannot be read.
    std::string importFrom(const std::string& path, bool* imported = nullptr);
    // A first graph to start from: music played on a Wolfe speaker goes to
    // the headphones and, quieter, into a Wolfe microphone, together with the
    // computer's microphone cleaned of noise. Uses the default devices.
    // Returns what could not be made, if anything.
    std::string buildExample();

    // Call regularly: picks up discoveries, refreshes statuses and levels,
    // saves a moment after the last change.
    void update(double now);
    void apply(const Discovery& discovery);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Connection>& links() const { return links_; }
    const Node* node(int uid) const;
    bool linked(int nodeUid) const;

    // Returns an empty string, or why it cannot be done.
    std::string connect(int fromUid, int toUid);
    // A link taken off one pin and dropped on another: the two together are
    // a move, which leaves the old link in place if the new one cannot be made.
    std::string move(int linkUid, int fromUid, int toUid);
    void disconnect(int linkUid);
    void disconnectAll(int nodeUid);
    // A muted link carries silence; the boxes at its ends go on as they were.
    void setLinkMuted(int linkUid, bool muted);

    // A selector lets through one of the nodes linked into it: the one the
    // user chose, or the first if that one is not linked any more.
    void select(int selectorUid, int fromUid);
    int selectedInput(int selectorUid) const;  // its uid; 0 with nothing linked
    // What a selector calls one of its inputs: the name the user gave it, or
    // the source's own (a device's without its adapter).
    std::string inputName(int selectorUid, int fromUid) const;
    // A name of the user's for an input of a selector; empty (or the source's
    // own name) goes back to the source's own. At most 40 letters.
    void renameInput(int selectorUid, int fromUid, const std::string& name);
    bool inputRenamed(int selectorUid, int fromUid) const;
    // The nodes linked into a node, in the order their links were made.
    std::vector<int> inputsOf(int nodeUid) const;
    // The links that carry no sound because of a mute (of a box or a link,
    // here or anywhere before) or a selector, for the canvas to grey out.
    std::set<int> silentLinks() const;

    // An effect of one of the types effectTypes() offers. 0 for any other type.
    int addEffect(const std::string& type, float x, float y);
    // One of an effect's settings, by its index in the type's parameters;
    // kept within the parameter's range.
    void setParameter(int nodeUid, int index, float value);
    // Every setting of an effect back to its initial value.
    void resetParameters(int nodeUid);
    // A muted box passes on silence: what comes out of a source or an effect,
    // what goes into a sink. The device itself is not touched.
    void setMuted(int nodeUid, bool muted);
    // A box for a device of the computer, by its id (see devicesToAdd()). 0 if it cannot be made.
    int addDevice(bool recording, const std::string& deviceId, float x, float y);
    // The computer's devices that are not on the canvas yet, Wolfe's own left out.
    std::vector<DeviceInfo> devicesToAdd(bool recording) const;
    // One of Wolfe's own devices, which Windows will have as long as Wolfe
    // runs. Returns its uid, or 0 with the reason in `problem`.
    int addWolfeDevice(bool recording, const std::string& name, float x, float y, std::string* problem);
    // A name for a new device of Wolfe's that no other has.
    std::string freeWolfeName(bool recording) const;
    void removeNode(int nodeUid);
    void moveNode(int nodeUid, float x, float y);

    // A few lines about a node or a link, for a tooltip.
    std::string describeNode(int nodeUid);
    std::string describeLink(int linkUid) const;
    // What a box is for, for its "?".
    static std::string helpFor(const Node& node);

    bool closeToTray() const { return closeToTray_; }
    void setCloseToTray(bool value);
    // Whether boxes list the applications that use their device (tooltips always do).
    bool showApps() const { return showApps_; }
    void setShowApps(bool value);

    // While nobody is looking at the canvas the system is looked at less often.
    void setWatched(bool someoneIsLooking);

private:
    Node* find(int uid);
    Node* findById(const std::string& id);
    int insertNode(Node node);
    std::string adopt(const GraphDescription& description);
    void clear();

    std::string link(int fromUid, int toUid, bool muted = false);
    void unlink(int linkUid);
    float gainOf(const Connection& link) const;  // what the engine is told to carry
    void applyGains(int toUid);                   // after a change to what goes into a node
    GraphDescription describe() const;
    void expectNewDevice(const std::string& serial);
    void ensureEngine(Node& node);
    void syncEngine(int nodeUid);
    bool reaches(int fromUid, int toUid) const;
    std::string newId(const char* prefix) const;

    void place(Node& node);
    void syncDevices(const std::vector<DeviceInfo>& devices, NodeKind kind);
    void markDirty();
    void watchMain();

    std::string documentPath_;
    Graph graph_;
    std::vector<Node> nodes_;
    std::vector<Connection> links_;
    Discovery devices_;  // the last look at the computer's devices
    int nextUid_ = 1;
    double marginMs_ = 4.0;
    bool closeToTray_ = true;
    bool showApps_ = true;

    bool dirty_ = false;
    double dirtySince_ = 0;
    double now_ = 0;

    // Shared with the thread that watches the system.
    std::thread watcher_;
    std::mutex watchMutex_;
    std::condition_variable watchWake_;
    bool stopWatching_ = false;
    bool lookOften_ = true;
    bool lookNow_ = false;
    bool hasPending_ = false;
    std::map<std::string, float> devicePeaks_;  // highest level read since the canvas last asked, by device id
    std::vector<std::string> newDevices_;       // serials of Wolfe devices Windows has never had (see DefaultKeeper)
    Discovery pending_;
};

}  // namespace wolfe
