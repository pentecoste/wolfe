#include "controller.h"

#include "process_finder.h"
#include "virtual_devices.h"

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
    if (type == "output") return NodeKind::Output;
    if (type == "loopback") return NodeKind::Loopback;
    if (type == "virtual_speaker") return NodeKind::WolfeSpeaker;
    if (type == "virtual_mic") return NodeKind::WolfeMic;
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

// Roughly the size of a box on the canvas, in the units positions are kept in.
const float kBoxWidth = 190.0f, kBoxHeight = 60.0f;

bool engineIsFine(const std::string& status) {
    return startsWith(status, "capturing") || startsWith(status, "playing") || startsWith(status, "running") ||
           startsWith(status, "ready") || startsWith(status, "being added");
}

const char* const kWhereWindowsDecides =
    "Windows decides where each application plays and records from: Settings > System > Sound > Volume mixer, "
    "or the application's own settings.";

// The editor's names for application boxes: one for what it plays, one for
// what it records.
std::string appBoxId(const std::string& path, bool recording) { return (recording ? "rec|" : "play|") + path; }

}  // namespace

// Where a box goes when nobody has placed it: sources on the left, sinks on
// the right, effects in between, applications at either edge.
float Controller::columnOf(const Node& node) const {
    switch (node.kind) {
        case NodeKind::PlayingApp: return 40.0f;
        case NodeKind::Input:
        case NodeKind::WolfeSpeaker:
        case NodeKind::Loopback: return 330.0f;
        case NodeKind::Effect: return 620.0f;
        case NodeKind::Output:
        case NodeKind::WolfeMic: return 910.0f;
        case NodeKind::RecordingApp: return 1200.0f;
    }
    return 40.0f;
}

Controller::Controller(std::string documentPath, bool watch) : documentPath_(std::move(documentPath)) {
    if (!watch) graph_.setOfferVirtualDevices(false);
    graph_.start();
    if (watch) watcher_ = std::thread([this] { watchMain(); });
}

Controller::~Controller() {
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        stopWatching_ = true;
    }
    watchWake_.notify_all();
    if (watcher_.joinable()) watcher_.join();
    if (dirty_) save();
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

// Things the user made are always shown; devices while they exist, or while
// something is still connected to them; applications while they have audio open.
bool Controller::visible(const Node& node) const {
    switch (node.kind) {
        case NodeKind::Input:
        case NodeKind::Output: return node.present || linked(node.uid);
        case NodeKind::PlayingApp:
        case NodeKind::RecordingApp: return node.present;
        default: return true;
    }
}

const SystemLink* Controller::systemLink(int linkUid) const {
    for (const SystemLink& link : systemLinks_) {
        if (link.uid == linkUid) return &link;
    }
    return nullptr;
}

// A side has a pin where Wolfe can link, and where an application is
// connected by Windows.
bool Controller::showsInput(const Node& node) const {
    if (node.takesLinks() || node.kind == NodeKind::RecordingApp) return true;
    for (const SystemLink& link : systemLinks_) {
        if (link.to == node.uid) return true;
    }
    return false;
}

bool Controller::showsOutput(const Node& node) const {
    if (node.givesLinks() || node.kind == NodeKind::PlayingApp) return true;
    for (const SystemLink& link : systemLinks_) {
        if (link.from == node.uid) return true;
    }
    return false;
}

bool Controller::removable(const Node& node) const {
    switch (node.kind) {
        case NodeKind::Input:
        case NodeKind::Output: return !node.present;
        case NodeKind::PlayingApp:
        case NodeKind::RecordingApp: return false;
        default: return true;
    }
}

