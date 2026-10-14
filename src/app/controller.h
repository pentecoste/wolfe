#pragma once

#include "config.h"
#include "devices.h"
#include "graph.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wolfe {

enum class NodeKind { Input, Loopback, App, Effect, Output };

// A box on the canvas.
struct Node {
    int uid = 0;          // number used by the canvas; changes from one run to the next
    std::string id;       // name inside the engine and in the saved file
    NodeKind kind = NodeKind::Effect;
    std::string type;     // "input", "loopback", "app", "output", or the effect type
    std::string target;   // device or application; empty for effects
    std::string label;
    std::string detail;   // second line: where the application lives

    bool resolved = false;  // devices: `target` is the device's own id, not part of its name
    bool present = false;   // the device is plugged in / the application is running
    bool active = false;    // links made in Wolfe use it, so it exists in the engine
    bool monitoring = false;  // playback devices: Wolfe is listening to what is played on it
    bool healthy = false;   // in the engine and doing its job
    bool kept = false;      // remembered in the file even with no links (the user placed it)
    std::string status;     // what the engine says about it
    float level = 0.0f;     // recent peak, 0..1, falling back on its own

    // Applications only.
    bool parked = false;    // the user took it off every playback device (remembered in the file)
    bool silenced = false;  // Windows is playing it on the silent device, where nobody hears it
    bool ownOutput = false, ownInput = false;  // Windows was told which device to use for it

    bool hasPosition = false;
    float x = 0, y = 0;
    bool shown = false;     // it was on the canvas the last time boxes were given their places

    // Whether a link made in Wolfe can end here.
    bool takesLinks() const { return kind == NodeKind::Effect || kind == NodeKind::Output; }
    // Whether Wolfe is using it at all.
    bool used() const { return active || monitoring; }
};

// A link made in Wolfe: audio that the engine carries from one node to another.
// One that starts at a playback device carries what is being played on it.
struct Connection {
    int uid = 0;
    int from = 0, to = 0;  // node uids
    float gainDb = 0.0f;
    // Windows is playing the application on that same device by itself: this
    // copy would be heard there a second time, so it carries nothing for now.
    bool suspended = false;
};

// A connection that Windows makes, shown so that the canvas matches what is
// really happening on the computer. Wolfe carries none of this audio; it can
// ask Windows to connect an application to a different device.
struct SystemLink {
    enum Kind {
        Plays,    // an application is playing on an output device
        Records,  // an application is recording from an input device
        Cable,    // the two ends of a virtual cable
        Listens,  // Windows plays a recording device on a playback device ("Listen to this device")
    };
    int uid = 0;
    int from = 0, to = 0;  // node uids
    Kind kind = Plays;
    bool active = false;   // audio is flowing, as opposed to a stream that is merely open
};

// What exists on the computer at one moment, as far as the editor cares.
struct Discovery {
    std::vector<DeviceInfo> inputs, outputs;
    std::vector<std::string> apps;        // executables using audio that belong to no known application
    std::map<std::string, bool> running;  // for each known application: is it running?

    // A stream an application has open on a device.
    struct Session {
        std::string app;     // how the application is known: as in `running`, or as in `apps`
        std::string device;  // the device's id
        bool recording = false;
        bool active = false;
    };
    std::vector<Session> sessions;

    // The device Windows sends an application to when the application does
    // not pick one itself: its own, if it was given one, else the default.
    struct Choice {
        std::string device;  // the device's id
        bool own = false;
    };
    std::map<std::string, Choice> playsOn, recordsFrom;  // by application, as in `sessions`

    // A recording device that Windows plays on a playback device by itself.
    struct Listen {
        std::string input, output;  // device ids
    };
    std::vector<Listen> listens;
};

// Asks Windows to play an application on (or make it record from) a device;
// an empty id means the default one. Returns how many processes were told, or
// -1 when this version of Windows cannot do it.
using AppMover = std::function<int(const std::string& app, bool recording, const std::string& deviceId)>;

// Asks Windows to play a recording device on a playback device by itself, or
// to stop doing so. False if Windows refused.
using ListenSetter = std::function<bool(const std::string& inputId, bool on, const std::string& outputId)>;

// Everything the graphical editor does, without any drawing: the boxes and
// links on the canvas, kept in step with the engine, with Windows and with
// the file.
//
// A box only exists in the engine while something is connected to it, so a
// microphone that is merely shown is not opened.
//
// Use from one thread (the user-interface thread).
class Controller {
public:
    // With `watch` false nothing looks at the system in the background,
    // discoveries have to be handed in through apply(), and nothing is ever
    // asked of Windows unless setAppMover() says how (used by the tests).
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
    bool isSystemLink(int linkUid) const;
    const SystemLink* systemLink(int linkUid) const;
    // Whether the node has a pin on that side.
    bool showsInput(const Node& node) const;
    bool showsOutput(const Node& node) const;

