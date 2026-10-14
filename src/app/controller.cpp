#include "controller.h"

#include "app_devices.h"
#include "listening.h"
#include "process_finder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>

namespace wolfe {

namespace {

NodeKind kindOfType(const std::string& type) {
    if (type == "input") return NodeKind::Input;
    if (type == "loopback") return NodeKind::Loopback;
    if (type == "app") return NodeKind::App;
    if (type == "output") return NodeKind::Output;
    return NodeKind::Effect;
}

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

std::string effectLabel(const std::string& type) {
    if (type == "rnnoise") return "Noise suppression";
    if (type == "mix") return "Mix";
    return type;
}

// "C:\Program Files\Soundrio\bin\java.exe" -> "java.exe"
std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of('\\');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "C:\Program Files\Soundrio\bin\java.exe" -> "Soundrio\bin": enough to tell
// two programs with the same executable name apart.
std::string folderHint(const std::string& path) {
    size_t last = path.find_last_of('\\');
    if (last == std::string::npos || last == 0) return {};
    size_t previous = path.find_last_of('\\', last - 1);
    if (previous == std::string::npos || previous == 0) return path.substr(0, last);
    size_t beforeThat = path.find_last_of('\\', previous - 1);
    size_t start = beforeThat == std::string::npos ? 0 : beforeThat + 1;
    return path.substr(start, last - start);
}

// What to call an application given only how it is described in a file:
// "\Soundrio\" -> "Soundrio", "chrome.exe" -> "chrome.exe".
std::string appLabel(std::string target) {
    while (!target.empty() && (target.back() == '\\' || target.back() == '/')) target.pop_back();
    size_t slash = target.find_last_of("\\/");
    return slash == std::string::npos ? target : target.substr(slash + 1);
}

// "CABLE Input (VB-Audio Virtual Cable)" -> "VB-Audio Virtual Cable"
std::string adapterOf(const std::string& deviceName) {
    size_t open = deviceName.rfind(" (");
    if (open == std::string::npos || deviceName.empty() || deviceName.back() != ')') return {};
    return deviceName.substr(open + 2, deviceName.size() - open - 3);
}

// Windows does not say that a playback device and a recording device are the
// two ends of a virtual cable, so this goes by the names of the known ones.
bool looksLikeVirtualCable(const std::string& adapter) {
    const std::string name = toLower(adapter);
    for (const char* known : {"vb-audio", "virtual cable", "virtual audio cable", "steam streaming"}) {
        if (name.find(known) != std::string::npos) return true;
    }
    return false;
}

// Steam's devices exist for Steam to stream through. Nobody listens to them
// otherwise, which makes them a good place to send what should not be heard;
// as a cable they only carry audio properly while Steam is streaming.
bool belongsToSteam(const std::string& deviceName) { return toLower(deviceName).find("steam streaming") != std::string::npos; }

// Roughly the size of a box on the canvas, in the units positions are kept in.
const float kBoxWidth = 190.0f, kBoxHeight = 60.0f;

bool engineIsFine(const std::string& status) {
    return startsWith(status, "capturing") || startsWith(status, "playing") || startsWith(status, "running");
}

const Discovery::Choice* choiceOf(const std::map<std::string, Discovery::Choice>& choices, const std::string& app) {
    const std::string wanted = toLower(app);
    for (const auto& [name, choice] : choices) {
        if (toLower(name) == wanted) return &choice;
    }
    return nullptr;
}

// The real AppMover.
//
// A device is given to the processes of the application that have a stream of
// that kind open: those are the ones Windows knows as its audio, and the
// choice is remembered for their executables. Giving back to the default is
// tried on every process of the application (Windows refuses the ones it has
// nothing recorded for).
int moveApplication(const std::string& app, bool recording, const std::string& deviceId) {
    AppDevices devices;
    if (!devices.available()) return -1;
    const EDataFlow flow = recording ? eCapture : eRender;
    ProcessSnapshot snapshot;
    const std::map<DWORD, int> family = snapshot.family(app);

    std::set<DWORD> processes;
    if (deviceId.empty()) {
        for (const auto& [pid, depth] : family) processes.insert(pid);
    } else {
        for (const AudioSession& session : audioSessions()) {
            if (session.recording == recording && family.count(session.pid)) processes.insert(session.pid);
        }
    }
    int told = 0;
    for (DWORD pid : processes) told += devices.set(pid, flow, toWide(deviceId)) ? 1 : 0;
    logLine("%s: %d process(es) told to %s %s", app.c_str(), told, recording ? "record from" : "play on",
            deviceId.empty() ? "the default device" : deviceId.c_str());
    return told;
}

}  // namespace

// Where a box goes when nobody has placed it. Columns run left to right the
// way audio usually travels: sources, effects, playback devices, the far ends
// of virtual cables, and the applications that record.
float Controller::columnOf(const Node& node) const {
    bool cableEnd = false, records = false;
    for (const SystemLink& link : systemLinks_) {
        if (link.to != node.uid) continue;
        cableEnd |= link.kind == SystemLink::Cable;
        records |= link.kind == SystemLink::Records;
    }
    switch (node.kind) {
        case NodeKind::Effect: return 330.0f;
        case NodeKind::Output: return 620.0f;
        case NodeKind::Input: return cableEnd ? 910.0f : 40.0f;
        case NodeKind::App: return records ? 1200.0f : 40.0f;
        default: return 40.0f;
    }
}

Controller::Controller(std::string documentPath, bool watch) : documentPath_(std::move(documentPath)) {
    graph_.start();
    if (watch) {
        mover_ = moveApplication;
        listener_ = [](const std::string& inputId, bool on, const std::string& outputId) {
            const bool done = setListening(toWide(inputId), on, toWide(outputId));
            logLine("\"Listen to this device\" %s for %s: %s", on ? "on" : "off", inputId.c_str(), done ? "done" : "refused");
            return done;
        };
        watcher_ = std::thread([this] { watchMain(); });
    }
}

Controller::~Controller() {
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        stopWatching_ = true;
    }
    watchWake_.notify_all();
    if (watcher_.joinable()) watcher_.join();
    if (dirty_) save();
    giveBackParked();
    graph_.stop();
}

// ---------------------------------------------------------------------------
// Looking things up
// ---------------------------------------------------------------------------

Node* Controller::find(int uid) {
    for (Node& node : nodes_) {
        if (node.uid == uid) return &node;
    }
    return nullptr;
}

const Node* Controller::node(int uid) const { return const_cast<Controller*>(this)->find(uid); }