// Whether audio leaving `fromUid` arrives at `toUid`: along links made in
// Wolfe, from a playback device into what is played on it, and from one end
// of a virtual cable to the other.
bool Controller::reaches(int fromUid, int toUid) const {
    std::set<int> seen;
    std::function<bool(int)> visit = [&](int uid) {
        if (uid == toUid) return true;
        if (!seen.insert(uid).second) return false;
        for (const Connection& link : links_) {
            if (link.from == uid && visit(link.to)) return true;
        }
        const Node* here = node(uid);
        if (!here || here->kind != NodeKind::Output) return false;
        const std::string adapter = adapterOf(here->label);
        for (const Node& other : nodes_) {
            bool carries = false;
            if (other.kind == NodeKind::Loopback) {
                carries = other.resolved && here->resolved
                              ? other.target == here->target
                              : toLower(here->label).find(toLower(other.target)) != std::string::npos;
            } else if (other.kind == NodeKind::Input) {
                carries = looksLikeVirtualCable(adapter) && adapterOf(other.label) == adapter;
            }
            if (carries && visit(other.uid)) return true;
        }
        return false;
    };
    return visit(fromUid);
}

std::string Controller::whyNotFrom(const Node& node) const {
    const std::string name = "\"" + node.label + "\"";
    switch (node.kind) {
        case NodeKind::PlayingApp:
            return "Wolfe takes what applications play from the devices they play on. Have " + name +
                   " play on one of Wolfe's speakers, then link that speaker. " + kWhereWindowsDecides;
        case NodeKind::RecordingApp: return name + " records audio: nothing comes out of it.";
        case NodeKind::Output:
            return name + " is a playback device. To use what is played on it, right-click it and add a box for that.";
        case NodeKind::WolfeMic:
            return "Applications record from " + name + ". What goes into it is in Wolfe already: link from where it comes.";
        default: return {};
    }
}

std::string Controller::whyNotInto(const Node& node) const {
    const std::string name = "\"" + node.label + "\"";
    switch (node.kind) {
        case NodeKind::PlayingApp: return name + " plays audio: nothing goes into it.";
        case NodeKind::RecordingApp:
            return "To give audio to " + name + ", have it record from one of Wolfe's microphones and link into that. " +
                   kWhereWindowsDecides;
        case NodeKind::Input: return name + " is a recording device: Wolfe takes audio from it, it cannot play into it.";
        case NodeKind::WolfeSpeaker:
            return "Applications play on " + name + ", and Wolfe takes that in. To give audio to applications, link into "
                   "one of Wolfe's microphones.";
        case NodeKind::Loopback:
            return name + " is what is played on a device: to play there, link into the device itself.";
        default: return {};
    }
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
        description.closeToTray = closeToTray_;  // this belongs to the installation, not to the graph
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
    std::map<std::string, std::pair<float, float>> places = appPlaces_;
    for (const Node& node : nodes_) {
        if (node.isApp()) {
            if (node.kept && node.hasPosition) places[node.id] = {node.x, node.y};
            continue;
        }
        const bool device = node.kind == NodeKind::Input || node.kind == NodeKind::Output;
        if (device && !node.kept && !linked(node.uid)) continue;
        NodeDescription out;
        out.id = node.id;
        out.type = node.type;
        out.target = node.target;
        out.serial = node.serial;
        if (node.kind != NodeKind::Effect && !node.isWolfeDevice()) out.label = node.label;
        if (node.kind == NodeKind::Effect && node.label != effectLabel(node.type)) out.label = node.label;
        out.hasPosition = node.hasPosition;
        out.x = node.x;
        out.y = node.y;
        description.nodes.push_back(out);
    }
    for (const auto& [id, place] : places) description.layout.push_back({id, place.first, place.second});
    for (const Connection& link : links_) description.links.push_back({node(link.from)->id, node(link.to)->id, link.gainDb});

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
    for (const Node& node : nodes_) {
        if (node.active) graph_.removeNode(node.id);
    }
    nodes_.clear();
    links_.clear();
    systemLinks_.clear();
    appPlaces_.clear();
}