    // Joins two boxes in whatever way does it: a link carried by Wolfe, a
    // request to Windows to move an application, a virtual cable into an
    // application that records. `replacedLinkUid` is the link the user pulled
    // off to make this one, if any: the two together are a move.
    // Returns an empty string, or why it cannot be done.
    std::string connect(int fromUid, int toUid, int replacedLinkUid = 0);
    // Either kind of link. Returns an empty string, or why it cannot be done.
    std::string disconnect(int linkUid);
    std::string disconnectAll(int nodeUid);
    void setGainDb(int linkUid, float gainDb);
    // A recording device can be carried to a playback device by Wolfe (with a
    // gain, and less delay) or by Windows (also while Wolfe is not running).
    // This hands such a link from the one to the other.
    bool canHandOver(int linkUid) const;
    std::string handOver(int linkUid);
    int addEffect(const std::string& type, float x, float y);
    bool removable(const Node& node) const;
    void removeNode(int nodeUid);
    void moveNode(int nodeUid, float x, float y);

    // The playback device nobody listens to, where an application goes when
    // it is taken off every other one. 0 when there is none.
    int silentDevice() const;
    void setSilentDevice(int outputUid);
    // Whether Windows was told anything about this application's devices,
    // and undoing all of it.
    bool managed(const Node& node) const { return node.parked || node.ownOutput || node.ownInput; }
    void giveBack(int appUid);
    // Applications taken off every device are only heard through Wolfe: when
    // Wolfe stops they go back to Windows' default device.
    void giveBackParked();

    // Something the user should be told (a request Windows did not carry out...). Empty when there is nothing.
    std::string takeNotice();

    // A few lines about a node or a link, for a tooltip.
    std::string describeNode(int nodeUid);
    std::string describeLink(int linkUid) const;  // either kind of link

    bool closeToTray() const { return closeToTray_; }
    void setCloseToTray(bool value);

    // While nobody is looking at the canvas the system is looked at less often.
    void setWatched(bool someoneIsLooking);

    void setAppMover(AppMover mover) { mover_ = std::move(mover); }
    void setListenSetter(ListenSetter setter) { listener_ = std::move(setter); }

private:
    // A request to Windows about one application, until it is seen to have worked.
    struct Move {
        int app = 0;
        bool recording = false;
        std::string device;  // empty: the default one
        bool park = false;   // the silent device, whichever it is when the request goes out
        bool sent = false;
        bool quiet = false;  // not worth telling the user about if it does not happen
        double since = 0;
    };

    Node* find(int uid);
    Node* findById(const std::string& id);
    int insertNode(Node node);
    std::string adopt(const GraphDescription& description);
    void clear();

    std::string link(int fromUid, int toUid);
    void unlink(int linkUid);
    std::string sourceId(const Node& node) const;
    void ensureSource(Node& node);
    void ensureSink(Node& node);
    void syncEngine(int nodeUid);
    bool reaches(int fromUid, int toUid) const;

    std::string connectToApp(Node& from, Node& app);
    std::string leaveDevice(Node& app);
    std::string listen(const Node& input, const Node* output);
    std::string park(Node& app);
    void requestMove(Node& app, bool recording, const std::string& device, bool park = false, bool quiet = false);
    void send(Move& move);
    void followMoves(const Discovery& discovery);
    const Node* partner(const Node& cableEnd) const;
    void notice(const std::string& text);
    void hurry();

    float columnOf(const Node& node) const;
    void place(Node& node);
    void settle();
    void syncDevices(const std::vector<DeviceInfo>& devices, NodeKind kind);
    void syncSystemLinks(const Discovery& discovery);
    void publishWatched();
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

    std::string silentDevice_;   // a device id, or part of a name until the device is seen; empty: none
    bool silentChosen_ = false;  // by the user, as opposed to picked because it looked right
    std::vector<Move> moves_;
    AppMover mover_;
    ListenSetter listener_;
    std::vector<std::string> notices_;

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
    double hurryUntil_ = 0;  // something was just asked of Windows: look again soon, several times
    bool eager_ = false;     // a request is waiting for its application to appear: keep looking, even unwatched
    bool hasPending_ = false;
    // Highest level read since the canvas last asked: by device id, and by application.
    std::map<std::string, float> devicePeaks_, appPeaks_;
    Discovery pending_;
    std::vector<std::string> watched_;  // application descriptions to report on
};

}  // namespace wolfe