Node* Controller::findById(const std::string& id) {
    for (Node& node : nodes_) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

bool Controller::linked(int nodeUid) const {
    for (const Connection& link : links_) {
        if (link.from == nodeUid || link.to == nodeUid) return true;
    }
    return false;
}

// Things the user made are always shown; devices and applications are shown
// while they exist, or while something is still connected to them.
bool Controller::visible(const Node& node) const {
    if (node.kind == NodeKind::Effect || node.kind == NodeKind::Loopback) return true;
    return node.present || linked(node.uid);
}

const SystemLink* Controller::systemLink(int linkUid) const {
    for (const SystemLink& link : systemLinks_) {
        if (link.uid == linkUid) return &link;
    }
    return nullptr;
}

bool Controller::isSystemLink(int linkUid) const { return systemLink(linkUid) != nullptr; }

// Audio goes into effects and playback devices; into an application only
// while it records, and into a recording device only when it is the far end
// of a virtual cable (and then only by way of the near end).
bool Controller::showsInput(const Node& node) const {
    if (node.takesLinks()) return true;
    for (const SystemLink& link : systemLinks_) {
        if (link.to == node.uid) return true;
    }
    return false;
}

// Audio can be taken out of everything: what a device records, what an
// application plays, what an effect makes, what is being played on a device.
bool Controller::showsOutput(const Node&) const { return true; }

bool Controller::removable(const Node& node) const {
    return node.kind == NodeKind::Effect || node.kind == NodeKind::Loopback || !node.present;
}

int Controller::silentDevice() const {
    if (silentDevice_.empty()) return 0;
    for (const Node& node : nodes_) {
        if (node.kind == NodeKind::Output && node.resolved && node.target == silentDevice_) return node.uid;
    }
    return 0;
}

// The other end of a virtual cable, if the device is one end of one.
const Node* Controller::partner(const Node& cableEnd) const {
    for (const SystemLink& link : systemLinks_) {
        if (link.kind != SystemLink::Cable) continue;
        if (link.from == cableEnd.uid) return node(link.to);
        if (link.to == cableEnd.uid) return node(link.from);
    }
    return nullptr;
}

// Whether audio leaving `fromUid` arrives at `toUid`: along links made in
// Wolfe (a playback device passes on what it is given, through its monitor),
// from one end of a virtual cable to the other, and from a recording device
// to the playback device Windows plays it on.
bool Controller::reaches(int fromUid, int toUid) const {
    std::set<int> seen;
    std::function<bool(int)> visit = [&](int uid) {
        if (uid == toUid) return true;
        if (!seen.insert(uid).second) return false;
        for (const Connection& link : links_) {
            if (link.from == uid && visit(link.to)) return true;
        }
        for (const SystemLink& link : systemLinks_) {
            const bool carries = link.kind == SystemLink::Cable || link.kind == SystemLink::Listens;
            if (carries && link.from == uid && visit(link.to)) return true;
        }
        return false;
    };
    return visit(fromUid);
}

void Controller::notice(const std::string& text) { notices_.push_back(text); }

std::string Controller::takeNotice() {
    if (notices_.empty()) return {};
    std::string text = notices_.front();
    notices_.erase(notices_.begin());
    return text;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

std::string Controller::load() {
    if (!std::filesystem::exists(std::filesystem::path(toWide(documentPath_)))) return {};
    std::string problems;
    try {
        problems = adopt(loadDescription(documentPath_));
    } catch (const std::exception& error) {
        return error.what();
    }
    dirty_ = false;
    return problems;
}

std::string Controller::importFrom(const std::string& path) {
    std::string problems;
    try {
        GraphDescription description = loadDescription(path);
        // These belong to this installation, not to the graph.
        description.closeToTray = closeToTray_;
        description.silentDevice = !silentChosen_ ? "" : silentDevice_.empty() ? "none" : silentDevice_;
        problems = adopt(description);
    } catch (const std::exception& error) {
        return error.what();
    }
    markDirty();
    return problems;
}

void Controller::save() {
    GraphDescription description;
    description.marginMs = marginMs_;
    description.closeToTray = closeToTray_;
    if (silentChosen_) description.silentDevice = silentDevice_.empty() ? "none" : silentDevice_;
    for (const Node& node : nodes_) {
        if (!node.kept && !node.parked && !linked(node.uid)) continue;
        NodeDescription out;
        out.id = node.id;
        out.type = node.type;
        out.target = node.target;
        out.label = node.label;
        out.hasPosition = node.hasPosition;
        out.x = node.x;
        out.y = node.y;
        out.parked = node.parked;
        description.nodes.push_back(out);

        // What is taken from a playback device is, in the file as in the
        // engine, a node of its own: a monitor of that device.
        bool listenedTo = false;
        for (const Connection& link : links_) listenedTo |= link.from == node.uid;
        if (node.kind == NodeKind::Output && listenedTo) {
            NodeDescription monitor;
            monitor.id = sourceId(node);
            monitor.type = "loopback";
            monitor.target = node.target;
            description.nodes.push_back(monitor);
        }
    }
    for (const Connection& link : links_) {
        description.links.push_back({sourceId(*node(link.from)), node(link.to)->id, link.gainDb});
    }

    dirty_ = false;
    try {
        std::filesystem::path path(toWide(documentPath_));
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        saveDescription(documentPath_, description);
    } catch (const std::exception& error) {
        logLine("could not save the graph: %s", error.what());
    }
}

void Controller::clear() {
    giveBackParked();
    for (const Node& node : nodes_) {
        if (node.active) graph_.removeNode(node.id);
        if (node.monitoring) graph_.removeNode(sourceId(node));
    }
    nodes_.clear();
    links_.clear();
    systemLinks_.clear();
    moves_.clear();
}

std::string Controller::adopt(const GraphDescription& description) {
    clear();
    marginMs_ = description.marginMs;
    closeToTray_ = description.closeToTray;
    silentChosen_ = !description.silentDevice.empty();
    silentDevice_ = description.silentDevice == "none" ? std::string() : description.silentDevice;
    graph_.setMarginMs(marginMs_);

    std::string problems;
    const auto add = [&](const NodeDescription& in) {
        Node node;
        node.id = in.id;
        node.type = in.type;
        node.kind = kindOfType(in.type);
        node.target = in.target;
        if (node.kind == NodeKind::Effect && !createEffect(node.type)) {
            problems += "unknown node type \"" + node.type + "\"\n";
            return;
        }
        if (findById(node.id)) {
            problems += "two nodes are called \"" + node.id + "\"\n";
            return;
        }
        if (!in.label.empty()) node.label = in.label;
        else if (node.kind == NodeKind::Effect) node.label = effectLabel(node.type);
        else if (node.kind == NodeKind::App) node.label = appLabel(node.target);
        else node.label = node.target;
        // A device written by its own id looks like "{0.0.1.00000000}.{...}".
        node.resolved = startsWith(node.target, "{");
        node.present = node.kind == NodeKind::Effect;
        node.kept = true;
        node.parked = in.parked && node.kind == NodeKind::App;
        node.hasPosition = in.hasPosition;
        node.x = in.x;
        node.y = in.y;
        if (startsWith(node.id, "fx|")) nextEffect_ = std::max(nextEffect_, atoi(node.id.c_str() + 3) + 1);
        insertNode(node);
    };

    // The monitor of a playback device that is itself in the file (see save())
    // is not a box of its own: its links start at the device.
    std::map<std::string, std::string> monitorOf;
    std::vector<const NodeDescription*> monitors;
    for (const NodeDescription& in : description.nodes) {
        if (in.type == "loopback" && startsWith(in.id, "mon|")) monitors.push_back(&in);
        else add(in);
    }
    for (const NodeDescription* in : monitors) {
        const Node* output = findById(in->id.substr(4));
        if (output && output->kind == NodeKind::Output) monitorOf[in->id] = output->id;
        else add(*in);
    }

    for (const LinkDescription& in : description.links) {
        auto monitored = monitorOf.find(in.from);
        Node* from = findById(monitored == monitorOf.end() ? in.from : monitored->second);
        Node* to = findById(in.to);
        if (!from || !to) {
            problems += "a link uses a node that is not there (\"" + in.from + "\" -> \"" + in.to + "\")\n";
            continue;
        }
        std::string error = link(from->uid, to->uid);
        if (!error.empty()) {
            problems += error + "\n";
            continue;
        }
        setGainDb(links_.back().uid, (float)in.gainDb);
    }

    // Applications that were kept off every device go back there as soon as
    // they are seen (they were given back to Windows when Wolfe last stopped).
    for (const Node& node : nodes_) {
        if (!node.parked) continue;
        Move move;
        move.app = node.uid;
        move.park = true;
        move.quiet = true;
        moves_.push_back(move);
    }

    settle();
    publishWatched();
    return problems;
}

void Controller::markDirty() {
    if (!dirty_) dirtySince_ = now_;
    dirty_ = true;
}

void Controller::setWatched(bool someoneIsLooking) {
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        if (lookOften_ == someoneIsLooking) return;
        lookOften_ = someoneIsLooking;
        lookNow_ = someoneIsLooking;  // the canvas was just opened: bring it up to date at once
    }
    watchWake_.notify_all();
}

void Controller::hurry() {
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        hurryUntil_ = nowSeconds() + 2.5;
        lookNow_ = true;
    }
    watchWake_.notify_all();
}