std::string Controller::adopt(const GraphDescription& description) {
    clear();
    marginMs_ = description.marginMs;
    closeToTray_ = description.closeToTray;
    graph_.setMarginMs(marginMs_);

    std::string problems;
    std::set<std::string> dropped;  // nodes left out, whose links are left out with them
    for (const NodeDescription& in : description.nodes) {
        if (isAppType(in.type)) {
            problems += "\"" + in.id + "\" captured one application by itself, which Wolfe no longer does: have " +
                        "that application play on one of Wolfe's speakers in Windows' settings, and link the speaker.\n";
            dropped.insert(in.id);
            continue;
        }
        Node node;
        node.id = in.id;
        node.type = in.type;
        node.kind = kindOfType(in.type);
        node.target = in.target;
        if (node.kind == NodeKind::Effect && !createEffect(node.type)) {
            problems += "unknown node type \"" + node.type + "\"\n";
            dropped.insert(in.id);
            continue;
        }
        if (findById(node.id)) {
            problems += "two nodes are called \"" + node.id + "\"\n";
            continue;
        }
        if (!in.label.empty() && !node.isWolfeDevice()) node.label = in.label;
        else if (node.kind == NodeKind::Effect) node.label = effectLabel(node.type);
        else node.label = node.target;
        if (node.kind == NodeKind::Loopback) node.detail = "what is played on it";
        if (node.isWolfeDevice()) {
            node.serial = in.serial.empty() ? serialFor(node.id) : in.serial;
            node.detail = node.kind == NodeKind::WolfeSpeaker ? "Wolfe's speaker" : "Wolfe's microphone";
        }
        // A device written by its own id looks like "{0.0.1.00000000}.{...}".
        node.resolved = startsWith(node.target, "{");
        node.present = node.kind == NodeKind::Effect;
        node.kept = true;
        node.hasPosition = in.hasPosition;
        node.x = in.x;
        node.y = in.y;
        if (startsWith(node.id, "fx|")) nextEffect_ = std::max(nextEffect_, atoi(node.id.c_str() + 3) + 1);
        Node& added = *find(insertNode(node));
        if (added.isWolfeDevice()) {
            try {
                ensureEngine(added);
            } catch (const std::exception& error) {
                problems += error.what() + std::string("\n");
            }
        }
    }

    for (const LinkDescription& in : description.links) {
        if (dropped.count(in.from) || dropped.count(in.to)) continue;
        Node* from = findById(in.from);
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
    for (const GraphDescription::Place& place : description.layout) appPlaces_[place.id] = {place.x, place.y};

    settle();
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

void Controller::ensureEngine(Node& node) {
    if (node.active) return;
    NodeDescription description;
    description.id = node.id;
    description.type = node.type;
    description.target = node.target;
    description.serial = node.serial;
    addNode(description, graph_);
    node.active = true;
}

// Takes a device out of the engine once no link uses it. Wolfe's own devices
// stay: applications may be using them.
void Controller::syncEngine(int nodeUid) {
    Node* node = find(nodeUid);
    if (!node || !node->active || node->isWolfeDevice() || linked(nodeUid)) return;
    graph_.removeNode(node->id);
    node->active = false;
    node->healthy = false;
    node->status.clear();
}

std::string Controller::link(int fromUid, int toUid) {
    Node* from = find(fromUid);
    Node* to = find(toUid);
    if (!from || !to) return "that node is not there any more";
    if (from == to) return "a node cannot be connected to itself";
    if (std::string why = whyNotFrom(*from); !why.empty()) return why;
    if (std::string why = whyNotInto(*to); !why.empty()) return why;
    for (const Connection& link : links_) {
        if (link.from == fromUid && link.to == toUid) return "those two are already connected";
    }
    if (reaches(toUid, fromUid)) {
        return "That would make a loop: what comes out of \"" + to->label + "\" would come back into it.";
    }

    try {
        ensureEngine(*from);
        ensureEngine(*to);
        graph_.addLink(from->id, to->id, 1.0f);
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
    graph_.removeLink(node(link.from)->id, node(link.to)->id);
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
        graph_.setLinkGain(node(link.from)->id, node(link.to)->id, std::pow(10.0f, gainDb / 20.0f));
        markDirty();
    }
}

std::string Controller::connect(int fromUid, int toUid, int replacedLinkUid) {
    if (!find(fromUid) || !find(toUid)) return "that node is not there any more";

    // A link pulled off one pin and dropped on another.
    for (const Connection& old : links_) {
        if (old.uid != replacedLinkUid) continue;
        if (old.from == fromUid && old.to == toUid) return {};  // put back where it was
        const std::string problem = link(fromUid, toUid);
        if (problem.empty()) unlink(replacedLinkUid);
        return problem;
    }
    if (const SystemLink* old = systemLink(replacedLinkUid)) {
        if (old->from == fromUid && old->to == toUid) return {};
        return disconnect(replacedLinkUid);
    }
    return link(fromUid, toUid);
}

std::string Controller::disconnect(int linkUid) {
    for (const Connection& link : links_) {
        if (link.uid != linkUid) continue;
        unlink(linkUid);
        return {};
    }
    if (systemLink(linkUid)) return std::string("Wolfe does not move applications. ") + kWhereWindowsDecides;
    return {};
}

void Controller::disconnectAll(int nodeUid) {
    std::vector<int> touching;
    for (const Connection& link : links_) {
        if (link.from == nodeUid || link.to == nodeUid) touching.push_back(link.uid);
    }
    for (int linkUid : touching) unlink(linkUid);
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

int Controller::addLoopback(int outputUid, float x, float y) {
    const Node* output = node(outputUid);
    if (!output || output->kind != NodeKind::Output) return 0;
    for (const Node& existing : nodes_) {
        if (existing.kind == NodeKind::Loopback && existing.target == output->target) return existing.uid;
    }
    Node node;
    node.id = "loop|" + output->target;
    node.kind = NodeKind::Loopback;
    node.type = "loopback";
    node.target = output->target;
    node.resolved = output->resolved;
    node.label = output->label;
    node.detail = "what is played on it";
    node.present = output->present;
    node.kept = true;
    node.hasPosition = true;
    node.x = x;
    node.y = y;
    markDirty();
    return insertNode(node);
}

std::string Controller::freeWolfeName(bool recording) const {
    const std::string base = recording ? "Wolfe Mic" : "Wolfe Speaker";
    for (int n = 1;; n++) {
        const std::string candidate = n == 1 ? base : base + " " + std::to_string(n);
        bool taken = false;
        for (const Node& node : nodes_) taken |= node.isWolfeDevice() && toLower(node.label) == toLower(candidate);
        if (!taken) return candidate;
    }
}

int Controller::addWolfeDevice(bool recording, const std::string& name, float x, float y, std::string* problem) {
    std::string trimmed = name;
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    auto fail = [&](const std::string& why) {
        if (problem) *problem = why;
        return 0;
    };
    if (trimmed.empty()) return fail("The device needs a name.");
    if (trimmed.size() > 40) return fail("That name is too long: at most 40 letters.");
    for (const Node& node : nodes_) {
        if (node.isWolfeDevice() && toLower(node.label) == toLower(trimmed)) {
            return fail("Wolfe already has a device called \"" + trimmed + "\".");
        }
    }

    // Windows knows the device by its serial number from now on: one that no
    // other device of Wolfe's has had.
    Node node;
    for (int attempt = 0; node.serial.empty() || findById("wolfe|" + node.serial); attempt++) {
        node.serial = serialFor(trimmed + "|" + std::to_string(GetTickCount64()) + "|" + std::to_string(attempt));
    }
    node.id = "wolfe|" + node.serial;
    node.kind = recording ? NodeKind::WolfeMic : NodeKind::WolfeSpeaker;
    node.type = recording ? "virtual_mic" : "virtual_speaker";
    node.target = trimmed;
    node.label = trimmed;
    node.detail = recording ? "Wolfe's microphone" : "Wolfe's speaker";
    node.kept = true;
    node.hasPosition = true;
    node.x = x;
    node.y = y;
    const int uid = insertNode(node);
    try {
        ensureEngine(*find(uid));
    } catch (const std::exception& error) {
        nodes_.pop_back();
        return fail(error.what());
    }
    markDirty();
    return uid;
}

void Controller::removeNode(int nodeUid) {
    Node* target = find(nodeUid);
    if (!target || !removable(*target)) return;
    disconnectAll(nodeUid);
    target = find(nodeUid);
    if (target->active) graph_.removeNode(target->id);  // Wolfe's own devices, which links do not keep
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.uid == nodeUid; }),
                 nodes_.end());
    systemLinks_.erase(std::remove_if(systemLinks_.begin(), systemLinks_.end(),
                                      [&](const SystemLink& link) { return link.from == nodeUid || link.to == nodeUid; }),
                       systemLinks_.end());
    markDirty();
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

// `kind` is Input or Output. Wolfe's own devices are among them: they are not
// boxes of their own but the devices of Wolfe's boxes.
void Controller::syncDevices(const std::vector<DeviceInfo>& devices, NodeKind kind) {
    const NodeKind wolfeKind = kind == NodeKind::Input ? NodeKind::WolfeMic : NodeKind::WolfeSpeaker;
    for (Node& node : nodes_) {
        if (node.kind == kind || node.kind == wolfeKind) node.present = false;
    }
    for (const DeviceInfo& device : devices) {
        const std::string id = toUtf8(device.id);
        if (!device.wolfeSerial.empty()) {
            for (Node& node : nodes_) {
                if (node.kind != wolfeKind || toLower(node.serial) != toLower(device.wolfeSerial)) continue;
                node.endpoint = id;
                node.present = true;
            }
            continue;
        }
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

// The applications that have audio open, and where Windows has them play or
// record from. Windows keeps listing a stream long after it has stopped, also
// on a device the application has since moved away from: while anything
// flows only that is shown, otherwise every stream it has open.
void Controller::syncApps(const Discovery& discovery) {
    for (Node& node : nodes_) {
        if (node.isApp()) node.present = false;
    }

    struct Streams {
        std::string path;
        bool recording = false;
        std::vector<const Discovery::Session*> sessions;
        bool flowing = false;
    };
    std::map<std::string, Streams> apps;  // by box id, in lower case
    for (const Discovery::Session& session : discovery.sessions) {
        Streams& streams = apps[toLower(appBoxId(session.app, session.recording))];
        if (streams.sessions.empty()) {
            streams.path = session.app;
            streams.recording = session.recording;
        }
        streams.sessions.push_back(&session);
        streams.flowing |= session.active;
    }

    std::vector<SystemLink> fresh;
    for (const auto& [key, streams] : apps) {
        const std::string id = appBoxId(streams.path, streams.recording);
        Node* app = nullptr;
        for (Node& node : nodes_) {
            if (node.isApp() && toLower(node.id) == key) app = &node;
        }
        if (!app) {
            Node node;
            node.id = id;
            node.kind = streams.recording ? NodeKind::RecordingApp : NodeKind::PlayingApp;
            node.type = "app";
            node.target = streams.path;
            node.label = fileName(streams.path);
            node.detail = folderHint(streams.path);
            if (auto place = appPlaces_.find(id); place != appPlaces_.end()) {
                node.hasPosition = node.kept = true;
                node.x = place->second.first;
                node.y = place->second.second;
            }
            app = find(insertNode(node));
        }
        app->present = true;

        for (const Discovery::Session* session : streams.sessions) {
            if (session->active != streams.flowing) continue;
            const Node* device = nullptr;
            for (const Node& node : nodes_) {
                const bool plain = node.kind == (streams.recording ? NodeKind::Input : NodeKind::Output) && node.resolved &&
                                   node.target == session->device;
                const bool ours = node.kind == (streams.recording ? NodeKind::WolfeMic : NodeKind::WolfeSpeaker) &&
                                  node.endpoint == session->device;
                if (plain || ours) device = &node;
            }
            if (!device) continue;
            const int from = streams.recording ? device->uid : app->uid;
            const int to = streams.recording ? app->uid : device->uid;
            bool known = false;
            for (const SystemLink& link : fresh) known |= link.from == from && link.to == to;
            if (known) continue;
            SystemLink link;
            link.from = from;
            link.to = to;
            link.active = session->active;
            // The same connection keeps its number, so that the canvas sees one
            // steady link and not a new one every time the system is looked at.
            for (const SystemLink& old : systemLinks_) {
                if (old.from == from && old.to == to) link.uid = old.uid;
            }
            if (!link.uid) link.uid = nextUid_++;
            fresh.push_back(link);
        }
    }
    systemLinks_ = std::move(fresh);

    // Applications that closed their audio leave the canvas; where the user
    // put them is remembered for when they come back.
    for (const Node& node : nodes_) {
        if (node.isApp() && !node.present && node.kept && node.hasPosition) appPlaces_[node.id] = {node.x, node.y};
    }
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [](const Node& node) { return node.isApp() && !node.present; }),
                 nodes_.end());
}

void Controller::apply(const Discovery& discovery) {
    syncDevices(discovery.inputs, NodeKind::Input);
    syncDevices(discovery.outputs, NodeKind::Output);

    // Windows makes a device the default one when it first appears, and again
    // whenever it comes back while it is the default it remembers. Wolfe can
    // only say so: choosing the default is the user's, in Windows' settings.
    for (bool recording : {false, true}) {
        for (const DeviceInfo& device : recording ? discovery.inputs : discovery.outputs) {
            if (device.wolfeSerial.empty() || !device.isDefault || !toldDefault_.insert(device.wolfeSerial).second) continue;
            std::string name = device.name;
            for (const Node& node : nodes_) {
                if (node.isWolfeDevice() && toLower(node.serial) == toLower(device.wolfeSerial)) name = node.label;
            }
            notice("Windows has made \"" + name + "\" its default " + (recording ? "recording" : "playback") +
                   " device: applications that do not choose one " + (recording ? "record from" : "play on") +
                   " it now. If that is not what you want, choose your own device as the default in Settings > "
                   "System > Sound; Windows remembers that.");
        }
    }

    // What is played on a device exists while the device does.
    for (Node& node : nodes_) {
        if (node.kind != NodeKind::Loopback) continue;
        node.present = false;
        for (const DeviceInfo& device : discovery.outputs) {
            const bool same = node.resolved ? toUtf8(device.id) == node.target
                                            : toLower(device.name).find(toLower(node.target)) != std::string::npos;
            if (same && device.wolfeSerial.empty()) {
                node.present = true;
                node.label = device.name;
            }
        }
    }

    syncApps(discovery);
    settle();
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
        if (node.isApp()) {
            auto found = appPeaks.find(toLower(node.id));
            if (found != appPeaks.end()) peak = found->second;
        } else if (node.kind != NodeKind::Effect) {
            // What is played on a device has the device's level.
            auto found = devicePeaks.find(node.isWolfeDevice() ? node.endpoint : node.target);
            if (found != devicePeaks.end()) peak = found->second;
        }
        if (node.active) {
            node.status = graph_.nodeStatus(node.id);
            node.healthy = engineIsFine(node.status);
            peak = std::max(peak, graph_.takeNodePeak(node.id));  // what the engine itself sees
        }
        node.level = std::max(std::min(peak, 1.0f), node.level * fall);
    }

    if (dirty_ && now - dirtySince_ > 1.0) save();
}

