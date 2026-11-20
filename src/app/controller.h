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
#include <utility>
#include <vector>

namespace wolfe {

// What a box on the canvas is. Sources are on the left, where audio comes
// from; sinks on the right, where it goes; effects in between.
enum class NodeKind {
    PlayingApp,    // an application that plays on some device (shown only, never changed)
    Input,         // a recording device: a microphone, the far end of a virtual cable...
    WolfeSpeaker,  // one of Wolfe's own playback devices: applications play on it, Wolfe takes that in
    Loopback,      // what is played on a playback device
    Effect,
    Output,        // a playback device: Wolfe plays on it
    WolfeMic,      // one of Wolfe's own recording devices: Wolfe plays into it, applications record from it
    RecordingApp,  // an application that records from some device (shown only, never changed)
};

// A box on the canvas.
struct Node {
    int uid = 0;          // number used by the canvas; changes from one run to the next
    std::string id;       // name inside the engine and in the saved file
    NodeKind kind = NodeKind::Effect;
    std::string type;     // as in the file: "input", "output", "loopback", "virtual_speaker"..., or the effect type
    // Devices: the device's id, or part of its name until it is found.
    // Wolfe's devices: their name. Applications: the executable's path.
    std::string target;
    std::string serial;    // Wolfe's devices: their identity for Windows
    std::string endpoint;  // Wolfe's devices: the id Windows gave the device, once it has it
    std::string label;
    std::string detail;    // second line: where the application lives, what a loopback is

    bool resolved = false;  // devices: `target` is the device's own id, not part of its name
    bool present = false;   // the device is plugged in / the application has its audio open
    bool active = false;    // it exists in the engine
    bool healthy = false;   // in the engine and doing its job
    bool kept = false;      // remembered in the file even with no links (the user placed or made it)
    std::string status;     // what the engine says about it
    float level = 0.0f;     // recent peak, 0..1, falling back on its own

    bool hasPosition = false;
    float x = 0, y = 0;
    bool shown = false;     // it was on the canvas the last time boxes were given their places

    bool isApp() const { return kind == NodeKind::PlayingApp || kind == NodeKind::RecordingApp; }
    bool isWolfeDevice() const { return kind == NodeKind::WolfeSpeaker || kind == NodeKind::WolfeMic; }
    // Whether a link made in Wolfe can start / end here.
    bool givesLinks() const {
        return kind == NodeKind::Input || kind == NodeKind::WolfeSpeaker || kind == NodeKind::Loopback ||
               kind == NodeKind::Effect;
    }
    bool takesLinks() const { return kind == NodeKind::Effect || kind == NodeKind::Output || kind == NodeKind::WolfeMic; }
};

// A link made in Wolfe: audio that the engine carries from one node to another.
struct Connection {
    int uid = 0;
    int from = 0, to = 0;  // node uids
    float gainDb = 0.0f;
};

// An application and the device Windows has it play on or record from, shown
// so that the canvas matches what really happens on the computer. Wolfe does
// not change these: that is done in Windows' settings, or in the application's.
struct SystemLink {
    int uid = 0;
    int from = 0, to = 0;  // node uids: application -> device, or device -> application
    bool active = false;   // audio is flowing, as opposed to a stream that is merely open
};

// What exists on the computer at one moment, as far as the editor cares.
struct Discovery {
    // Every usable device, Wolfe's own included (they have a wolfeSerial).
    std::vector<DeviceInfo> inputs, outputs;

    // A stream an application has open on a device.
    struct Session {
        std::string app;     // the path of its executable
        std::string device;  // the device's id
        bool recording = false;
        bool active = false;
    };
    std::vector<Session> sessions;
};

// Everything the graphical editor does, without any drawing: the boxes and
// links on the canvas, kept in step with the engine, with Windows and with
// the file.
//
// A device only exists in the engine while something is connected to it, so a
// microphone that is merely shown is not opened. Wolfe's own devices always
// exist, so that applications can be given them in Windows' settings.
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
    // Replaces everything with the graph in another file.
    std::string importFrom(const std::string& path);
    void save();

    // Call regularly: picks up discoveries, refreshes statuses and levels,
    // saves a moment after the last change.
    void update(double now);
    void apply(const Discovery& discovery);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Connection>& links() const { return links_; }
    const std::vector<SystemLink>& systemLinks() const { return systemLinks_; }
    const Node* node(int uid) const;
    bool visible(const Node& node) const;
    bool linked(int nodeUid) const;
    const SystemLink* systemLink(int linkUid) const;
    // Whether the node has a pin on that side.
    bool showsInput(const Node& node) const;
    bool showsOutput(const Node& node) const;

    // A link carried by Wolfe. `replacedLinkUid` is the link the user pulled
    // off to make this one, if any: the two together are a move.
    // Returns an empty string, or why it cannot be done.
    std::string connect(int fromUid, int toUid, int replacedLinkUid = 0);
    // Returns an empty string, or why it cannot be done (a link Windows makes).
    std::string disconnect(int linkUid);
    void disconnectAll(int nodeUid);
    void setGainDb(int linkUid, float gainDb);

    int addEffect(const std::string& type, float x, float y);
    // A box for what is played on a playback device. 0 if it cannot be made.
    int addLoopback(int outputUid, float x, float y);
    // One of Wolfe's own devices, which Windows will have as long as Wolfe
    // runs. Returns its uid, or 0 with the reason in `problem`.
    int addWolfeDevice(bool recording, const std::string& name, float x, float y, std::string* problem);
    // A name for a new device of Wolfe's that no other has.
    std::string freeWolfeName(bool recording) const;
    bool removable(const Node& node) const;
    void removeNode(int nodeUid);
    void moveNode(int nodeUid, float x, float y);

    // Something the user should be told. Empty when there is nothing.
    std::string takeNotice();

    // A few lines about a node or a link, for a tooltip.
    std::string describeNode(int nodeUid);
    std::string describeLink(int linkUid) const;  // either kind of link

    bool closeToTray() const { return closeToTray_; }
    void setCloseToTray(bool value);

    // While nobody is looking at the canvas the system is looked at less often.
    void setWatched(bool someoneIsLooking);

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
    std::string whyNotFrom(const Node& node) const;
    std::string whyNotInto(const Node& node) const;
    void notice(const std::string& text);

    float columnOf(const Node& node) const;
    void place(Node& node);
    void settle();
    void syncDevices(const std::vector<DeviceInfo>& devices, NodeKind kind);
    void syncApps(const Discovery& discovery);
    void markDirty();
    void watchMain();

    std::string documentPath_;
    Graph graph_;
    std::vector<Node> nodes_;
    std::vector<Connection> links_;
    std::vector<SystemLink> systemLinks_;
    int nextUid_ = 1;
    int nextEffect_ = 1;
    double marginMs_ = 4.0;
    bool closeToTray_ = true;
    // Where the user put applications that are not on the canvas right now.
    std::map<std::string, std::pair<float, float>> appPlaces_;
    std::vector<std::string> notices_;
    std::set<std::string> toldDefault_;  // Wolfe's devices the user was told Windows made the default

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
    // Highest level read since the canvas last asked: by device id, and by
    // application box id.
    std::map<std::string, float> devicePeaks_, appPeaks_;
    Discovery pending_;
};

}  // namespace wolfe