void Controller::setCloseToTray(bool value) {
    if (closeToTray_ == value) return;
    closeToTray_ = value;
    markDirty();
}

// ---------------------------------------------------------------------------
// Links made in Wolfe
// ---------------------------------------------------------------------------

int Controller::insertNode(Node node) {
    node.uid = nextUid_++;
    nodes_.push_back(std::move(node));
    return nodes_.back().uid;
}

// The engine node a link from this box starts at. What comes out of a
// playback device is what is being played on it: a monitor of the device,
// which the engine has as a separate input.
std::string Controller::sourceId(const Node& node) const {
    return node.kind == NodeKind::Output ? "mon|" + node.id : node.id;
}

void Controller::ensureSink(Node& node) {
    if (node.active) return;
    NodeDescription description;
    description.id = node.id;
    description.type = node.type;
    description.target = node.target;
    addNode(description, graph_);
    node.active = true;
}

void Controller::ensureSource(Node& node) {
    if (node.kind != NodeKind::Output) {
        ensureSink(node);
        return;
    }
    if (node.monitoring) return;
    graph_.addInput(sourceId(node), {InputKind::Loopback, node.target});
    node.monitoring = true;
}

// Takes out of the engine what the node's links no longer need.
void Controller::syncEngine(int nodeUid) {
    Node* node = find(nodeUid);
    if (!node) return;
    bool gives = false, takes = false;
    for (const Connection& link : links_) {
        gives |= link.from == nodeUid;
        takes |= link.to == nodeUid;
    }
    const bool isOutput = node->kind == NodeKind::Output;
    if (node->monitoring && !gives) {
        graph_.removeNode(sourceId(*node));
        node->monitoring = false;
    }
    if (node->active && !(isOutput ? takes : gives || takes)) {
        graph_.removeNode(node->id);
        node->active = false;
    }
    if (!node->used()) {
        node->healthy = false;
        node->status.clear();
    }
}

std::string Controller::link(int fromUid, int toUid) {
    Node* from = find(fromUid);
    Node* to = find(toUid);
    if (!from || !to) return "that node is not there any more";
    if (from == to) return "a node cannot be connected to itself";
    if (!to->takesLinks()) return "\"" + to->label + "\" is a source: nothing can go into it.";
    for (const Connection& link : links_) {
        if (link.from == fromUid && link.to == toUid) return "those two are already connected";
    }
    if (reaches(toUid, fromUid)) {
        return "That would make a loop: what comes out of \"" + to->label + "\" would go back into it.";
    }

    try {
        ensureSource(*from);
        ensureSink(*to);
        graph_.addLink(sourceId(*from), to->id, 1.0f);
    } catch (const std::exception& error) {
        syncEngine(fromUid);
        syncEngine(toUid);
        return error.what();
    }

    Connection link;
    link.uid = nextUid_++;
    link.from = fromUid;
    link.to = toUid;
    links_.push_back(link);
    from->kept = to->kept = true;
    markDirty();
    return {};
}

void Controller::unlink(int linkUid) {
    auto found = std::find_if(links_.begin(), links_.end(), [&](const Connection& link) { return link.uid == linkUid; });
    if (found == links_.end()) return;
    const Connection link = *found;
    graph_.removeLink(sourceId(*node(link.from)), node(link.to)->id);
    links_.erase(found);
    syncEngine(link.from);
    syncEngine(link.to);
    markDirty();
}

void Controller::setGainDb(int linkUid, float gainDb) {
    gainDb = std::max(-60.0f, std::min(20.0f, gainDb));
    for (Connection& link : links_) {
        if (link.uid != linkUid || link.gainDb == gainDb) continue;
        link.gainDb = gainDb;
        graph_.setLinkGain(sourceId(*node(link.from)), node(link.to)->id,
                           link.suspended ? 0.0f : std::pow(10.0f, gainDb / 20.0f));
        markDirty();
    }
}

// ---------------------------------------------------------------------------
// Connections that Windows makes
// ---------------------------------------------------------------------------

// Hands a request to Windows. Afterwards it is marked as sent, or left as it
// was when that is not possible yet (the application has no audio open), or
// its application is set to 0 when it never will be.
void Controller::send(Move& move) {
    const Node* app = node(move.app);
    if (!app) return;
    if (move.park) {
        const Node* silent = node(silentDevice());
        if (!silent) return;
        move.device = silent->target;
    }
    const int told = mover_ ? mover_(app->target, move.recording, move.device) : -1;
    if (told < 0) {
        move.app = 0;  // given up
        notice("This version of Windows cannot move an application from one device to another.");
    } else if (told > 0) {
        move.sent = true;
        move.since = now_;
    }
}

void Controller::requestMove(Node& app, bool recording, const std::string& device, bool park, bool quiet) {
    const int uid = app.uid;
    moves_.erase(std::remove_if(moves_.begin(), moves_.end(),
                                [&](const Move& move) { return move.app == uid && move.recording == recording; }),
                 moves_.end());
    Move move;
    move.app = uid;
    move.recording = recording;
    move.device = device;
    move.park = park;
    move.quiet = quiet;
    move.since = now_;
    send(move);
    if (move.app == 0) {
        if (park && app.parked) app.parked = false;
        return;
    }
    // Handing back to the default needs no watching; anything else is
    // followed until Windows is seen to have done it.
    const bool givingBack = device.empty() && !park;
    if (!(givingBack && move.sent)) moves_.push_back(move);
    hurry();
}

