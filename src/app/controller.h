#pragma once

#include "config.h"
#include "devices.h"
#include "graph.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wolfe {

// What a box on the canvas is. Audio comes out of sources (pin on the right)
// and goes into sinks (pin on the left); a gain has both.
enum class NodeKind {
    Input,         // source: a recording device of the computer (a microphone, the far end of a cable...)
    WolfeSpeaker,  // source: one of Wolfe's own playback devices; what applications play on it comes in
    Gain,          // passes on what comes into it, louder or quieter
    Output,        // sink: a playback device of the computer (headphones, speakers...)
    WolfeMic,      // sink: one of Wolfe's own recording devices; applications that record from it hear it
};

// A box on the canvas. Every box is one the user made.
struct Node {
    int uid = 0;          // number used by the canvas; changes from one run to the next
    std::string id;       // name inside the engine and in the saved file
    NodeKind kind = NodeKind::Gain;
    std::string type;     // as in the file: "input", "output", "virtual_speaker", "virtual_mic", "gain"
    // Devices: the device's id, or part of its name until it is found.
    // Wolfe's devices: their name.
    std::string target;
    std::string serial;    // Wolfe's devices: their identity for Windows
    std::string endpoint;  // Wolfe's devices: the id Windows gave the device, once it has it
    std::string label;
    std::string detail;    // second line under the title
    float gainDb = 0.0f;   // gains

    bool resolved = false;  // devices: `target` is the device's own id, not part of its name
    bool present = false;   // the device is plugged in (Wolfe's: Windows has it)
    bool active = false;    // it exists in the engine
    bool healthy = false;   // in the engine and doing its job
    std::string status;     // what the engine says about it
    float level = 0.0f;     // recent peak, 0..1, falling back on its own

    bool hasPosition = false;
    float x = 0, y = 0;

    bool isWolfeDevice() const { return kind == NodeKind::WolfeSpeaker || kind == NodeKind::WolfeMic; }
    bool isDevice() const { return kind == NodeKind::Input || kind == NodeKind::Output; }
    // Whether it has a pin audio comes out of / goes into.
    bool givesLinks() const { return kind == NodeKind::Input || kind == NodeKind::WolfeSpeaker || kind == NodeKind::Gain; }
    bool takesLinks() const { return kind == NodeKind::Output || kind == NodeKind::WolfeMic || kind == NodeKind::Gain; }
};

// A link: audio that the engine carries from one node to another, as it is.
struct Connection {
    int uid = 0;
    int from = 0, to = 0;  // node uids
};

// The devices on the computer at one moment, Wolfe's own included (they
// have a wolfeSerial).
struct Discovery {
    std::vector<DeviceInfo> inputs, outputs;
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

    int addGain(float x, float y);
    void setGainDb(int nodeUid, float gainDb);
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

    bool closeToTray() const { return closeToTray_; }
    void setCloseToTray(bool value);

    // While nobody is looking at the canvas the system is looked at less often.
    void setWatched(bool someoneIsLooking);

    static constexpr float kMinGainDb = -100.0f, kMaxGainDb = 40.0f;

private:
    Node* find(int uid);
    Node* findById(const std::string& id);
    int insertNode(Node node);
    std::string adopt(const GraphDescription& description);
    void clear();

    std::string link(int fromUid, int toUid);
    void unlink(int linkUid);
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
    Discovery pending_;
};

}  // namespace wolfe