void Controller::watchMain() {
    ComApartment com;

    // The level meters found at the last look; they are read many times a second in between.
    struct Meter {
        bool ofDevice;    // otherwise: of one stream of an application
        std::string key;  // the device's id, or the application's box id (lower case)
        ComPtr<IAudioMeterInformation> meter;
    };
    std::vector<Meter> meters;
    double nextLook = 0.0;

    std::unique_lock<std::mutex> lock(watchMutex_);
    while (!stopWatching_) {
        if (lookNow_ || nowSeconds() >= nextLook) {
            const bool metering = lookOften_;
            lookNow_ = false;
            lock.unlock();

            Discovery discovery;
            meters.clear();
            try {
                discovery.inputs = listDevices(eCapture);
                discovery.outputs = listDevices(eRender);
                // Every stream open on every device, by application. Wolfe's
                // own are left out: its links on the canvas already say the same.
                ProcessSnapshot snapshot;
                for (const AudioSession& session : audioSessions()) {
                    if (session.pid == GetCurrentProcessId()) continue;
                    const std::string app = snapshot.path(session.pid);
                    if (app.empty()) continue;  // a system process
                    discovery.sessions.push_back({app, toUtf8(session.device), session.recording, session.active});
                    if (metering && session.meter) {
                        meters.push_back({false, toLower(appBoxId(app, session.recording)), session.meter});
                    }
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
            nextLook = nowSeconds() + (lookOften_ ? 1.5 : 6.0);
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

    // Where Windows has an application play or record from.
    auto devicesOf = [&](bool recording) {
        std::string names;
        for (const SystemLink& link : systemLinks_) {
            const Node* device = this->node(recording ? link.from : link.to);
            if ((recording ? link.to : link.from) != node.uid || !device) continue;
            names += (names.empty() ? "" : ", ") + device->label;
        }
        return names;
    };

    switch (node.kind) {
        case NodeKind::PlayingApp:
        case NodeKind::RecordingApp: {
            const bool recording = node.kind == NodeKind::RecordingApp;
            text = node.target + "\n";
            const std::string where = devicesOf(recording);
            if (!where.empty()) text += (recording ? "Records from " : "Plays on ") + where + ".\n";
            text += kWhereWindowsDecides;
            return text;
        }
        case NodeKind::Input: text = "Recording device\n"; break;
        case NodeKind::Output: text = "Playback device\n"; break;
        case NodeKind::Loopback: text = "Everything that is being played on this device\n"; break;
        case NodeKind::WolfeSpeaker:
            text = "One of Wolfe's own playback devices. What applications play on it comes into Wolfe,\n"
                   "and goes wherever it is linked to. In Windows it is called \"" + node.label + "\".\n";
            break;
        case NodeKind::WolfeMic:
            text = "One of Wolfe's own recording devices. Applications that record from it hear\n"
                   "whatever is linked into it. In Windows it is called \"" + node.label + "\".\n";
            break;
        case NodeKind::Effect:
            text = node.type == "rnnoise" ? "Removes background noise from a voice. Its output is mono.\n"
                                           : "Adds together whatever is connected to it.\n";
            break;
    }
    if (!node.present && (node.kind == NodeKind::Input || node.kind == NodeKind::Output || node.kind == NodeKind::Loopback)) {
        text += "Not plugged in.\n";
    }
    if (node.isWolfeDevice()) {
        std::string users;
        for (const SystemLink& link : systemLinks_) {
            const Node* app = this->node(link.from == node.uid ? link.to : link.to == node.uid ? link.from : 0);
            if (app) users += (users.empty() ? "" : ", ") + app->label;
        }
        if (!users.empty()) {
            text += (node.kind == NodeKind::WolfeSpeaker ? "Played on by " : "Recorded from by ") + users + ".\n";
        }
        if (!node.present && node.healthy) text += "Windows does not have it yet.\n";
    }

    if (!node.active) {
        if (node.kind != NodeKind::Effect) text += "Wolfe is not using it: nothing is linked to it here.";
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return text;
    }
    if (node.kind != NodeKind::Effect) text += node.status + "\n";

    if (node.kind == NodeKind::Output) {
        Graph::OutputReport report = graph_.outputReport(node.id);
        snprintf(line, sizeof(line), "Keeps %.0f ms of audio queued. Ran out of audio %llu times.\n",
                 report.output.queuedMs, (unsigned long long)report.output.underruns);
        text += line;
        for (const Graph::OutputReport::Source& source : report.sources) {
            const Node* from = findById(source.id);
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
        snprintf(line, sizeof(line),
                 "%s%s  ->  %s\nCarried by Wolfe.  Gain %+.1f dB\nRight-click to change it; drag the link to move the end you took.",
                 from->kind == NodeKind::Loopback ? "What is played on " : "", from->label.c_str(),
                 node(link.to)->label.c_str(), link.gainDb);
        return line;
    }
    const SystemLink* found = systemLink(linkUid);
    if (!found) return {};
    const Node* from = node(found->from);
    const Node* to = node(found->to);
    if (!from || !to) return {};
    const char* state = found->active ? "" : " (nothing is flowing right now)";
    const std::string what = from->isApp() ? from->label + " plays on " + to->label
                                           : to->label + " records from " + from->label;
    return what + state + ".\n" + kWhereWindowsDecides;
}

}  // namespace wolfe