// Requests not yet handed over go out once the application has audio open;
// the others are checked against what Windows now shows.
void Controller::followMoves(const Discovery& discovery) {
    for (size_t i = 0; i < moves_.size();) {
        Move& move = moves_[i];
        const Node* app = node(move.app);
        bool done = !app;
        if (app) {
            bool hasStream = false, arrived = false;
            std::string elsewhere;
            for (const Discovery::Session& session : discovery.sessions) {
                if (session.recording != move.recording || toLower(session.app) != toLower(app->target)) continue;
                hasStream = true;
                if (!session.active) continue;
                if (session.device == move.device) arrived = true;
                else elsewhere = session.device;
            }
            const bool givingBack = move.device.empty() && !move.park;
            if (!move.sent) {
                if (givingBack ? app->present : hasStream) {
                    send(move);
                    done = move.app == 0 || givingBack;
                }
            } else if (arrived) {
                // It plays there by itself now: a copy Wolfe was carrying to
                // the same device has become a second helping.
                for (const Node& device : nodes_) {
                    if (move.recording || device.kind != NodeKind::Output || device.target != move.device) continue;
                    for (const Connection& copy : links_) {
                        if (copy.from != move.app || copy.to != device.uid) continue;
                        unlink(copy.uid);
                        break;
                    }
                }
                done = true;
            } else if (now_ - move.since > 4.0) {
                if (!elsewhere.empty() && !move.quiet) {
                    std::string where = elsewhere;
                    for (const Node& device : nodes_) {
                        if (device.kind == (move.recording ? NodeKind::Input : NodeKind::Output) && device.target == elsewhere) {
                            where = device.label;
                        }
                    }
                    notice("\"" + app->label + "\" is still " + (move.recording ? "recording from" : "playing on") + " \"" +
                           where + "\". It will change the next time it opens its audio (restarting it does that), unless it " +
                           "chooses its device in its own settings.");
                }
                done = true;
            }
        }
        if (done) moves_.erase(moves_.begin() + (long long)i);
        else i++;
    }
}

// The application's own link was removed. If Wolfe is carrying a plain copy
// of it to some device, that device becomes its own: the same thing is heard,
// with no delay and without Wolfe. Otherwise it is heard on no device at all.
std::string Controller::leaveDevice(Node& app) {
    for (const Connection& copy : links_) {
        const Node* device = node(copy.to);
        if (copy.from != app.uid || copy.gainDb != 0.0f || !device || device->kind != NodeKind::Output) continue;
        if (!device->present || !device->resolved || device->uid == silentDevice()) continue;
        if (app.parked) {
            app.parked = false;
            markDirty();
        }
        requestMove(app, false, device->target);  // the copy goes once Windows has done it
        return {};
    }
    return park(app);
}

// Takes an application off every playback device: Windows has to play it
// somewhere, so it goes to the device nobody listens to.
std::string Controller::park(Node& app) {
    const Node* silent = node(silentDevice());
    if (!silent || !silent->present) {
        return "Windows always plays an application on some device. To take \"" + app.label +
               "\" off all of them Wolfe needs a playback device that nobody listens to: choose one in the Wolfe menu, " +
               "under Silent device.";
    }
    if (!app.parked) {
        app.parked = true;
        app.kept = true;
        markDirty();
    }
    requestMove(app, false, {}, true);
    return {};
}

// Something is dragged onto an application that records.
std::string Controller::connectToApp(Node& from, Node& app) {
    std::vector<const Node*> sources;  // what it records from now
    for (const SystemLink& link : systemLinks_) {
        if (link.kind == SystemLink::Records && link.to == app.uid) sources.push_back(node(link.from));
    }

    // A recording device: the application is told to record from it. What is
    // played into one end of a cable is what its other end records.
    const Node* device = from.kind == NodeKind::Input ? &from : nullptr;
    if (from.kind == NodeKind::Output) device = partner(from);
    if (device) {
        for (const Node* source : sources) {
            if (source == device) return "\"" + app.label + "\" is already recording from \"" + device->label + "\".";
        }
        if (!device->present || !device->resolved) return "\"" + device->label + "\" is not plugged in.";
        requestMove(app, true, device->target);
        return {};
    }

    // Anything else reaches it through a virtual cable: Wolfe plays into one
    // end, the application records from the other. The one it is on already:
    for (const Node* source : sources) {
        if (const Node* nearEnd = source ? partner(*source) : nullptr) return link(from.uid, nearEnd->uid);
    }
    // ...or one that nothing else is using.
    for (const SystemLink& cable : systemLinks_) {
        if (cable.kind != SystemLink::Cable) continue;
        const Node* nearEnd = node(cable.from);
        const Node* farEnd = node(cable.to);
        if (!nearEnd || !farEnd || !nearEnd->present || !farEnd->present) continue;
        bool busy = nearEnd->uid == silentDevice() || belongsToSteam(nearEnd->label);
        for (const Connection& link : links_) busy |= link.to == nearEnd->uid;
        for (const SystemLink& link : systemLinks_) {
            busy |= link.kind == SystemLink::Plays && link.to == nearEnd->uid;
            busy |= link.kind == SystemLink::Records && link.from == farEnd->uid;
        }
        if (busy) continue;
        const std::string farId = farEnd->target;
        const std::string problem = link(from.uid, nearEnd->uid);
        if (!problem.empty()) return problem;
        requestMove(app, true, farId);
        return {};
    }
    return "There is no free virtual cable to carry that into \"" + app.label +
           "\": each different mix needs a cable of its own. To give it what already goes into a cable, drag that cable's " +
           "recording end onto it.";
}

std::string Controller::connect(int fromUid, int toUid, int replacedLinkUid) {
    Node* from = find(fromUid);
    Node* to = find(toUid);
    if (!from || !to) return "that node is not there any more";

    // A link pulled off one pin and dropped on another.
    for (const Connection& old : links_) {
        if (old.uid != replacedLinkUid) continue;
        if (old.from == fromUid && old.to == toUid) return {};  // put back where it was
        const std::string problem = connect(fromUid, toUid);
        if (problem.empty()) unlink(replacedLinkUid);
        return problem;
    }
    if (const SystemLink* found = systemLink(replacedLinkUid)) {
        const SystemLink old = *found;
        if (old.from == fromUid && old.to == toUid) return {};
        if (old.kind == SystemLink::Plays) {
            // The same application, onto another playback device: Windows moves it.
            if (old.from == fromUid && to->kind == NodeKind::Output) {
                if (toUid == silentDevice()) return park(*from);
                if (!to->present || !to->resolved) return "\"" + to->label + "\" is not plugged in.";
                if (from->parked) {
                    from->parked = false;
                    markDirty();
                }
                requestMove(*from, false, to->target);
                return {};
            }
            // Anything else: the new connection, and the application leaves that device.
            const std::string problem = connect(fromUid, toUid);
            if (!problem.empty()) return problem;
            Node* app = find(old.from);
            return app ? leaveDevice(*app) : std::string();
        }
        if (old.kind == SystemLink::Listens) {
            const Node* oldInput = node(old.from);
            // Still a recording device onto a playback device: Windows goes on
            // carrying it, to the other device or from the other one.
            if (from->kind == NodeKind::Input && to->kind == NodeKind::Output) {
                if (fromUid != old.from && oldInput) {
                    const std::string problem = listen(*oldInput, nullptr);
                    if (!problem.empty()) return problem;
                }
                return listen(*from, to);
            }
            // Anything else: the new connection, and Windows stops.
            const std::string problem = connect(fromUid, toUid);
            if (!problem.empty()) return problem;
            return oldInput ? listen(*oldInput, nullptr) : std::string();
        }
        // What an application records from, or a cable: there is nothing to
        // take away, only the new connection to make.
    }

    if (from == to) return "a node cannot be connected to itself";
    if (to->kind == NodeKind::App) return connectToApp(*from, *to);
    for (const SystemLink& made : systemLinks_) {
        if (made.kind == SystemLink::Listens && made.from == fromUid && made.to == toUid) {
            return "Windows is already playing \"" + from->label + "\" on \"" + to->label + "\".";
        }
    }

    if (from->kind == NodeKind::App && to->kind == NodeKind::Output) {
        bool carried = false;
        for (const Connection& link : links_) carried |= link.from == fromUid && link.to == toUid;
        for (const SystemLink& link : systemLinks_) {
            if (link.kind == SystemLink::Plays && link.from == fromUid && link.to == toUid) {
                return "\"" + from->label + "\" is already playing on \"" + to->label + "\".";
            }
        }
        // An application that is heard nowhere gets a device of its own
        // again, with no delay. One that already plays somewhere stays there,
        // and what goes to the new device is a copy carried by Wolfe.
        if ((from->parked || from->silenced) && !carried && to->present && to->resolved && toUid != silentDevice()) {
            if (from->parked) {
                from->parked = false;
                markDirty();
            }
            requestMove(*from, false, to->target);
            return {};
        }
    }
    return link(fromUid, toUid);
}

std::string Controller::disconnect(int linkUid) {
    for (const Connection& link : links_) {
        if (link.uid != linkUid) continue;
        unlink(linkUid);
        return {};
    }
    const SystemLink* found = systemLink(linkUid);
    if (!found) return {};
    const SystemLink link = *found;
    Node* from = find(link.from);
    Node* to = find(link.to);
    if (!from || !to) return {};
    switch (link.kind) {
        case SystemLink::Plays: return leaveDevice(*from);
        case SystemLink::Listens: return listen(*from, nullptr);
        case SystemLink::Records:
            return "An application always records from some device. Drag this link to another recording device, or drag " +
                   std::string("something else onto \"") + to->label + "\".";
        default:
            return "\"" + from->label + "\" and \"" + to->label + "\" are the two ends of one virtual cable: that cannot be undone.";
    }
}

// Has Windows play a recording device on a playback device, or stop (no device).
std::string Controller::listen(const Node& input, const Node* output) {
    if (output && (!output->present || !output->resolved)) return "\"" + output->label + "\" is not plugged in.";
    if (!input.present || !input.resolved) return "\"" + input.label + "\" is not plugged in.";
    if (!listener_ || !listener_(input.target, output != nullptr, output ? output->target : std::string())) {
        return std::string("Windows did not ") + (output ? "start" : "stop") + " playing \"" + input.label +
               "\" by itself (\"Listen to this device\", in the device's properties).";
    }
    hurry();
    return {};
}

bool Controller::canHandOver(int linkUid) const {
    if (const SystemLink* made = systemLink(linkUid)) return made->kind == SystemLink::Listens;
    for (const Connection& link : links_) {
        if (link.uid != linkUid) continue;
        const Node* from = node(link.from);
        const Node* to = node(link.to);
        return from && to && from->kind == NodeKind::Input && to->kind == NodeKind::Output;
    }
    return false;
}

std::string Controller::handOver(int linkUid) {
    // From Windows to Wolfe.
    if (const SystemLink* found = systemLink(linkUid)) {
        const SystemLink made = *found;
        const Node* input = node(made.from);
        if (made.kind != SystemLink::Listens || !input) return {};
        const std::string problem = link(made.from, made.to);
        if (!problem.empty()) return problem;
        const std::string refused = listen(*input, nullptr);
        if (!refused.empty()) unlink(links_.back().uid);
        return refused;
    }
    // From Wolfe to Windows.
    for (const Connection& carried : links_) {
        if (carried.uid != linkUid) continue;
        const Node* from = node(carried.from);
        const Node* to = node(carried.to);
        if (!canHandOver(linkUid)) return "Windows can only do this between a recording device and a playback device.";
        for (const SystemLink& made : systemLinks_) {
            if (made.kind != SystemLink::Listens || made.from != carried.from) continue;
            return "Windows plays a recording device on one playback device only, and \"" + from->label +
                   "\" is already on \"" + node(made.to)->label + "\".";
        }
        const std::string refused = listen(*from, to);
        if (refused.empty()) unlink(linkUid);
        return refused;
    }
    return {};
}

std::string Controller::disconnectAll(int nodeUid) {
    std::vector<int> touching;
    for (const Connection& link : links_) {
        if (link.from == nodeUid || link.to == nodeUid) touching.push_back(link.uid);
    }
    for (int linkUid : touching) unlink(linkUid);

    // An application also leaves the device Windows plays it on.
    Node* app = find(nodeUid);
    if (!app || app->kind != NodeKind::App) return {};
    for (const SystemLink& link : systemLinks_) {
        if (link.kind == SystemLink::Plays && link.from == nodeUid) return park(*app);
    }
    return {};
}

void Controller::setSilentDevice(int outputUid) {
    const Node* chosen = node(outputUid);
    const std::string id = chosen && chosen->kind == NodeKind::Output && chosen->resolved ? chosen->target : std::string();
    silentChosen_ = true;
    markDirty();
    if (id == silentDevice_) return;
    silentDevice_ = id;
    // Applications kept off every device move to the new one; with none left
    // they go back to where Windows would play them.
    for (Node& app : nodes_) {
        if (app.kind != NodeKind::App || !app.parked) continue;
        if (id.empty()) {
            app.parked = false;
            requestMove(app, false, {});
        } else {
            requestMove(app, false, {}, true);
        }
    }
    hurry();
}

void Controller::giveBack(int appUid) {
    Node* app = find(appUid);
    if (!app || app->kind != NodeKind::App) return;
    if (app->parked) {
        app->parked = false;
        markDirty();
    }
    requestMove(*app, false, {});
    requestMove(*app, true, {});
}

void Controller::giveBackParked() {
    if (!mover_) return;
    for (const Node& node : nodes_) {
        if (node.kind == NodeKind::App && node.parked) mover_(node.target, false, {});
    }
}

// ---------------------------------------------------------------------------
// Boxes
// ---------------------------------------------------------------------------

int Controller::addEffect(const std::string& type, float x, float y) {
    if (!createEffect(type)) return 0;
    Node node;
    do {
        node.id = "fx|" + std::to_string(nextEffect_++);
    } while (findById(node.id));
    node.kind = NodeKind::Effect;
    node.type = type;
    node.label = effectLabel(type);
    node.present = true;
    node.kept = true;
    node.hasPosition = true;
    node.x = x;
    node.y = y;
    markDirty();
    return insertNode(node);
}

void Controller::removeNode(int nodeUid) {
    const Node* target = node(nodeUid);
    if (!target || !removable(*target)) return;
    disconnectAll(nodeUid);
    if (target->parked) giveBack(nodeUid);
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.uid == nodeUid; }),
                 nodes_.end());
    systemLinks_.erase(std::remove_if(systemLinks_.begin(), systemLinks_.end(),
                                      [&](const SystemLink& link) { return link.from == nodeUid || link.to == nodeUid; }),
                       systemLinks_.end());
    markDirty();
    publishWatched();
}

void Controller::moveNode(int nodeUid, float x, float y) {
    Node* node = find(nodeUid);
    if (!node) return;
    if (node->hasPosition && std::fabs(node->x - x) < 0.5f && std::fabs(node->y - y) < 0.5f) return;
    node->x = x;
    node->y = y;
    node->hasPosition = true;
    node->kept = true;
    markDirty();
}

// Under the lowest box of its column.
void Controller::place(Node& node) {
    const float x = columnOf(node);
    float y = 40.0f;
    for (const Node& other : nodes_) {
        if (&other == &node || !other.hasPosition || !visible(other)) continue;
        if (std::fabs(other.x - x) < kBoxWidth) y = std::max(y, other.y + 98.0f);
    }
    node.x = x;
    node.y = y;
    node.hasPosition = true;
}

// Gives a place to every box that has just come onto the canvas: one that has
// none yet, and one whose remembered place was taken by another box while it
// was away. Two boxes on the same spot look like one, and a drag from "its"
// pin acts on whichever of the two the canvas happens to pick.
void Controller::settle() {
    for (Node& node : nodes_) {
        const bool onCanvas = visible(node);
        if (onCanvas && !node.shown) {
            bool covered = false;
            for (const Node& other : nodes_) {
                if (&other == &node || !other.shown || !other.hasPosition || !visible(other)) continue;
                covered |= std::fabs(other.x - node.x) < kBoxWidth && std::fabs(other.y - node.y) < kBoxHeight;
            }
            if (!node.hasPosition || covered) {
                place(node);
                if (node.kept) markDirty();
            }
        }
        node.shown = onCanvas;
    }
}

// ---------------------------------------------------------------------------
// Keeping up with the system and the engine
// ---------------------------------------------------------------------------

void Controller::syncDevices(const std::vector<DeviceInfo>& devices, NodeKind kind) {
    for (Node& node : nodes_) {
        if (node.kind == kind) node.present = false;
    }
    for (const DeviceInfo& device : devices) {
        const std::string id = toUtf8(device.id);
        const std::string name = toLower(device.name);

        Node* found = nullptr;
        for (Node& node : nodes_) {
            if (node.kind == kind && node.resolved && node.target == id) found = &node;
        }
        // A node from a hand-written file names its device by part of its name;
        // from here on it is tied to the device itself.
        for (Node& node : nodes_) {
            if (found) break;
            if (node.kind != kind || node.resolved) continue;
            const std::string fragment = toLower(node.target);
            const bool matches = fragment == "default" ? device.isDefault : name.find(fragment) != std::string::npos;
            if (!matches) continue;
            node.target = id;
            node.resolved = true;
            markDirty();
            found = &node;
        }
        if (!found) {
            Node node;
            node.id = (kind == NodeKind::Input ? "in|" : "out|") + id;
            node.kind = kind;
            node.type = kind == NodeKind::Input ? "input" : "output";
            node.target = id;
            node.resolved = true;
            found = find(insertNode(node));
        }
        found->present = true;
        found->label = device.name;
    }
}

void Controller::apply(const Discovery& discovery) {
    syncDevices(discovery.inputs, NodeKind::Input);
    syncDevices(discovery.outputs, NodeKind::Output);

    // The silent device: the one named in the file, or failing any choice by
    // the user, one that is known to be listened to by nobody.
    for (const DeviceInfo& device : discovery.outputs) {
        const bool named = !silentDevice_.empty() && !startsWith(silentDevice_, "{") &&
                           toLower(device.name).find(toLower(silentDevice_)) != std::string::npos;
        const bool suitable = silentDevice_.empty() && !silentChosen_ && belongsToSteam(device.name);
        if (named || suitable) silentDevice_ = toUtf8(device.id);
    }

    // A monitor exists while the output it listens to does.
    for (Node& node : nodes_) {
        if (node.kind != NodeKind::Loopback) continue;
        node.present = false;
        for (const DeviceInfo& device : discovery.outputs) {
            const bool same = node.resolved ? toUtf8(device.id) == node.target
                                            : toLower(device.name).find(toLower(node.target)) != std::string::npos;
            if (same) node.present = true;
        }
    }

    for (Node& node : nodes_) {
        if (node.kind != NodeKind::App) continue;
        auto found = discovery.running.find(node.target);
        if (found != discovery.running.end()) node.present = found->second;
    }
    for (const std::string& path : discovery.apps) {
        const std::string wanted = toLower(path);
        bool known = false;
        for (const Node& node : nodes_) {
            if (node.kind == NodeKind::App && toLower(node.target) == wanted) known = true;
        }
        if (known) continue;
        Node node;
        node.id = "app|" + path;
        node.kind = NodeKind::App;
        node.type = "app";
        node.target = path;
        node.label = fileName(path);
        node.detail = folderHint(path);
        node.present = true;
        insertNode(node);
    }
    // Applications that came and went without the user touching them are forgotten.
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                [&](const Node& node) {
                                    return node.kind == NodeKind::App && !node.present && !node.kept && !linked(node.uid);
                                }),
                 nodes_.end());

    syncSystemLinks(discovery);
    followMoves(discovery);

    // A link of Wolfe's between two boxes that Windows connects by itself
    // (an application and the device it plays on, a recording device and the
    // one Windows plays it on) would make that be heard twice. It stays on
    // the canvas and in the file, carrying silence for as long as that lasts.
    for (Connection& link : links_) {
        bool doubled = false;
        for (const SystemLink& made : systemLinks_) {
            const bool sameThing = made.kind == SystemLink::Plays || made.kind == SystemLink::Listens;
            doubled |= sameThing && made.from == link.from && made.to == link.to;
        }
        if (doubled == link.suspended) continue;
        link.suspended = doubled;
        graph_.setLinkGain(sourceId(*node(link.from)), node(link.to)->id,
                           doubled ? 0.0f : std::pow(10.0f, link.gainDb / 20.0f));
    }
    settle();
    publishWatched();

    bool waiting = false;
    for (const Move& move : moves_) waiting |= !move.sent;
    std::lock_guard<std::mutex> lock(watchMutex_);
    eager_ = waiting;
}

// What Windows has connected: which application plays on or records from
// which device, and which devices are the two ends of a virtual cable.
void Controller::syncSystemLinks(const Discovery& discovery) {
    std::vector<SystemLink> fresh;
    auto add = [&](int from, int to, SystemLink::Kind kind, bool active) {
        for (SystemLink& link : fresh) {
            if (link.from == from && link.to == to) {
                link.active |= active;  // several streams between the same two
                return;
            }
        }
        SystemLink link;
        link.from = from;
        link.to = to;
        link.kind = kind;
        link.active = active;
        // The same connection keeps its number, so that the canvas sees one
        // steady link and not a new one every time the system is looked at.
        for (const SystemLink& old : systemLinks_) {
            if (old.from == from && old.to == to) link.uid = old.uid;
        }
        if (!link.uid) link.uid = nextUid_++;
        fresh.push_back(link);
    };

    for (Node& app : nodes_) {
        if (app.kind != NodeKind::App) continue;
        const std::string wanted = toLower(app.target);
        for (bool recording : {false, true}) {
            std::vector<const Discovery::Session*> streams;
            bool flowing = false;
            for (const Discovery::Session& session : discovery.sessions) {
                if (session.recording != recording || toLower(session.app) != wanted) continue;
                streams.push_back(&session);
                flowing |= session.active;
            }
            const Discovery::Choice* choice = choiceOf(recording ? discovery.recordsFrom : discovery.playsOn, app.target);
            (recording ? app.ownInput : app.ownOutput) = choice && choice->own;
            bool onChosen = false;  // it has a stream on the device Windows sends it to
            for (const Discovery::Session* stream : streams) onChosen |= choice && stream->device == choice->device;

            if (!recording) {
                // Heard nowhere: Windows sends it to the silent device or,
                // when Windows does not say, that is the only place it plays.
                app.silenced = false;
                if (!silentDevice_.empty() && choice) {
                    app.silenced = choice->device == silentDevice_;
                } else if (!silentDevice_.empty() && !streams.empty()) {
                    app.silenced = true;
                    for (const Discovery::Session* stream : streams) app.silenced &= stream->device == silentDevice_;
                }
            }

            // Windows keeps listing a stream long after it has stopped, also
            // on a device the application has since moved away from. So:
            // while anything flows, only that is shown; otherwise where the
            // application would play (or record) next. That is the device it
            // was given, also before it has opened anything there; with no
            // device of its own, the streams it has are all there is to go by.
            std::vector<std::pair<std::string, bool>> connected;  // device id, flowing
            if (!flowing && choice && (onChosen || choice->own)) {
                connected.push_back({choice->device, false});
            } else {
                for (const Discovery::Session* stream : streams) {
                    if (stream->active == flowing) connected.push_back({stream->device, stream->active});
                }
            }
            for (const auto& [deviceId, active] : connected) {
                // Playing on the silent device is not being connected to anything.
                if (!recording && !silentDevice_.empty() && deviceId == silentDevice_) continue;
                const Node* device = nullptr;
                for (const Node& node : nodes_) {
                    const bool rightKind = node.kind == (recording ? NodeKind::Input : NodeKind::Output);
                    if (rightKind && node.resolved && node.target == deviceId) device = &node;
                }
                if (!device) continue;
                if (recording) add(device->uid, app.uid, SystemLink::Records, active);
                else add(app.uid, device->uid, SystemLink::Plays, active);
            }
        }
    }

    for (const Discovery::Listen& listening : discovery.listens) {
        const Node* input = nullptr;
        const Node* output = nullptr;
        for (const Node& node : nodes_) {
            if (!node.resolved || !node.present) continue;
            if (node.kind == NodeKind::Input && node.target == listening.input) input = &node;
            if (node.kind == NodeKind::Output && node.target == listening.output) output = &node;
        }
        if (input && output) add(input->uid, output->uid, SystemLink::Listens, true);
    }

    for (const Node& output : nodes_) {
        if (output.kind != NodeKind::Output || !output.present) continue;
        const std::string adapter = adapterOf(output.label);
        if (!looksLikeVirtualCable(adapter)) continue;
        for (const Node& input : nodes_) {
            if (input.kind == NodeKind::Input && input.present && adapterOf(input.label) == adapter) {
                add(output.uid, input.uid, SystemLink::Cable, true);
            }
        }
    }
    systemLinks_ = std::move(fresh);
}

void Controller::publishWatched() {
    std::vector<std::string> targets;
    for (const Node& node : nodes_) {
        if (node.kind == NodeKind::App) targets.push_back(node.target);
    }
    std::lock_guard<std::mutex> lock(watchMutex_);
    watched_ = std::move(targets);
}

void Controller::update(double now) {
    const double elapsed = now_ > 0 ? now - now_ : 0.0;
    now_ = now;

    Discovery discovery;
    std::map<std::string, float> devicePeaks, appPeaks;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        if (hasPending_) {
            discovery = std::move(pending_);
            hasPending_ = false;
            fresh = true;
        }
        devicePeaks.swap(devicePeaks_);
        appPeaks.swap(appPeaks_);
    }
    if (fresh) apply(discovery);

    // Levels are the ones Windows measures, so that a box shows what is really
    // passing through it whether or not Wolfe is involved. They fall back by
    // themselves in about a third of a second.
    const float fall = (float)std::exp(-elapsed / 0.3);
    for (Node& node : nodes_) {
        float peak = 0.0f;
        if (node.kind == NodeKind::App) {
            auto found = appPeaks.find(toLower(node.target));
            if (found != appPeaks.end()) peak = found->second;
        } else if (node.kind != NodeKind::Effect) {
            auto found = devicePeaks.find(node.target);  // a monitor has the id of the output it listens to
            if (found != devicePeaks.end()) peak = found->second;
        }
        if (node.used()) {
            // A playback device can be in the engine twice: as the device
            // Wolfe plays on, and as the monitor Wolfe listens to it through.
            const std::string main = node.active ? graph_.nodeStatus(node.id) : std::string();
            const std::string monitor = node.monitoring ? graph_.nodeStatus(sourceId(node)) : std::string();
            const bool mainFine = !node.active || engineIsFine(main);
            const bool monitorFine = !node.monitoring || engineIsFine(monitor);
            node.healthy = mainFine && monitorFine;
            node.status = !mainFine ? main : !monitorFine ? monitor : node.active ? main : monitor;
            if (node.active) peak = std::max(peak, graph_.takeNodePeak(node.id));  // what the engine itself sees
        }
        node.level = std::max(std::min(peak, 1.0f), node.level * fall);
    }

    if (dirty_ && now - dirtySince_ > 1.0) save();
}

void Controller::watchMain() {
    ComApartment com;
    AppDevices appDevices;  // to read which device each application was given

    // The level meters found at the last look; they are read many times a second in between.
    struct Meter {
        bool ofDevice;    // otherwise: of one stream of an application
        std::string key;  // the device's id, or the application as the canvas knows it (lower case)
        ComPtr<IAudioMeterInformation> meter;
    };
    std::vector<Meter> meters;
    double nextLook = 0.0;

    std::unique_lock<std::mutex> lock(watchMutex_);
    while (!stopWatching_) {
        if (lookNow_ || nowSeconds() >= nextLook) {
            const bool metering = lookOften_;
            std::vector<std::string> watched = watched_;
            lookNow_ = false;
            lock.unlock();

            Discovery discovery;
            meters.clear();
            try {
                discovery.inputs = listDevices(eCapture);
                discovery.outputs = listDevices(eRender);
                std::string defaultInput, defaultOutput;
                for (const DeviceInfo& device : discovery.inputs) {
                    if (device.isDefault) defaultInput = toUtf8(device.id);
                }
                for (const DeviceInfo& device : discovery.outputs) {
                    if (device.isDefault) defaultOutput = toUtf8(device.id);
                }

                // Which known application each process belongs to.
                ProcessSnapshot snapshot;
                std::map<DWORD, std::string> owner;
                for (const std::string& target : watched) {
                    std::map<DWORD, int> family = snapshot.family(target);
                    discovery.running[target] = !family.empty();
                    for (const auto& [pid, depth] : family) owner.emplace(pid, target);
                }
                // Every stream open on every device, by application. Wolfe's own
                // are left out: its links on the canvas already say the same.
                std::set<std::string> seen;
                for (const AudioSession& session : audioSessions()) {
                    if (session.pid == GetCurrentProcessId()) continue;
                    std::string app;
                    auto known = owner.find(session.pid);
                    if (known != owner.end()) {
                        app = known->second;
                    } else {
                        app = snapshot.path(session.pid);
                        // No path: a system process, like the one behind "Listen to this device".
                        if (app.empty()) continue;
                        if (seen.insert(toLower(app)).second) discovery.apps.push_back(app);
                    }
                    discovery.sessions.push_back({app, toUtf8(session.device), session.recording, session.active});
                    // An application's level is that of what it plays.
                    if (metering && !session.recording && session.meter) meters.push_back({false, toLower(app), session.meter});

                    // Where Windows sends it: to a device of its own, if any
                    // of its processes was given one, else to the default.
                    Discovery::Choice& choice = (session.recording ? discovery.recordsFrom : discovery.playsOn)[app];
                    if (!choice.own) {
                        const std::wstring own = appDevices.get(session.pid, session.recording ? eCapture : eRender);
                        if (!own.empty()) choice = {toUtf8(own), true};
                        else choice.device = session.recording ? defaultInput : defaultOutput;
                    }
                }
                // "Listen to this device", towards the default device when none is named.
                for (const Listening& listening : listListening()) {
                    discovery.listens.push_back({toUtf8(listening.input),
                                                 listening.output.empty() ? defaultOutput : toUtf8(listening.output)});
                }
                if (metering) {
                    for (const DeviceMeter& device : deviceMeters()) meters.push_back({true, toUtf8(device.id), device.meter});
                }
            } catch (const std::exception& error) {
                logLine("looking at the system failed: %s", error.what());
            }

            lock.lock();
            pending_ = std::move(discovery);
            hasPending_ = true;
            nextLook = nowSeconds() + (nowSeconds() < hurryUntil_ ? 0.3 : lookOften_ ? 1.5 : eager_ ? 2.0 : 6.0);
        }

        const auto wake = [this] { return stopWatching_ || lookNow_; };
        if (lookOften_) {
            // Somebody is looking at the canvas: read the meters twenty times a second.
            lock.unlock();
            std::vector<float> values(meters.size(), 0.0f);
            for (size_t i = 0; i < meters.size(); i++) meters[i].meter->GetPeakValue(&values[i]);
            lock.lock();
            for (size_t i = 0; i < meters.size(); i++) {
                float& highest = (meters[i].ofDevice ? devicePeaks_ : appPeaks_)[meters[i].key];
                highest = std::max(highest, values[i]);
            }
            watchWake_.wait_for(lock, std::chrono::milliseconds(50), wake);
        } else {
            meters.clear();
            devicePeaks_.clear();
            appPeaks_.clear();
            const double pause = std::max(0.05, nextLook - nowSeconds());
            watchWake_.wait_for(lock, std::chrono::duration<double>(pause), wake);
        }
    }
}

// ---------------------------------------------------------------------------
// Words for tooltips
// ---------------------------------------------------------------------------

std::string Controller::describeNode(int nodeUid) {
    const Node* found = node(nodeUid);
    if (!found) return {};
    const Node& node = *found;
    std::string text;
    char line[300];

    switch (node.kind) {
        case NodeKind::Input: text = "Recording device\n"; break;
        case NodeKind::Output: text = "Playback device\n"; break;
        case NodeKind::Loopback: text = "Everything that is being played on that device\n"; break;
        case NodeKind::App: text = "Application\n" + node.target + "\n"; break;
        case NodeKind::Effect:
            text = node.type == "rnnoise" ? "Removes background noise from a voice. Its output is mono.\n"
                                           : "Adds together whatever is connected to it.\n";
            break;
    }
    if (!node.present) text += node.kind == NodeKind::App ? "Not running.\n" : "Not plugged in.\n";

    if (node.kind == NodeKind::Output && node.uid == silentDevice()) {
        text += "This is the silent device: an application taken off every other device plays here, where nobody listens.\n";
    }
    if (node.kind == NodeKind::App) {
        const Node* silent = this->node(silentDevice());
        if ((node.parked || node.silenced) && silent) {
            text += "Taken off every playback device: Windows plays it on " + silent->label +
                    ", where nobody listens.\nDrag from its pin to a device to hear it there again.\n";
        } else if (node.ownOutput) {
            text += "Windows was told which device to play it on (right-click to undo).\n";
        }
        if (node.ownInput) text += "Windows was told which device it records from (right-click to undo).\n";
    }

    if (!node.used()) {
        text += node.kind == NodeKind::App ? "Wolfe is not carrying its audio anywhere."
                                           : "Wolfe is not using it: nothing is linked to it here.";
        return text;
    }
    if (node.kind != NodeKind::Effect) text += node.status + "\n";
    if (node.monitoring) text += "Wolfe is listening to what is played on it.\n";

    if (node.kind == NodeKind::Output && node.active) {
        Graph::OutputReport report = graph_.outputReport(node.id);
        snprintf(line, sizeof(line), "Keeps %.0f ms of audio queued. Ran out of audio %llu times.\n",
                 report.output.queuedMs, (unsigned long long)report.output.underruns);
        text += line;
        for (const Graph::OutputReport::Source& source : report.sources) {
            const Node* from = findById(startsWith(source.id, "mon|") ? source.id.substr(4) : source.id);
            const std::string name = from ? from->label : source.id;
            if (source.stats.active) {
                snprintf(line, sizeof(line), "%s: %.0f ms behind, clock %+.0f ppm, %llu frames lost\n", name.c_str(),
                         source.stats.targetMs, source.stats.ratioPpm, (unsigned long long)source.stats.lateFrames);
            } else {
                snprintf(line, sizeof(line), "%s: silent\n", name.c_str());
            }
            text += line;
        }
    }
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

std::string Controller::describeLink(int linkUid) const {
    for (const Connection& link : links_) {
        if (link.uid != linkUid) continue;
        const Node* from = node(link.from);
        char line[400];
        snprintf(line, sizeof(line), "%s%s  ->  %s\nCarried by Wolfe.  Gain %+.1f dB\nRight-click to change it; drag the link to move the end you took.",
                 from->kind == NodeKind::Output ? "What is played on " : "", from->label.c_str(), node(link.to)->label.c_str(),
                 link.gainDb);
        if (link.suspended) {
            return std::string(line) + "\nPaused: Windows is already playing it on that device, and this would be heard as a second copy.";
        }
        return line;
    }
    const SystemLink* found = systemLink(linkUid);
    if (!found) return {};
    const Node* from = node(found->from);
    const Node* to = node(found->to);
    if (!from || !to) return {};
    const char* state = found->active ? "" : " (nothing is flowing right now)";
    if (found->kind == SystemLink::Plays) {
        return from->label + " is playing on " + to->label + state + ".\nWindows does this itself, with no delay." +
               "\nDrag this link to another device to move it there; remove it to take it off this one." +
               "\nAnother link from its pin sends a copy somewhere else as well.";
    }
    if (found->kind == SystemLink::Records) {
        return to->label + " is recording from " + from->label + state + "." +
               "\nDrag another recording device onto it to change that, or anything else to feed it through a virtual cable.";
    }
    if (found->kind == SystemLink::Listens) {
        return "Windows is playing what " + from->label + " records on " + to->label +
               ".\nThis is \"Listen to this device\", in the properties of the recording device:" +
               "\nit goes on with or without Wolfe, with a delay of its own and no gain." +
               "\nDrag this link to another playback device to move it; remove it to switch it off.";
    }
    return "Whatever is played on " + from->label + "\ncomes out of " + to->label +
           ":\nthey are the two ends of a virtual cable.";
}

}  // namespace wolfe
