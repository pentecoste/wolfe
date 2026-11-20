#include "controller.h"

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

bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

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

bool engineIsFine(const std::string& status) {
    return startsWith(status, "capturing") || startsWith(status, "playing") || startsWith(status, "running") ||
           startsWith(status, "ready") || startsWith(status, "being added");
}

float factorOf(float gainDb) { return std::pow(10.0f, gainDb / 20.0f); }

const char* const kGainLabel = "Gain";

}  // namespace

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

// Whether audio leaving `fromUid` arrives at `toUid`: along links, and from
// one end of a virtual cable to the other.
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
        if (!looksLikeVirtualCable(adapter)) return false;
        for (const Node& other : nodes_) {
            if (other.kind == NodeKind::Input && adapterOf(other.label) == adapter && visit(other.uid)) return true;
        }
        return false;
    };
    return visit(fromUid);
}

std::string Controller::newId(const char* prefix) const {
    for (int n = 1;; n++) {
        const std::string id = prefix + std::to_string(n);
        if (!const_cast<Controller*>(this)->findById(id)) return id;
    }
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

void Controller::save() {
    GraphDescription description;
    description.marginMs = marginMs_;
    description.closeToTray = closeToTray_;
    for (const Node& node : nodes_) {
        NodeDescription out;
        out.id = node.id;
        out.type = node.type;
        out.target = node.target;
        out.serial = node.serial;
        out.gainDb = node.gainDb;
        if (node.isDevice()) out.label = node.label;
        out.hasPosition = node.hasPosition;
        out.x = node.x;
        out.y = node.y;
        description.nodes.push_back(out);
    }
    for (const Connection& link : links_) description.links.push_back({node(link.from)->id, node(link.to)->id, 0.0});

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
}

std::string Controller::adopt(const GraphDescription& description) {
    clear();
    marginMs_ = description.marginMs;
    closeToTray_ = description.closeToTray;
    graph_.setMarginMs(marginMs_);

    std::string problems;
    std::set<std::string> dropped;  // nodes left out, whose links are left out with them
    for (const NodeDescription& in : description.nodes) {
        Node node;
        node.id = in.id;
        node.type = in.type;
        node.target = in.target;
        node.hasPosition = in.hasPosition;
        node.x = in.x;
        node.y = in.y;
        if (in.type == "input" || in.type == "output") {
            node.kind = in.type == "input" ? NodeKind::Input : NodeKind::Output;
            node.label = in.label.empty() ? in.target : in.label;
            // A device written by its own id looks like "{0.0.1.00000000}.{...}".
            node.resolved = startsWith(node.target, "{");
        } else if (isVirtualType(in.type)) {
            node.kind = in.type == "virtual_mic" ? NodeKind::WolfeMic : NodeKind::WolfeSpeaker;
            node.label = in.target;
            node.serial = in.serial.empty() ? serialFor(node.id) : in.serial;
            node.detail = node.kind == NodeKind::WolfeSpeaker ? "Wolfe speaker" : "Wolfe microphone";
        } else if (in.type == "gain" || in.type == "mix" || in.type == "rnnoise") {
            // A junction of the earlier versions is a gain that changes nothing.
            // So is, from now on, noise suppression: its links are kept that way.
            if (in.type == "rnnoise") {
                problems += "\"" + in.id + "\" was noise suppression, which Wolfe no longer has: it is a gain at 0 dB now.\n";
            }
            node.kind = NodeKind::Gain;
            node.type = "gain";
            node.label = kGainLabel;
            node.gainDb = std::max(kMinGainDb, std::min(kMaxGainDb, (float)in.gainDb));
            node.present = true;
        } else {
            problems += "\"" + in.id + "\" is a kind of node Wolfe does not have (" + in.type + "): it was left out.\n";
            dropped.insert(in.id);
            continue;
        }
        if (findById(node.id)) {
            problems += "two nodes are called \"" + node.id + "\"\n";
            dropped.insert(in.id);
            continue;
        }
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
        if (in.gainDb != 0.0) {
            problems += "the link \"" + in.from + "\" -> \"" + in.to + "\" had a gain, which links no longer have: " +
                        "it carries the audio as it is.\n";
        }
        const std::string error = link(from->uid, to->uid);
        if (!error.empty()) problems += error + "\n";
    }

    for (Node& node : nodes_) {
        if (!node.hasPosition) place(node);
    }
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
// Links
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
    description.gainDb = node.gainDb;
    addNode(description, graph_);
    node.active = true;
}

// Takes a node out of the engine once no link uses it. Wolfe's own devices
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
    if (!from || !to) return "That box is not there any more.";
    if (from == to) return "A box cannot be linked to itself.";
    if (!from->givesLinks()) return "Nothing comes out of \"" + from->label + "\": audio goes into it.";
    if (!to->takesLinks()) return "Nothing goes into \"" + to->label + "\": audio comes out of it.";
    for (const Connection& link : links_) {
        if (link.from == fromUid && link.to == toUid) return "Those two are already linked.";
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

std::string Controller::connect(int fromUid, int toUid) { return link(fromUid, toUid); }

std::string Controller::move(int linkUid, int fromUid, int toUid) {
    for (const Connection& old : links_) {
        if (old.uid != linkUid) continue;
        if (old.from == fromUid && old.to == toUid) return {};  // put back where it was
        const std::string problem = link(fromUid, toUid);
        if (problem.empty()) unlink(linkUid);
        return problem;
    }
    return link(fromUid, toUid);
}

void Controller::disconnect(int linkUid) { unlink(linkUid); }

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

int Controller::addGain(float x, float y) {
    Node node;
    node.id = newId("gain|");
    node.kind = NodeKind::Gain;
    node.type = "gain";
    node.label = kGainLabel;
    node.present = true;
    node.hasPosition = true;
    node.x = x;
    node.y = y;
    markDirty();
    return insertNode(node);
}

void Controller::setGainDb(int nodeUid, float gainDb) {
    Node* node = find(nodeUid);
    if (!node || node->kind != NodeKind::Gain) return;
    gainDb = std::max(kMinGainDb, std::min(kMaxGainDb, gainDb));
    if (node->gainDb == gainDb) return;
    node->gainDb = gainDb;
    if (node->active) graph_.setEffectGain(node->id, factorOf(gainDb));
    markDirty();
}

std::vector<DeviceInfo> Controller::devicesToAdd(bool recording) const {
    const NodeKind kind = recording ? NodeKind::Input : NodeKind::Output;
    std::vector<DeviceInfo> result;
    for (const DeviceInfo& device : recording ? devices_.inputs : devices_.outputs) {
        if (!device.wolfeSerial.empty()) continue;
        const std::string id = toUtf8(device.id);
        bool shown = false;
        for (const Node& node : nodes_) shown |= node.kind == kind && node.resolved && node.target == id;
        if (!shown) result.push_back(device);
    }
    return result;
}

int Controller::addDevice(bool recording, const std::string& deviceId, float x, float y) {
    const NodeKind kind = recording ? NodeKind::Input : NodeKind::Output;
    const DeviceInfo* device = nullptr;
    for (const DeviceInfo& candidate : recording ? devices_.inputs : devices_.outputs) {
        if (toUtf8(candidate.id) == deviceId && candidate.wolfeSerial.empty()) device = &candidate;
    }
    if (!device) return 0;
    for (const Node& node : nodes_) {
        if (node.kind == kind && node.resolved && node.target == deviceId) return 0;
    }
    Node node;
    node.id = (recording ? "in|" : "out|") + deviceId;
    node.kind = kind;
    node.type = recording ? "input" : "output";
    node.target = deviceId;
    node.resolved = true;
    node.label = device->name;
    node.present = true;
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
    node.detail = recording ? "Wolfe microphone" : "Wolfe speaker";
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
    if (!target) return;
    disconnectAll(nodeUid);
    target = find(nodeUid);
    if (target->active) graph_.removeNode(target->id);  // Wolfe's own devices, which links do not keep
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.uid == nodeUid; }),
                 nodes_.end());
    markDirty();
}

void Controller::moveNode(int nodeUid, float x, float y) {
    Node* node = find(nodeUid);
    if (!node) return;
    if (node->hasPosition && std::fabs(node->x - x) < 0.5f && std::fabs(node->y - y) < 0.5f) return;
    node->x = x;
    node->y = y;
    node->hasPosition = true;
    markDirty();
}

// A box from a file that does not say where it goes (one written by hand):
// sources on the left, gains in the middle, sinks on the right, each under
// the lowest box of its column.
void Controller::place(Node& node) {
    const float x = node.givesLinks() && !node.takesLinks() ? 80.0f : node.takesLinks() && !node.givesLinks() ? 680.0f : 380.0f;
    float y = 60.0f;
    for (const Node& other : nodes_) {
        if (&other == &node || !other.hasPosition) continue;
        if (std::fabs(other.x - x) < 190.0f) y = std::max(y, other.y + 100.0f);
    }
    node.x = x;
    node.y = y;
    node.hasPosition = true;
    markDirty();
}

// ---------------------------------------------------------------------------
// Keeping up with the system and the engine
// ---------------------------------------------------------------------------

// `kind` is Input or Output. Wolfe's own devices are among the devices: they
// are found by their serial number.
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
            const bool matches = fragment == "default" ? device.isDefault : toLower(device.name).find(fragment) != std::string::npos;
            if (!matches) continue;
            node.target = id;
            node.resolved = true;
            markDirty();
            found = &node;
        }
        if (!found) continue;
        found->present = true;
        if (found->label != device.name) {
            found->label = device.name;
            markDirty();
        }
    }
}

void Controller::apply(const Discovery& discovery) {
    devices_ = discovery;
    syncDevices(discovery.inputs, NodeKind::Input);
    syncDevices(discovery.outputs, NodeKind::Output);
}

void Controller::update(double now) {
    const double elapsed = now_ > 0 ? now - now_ : 0.0;
    now_ = now;

    Discovery discovery;
    std::map<std::string, float> devicePeaks;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(watchMutex_);
        if (hasPending_) {
            discovery = std::move(pending_);
            hasPending_ = false;
            fresh = true;
        }
        devicePeaks.swap(devicePeaks_);
    }
    if (fresh) apply(discovery);

    // Levels are the ones Windows measures, so that a box shows what is really
    // passing through the device whether or not Wolfe is involved. They fall
    // back by themselves in about a third of a second.
    const float fall = (float)std::exp(-elapsed / 0.3);
    for (Node& node : nodes_) {
        float peak = 0.0f;
        if (node.kind != NodeKind::Gain) {
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
    std::vector<DeviceMeter> meters;
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
                if (metering) meters = deviceMeters();
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
                float& highest = devicePeaks_[toUtf8(meters[i].id)];
                highest = std::max(highest, values[i]);
            }
            watchWake_.wait_for(lock, std::chrono::milliseconds(50), wake);
        } else {
            meters.clear();
            devicePeaks_.clear();
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
        case NodeKind::Input: text = "A recording device of this computer: what it records comes into Wolfe.\n"; break;
        case NodeKind::Output: text = "A playback device of this computer: Wolfe plays on it what is linked into it.\n"; break;
        case NodeKind::WolfeSpeaker:
            text = "One of Wolfe's own playback devices: what applications play on it comes into Wolfe.\n"
                   "In Windows it is called \"" + node.label + "\"; which applications play on it is chosen there.\n";
            break;
        case NodeKind::WolfeMic:
            text = "One of Wolfe's own recording devices: applications that record from it hear what is linked into it.\n"
                   "In Windows it is called \"" + node.label + "\"; which applications record from it is chosen there.\n";
            break;
        case NodeKind::Gain:
            snprintf(line, sizeof(line),
                     "Passes on what is linked into it, %+.1f dB.\n"
                     "Drag the value left or right, or double-click it to type one.",
                     node.gainDb);
            return line;
    }
    if (node.isDevice() && !node.present) text += "Not plugged in.\n";
    if (node.isWolfeDevice() && !node.present && node.healthy) text += "Windows does not have it yet.\n";

    if (!node.active) {
        text += "Wolfe is not using it: nothing is linked to it.";
        return text;
    }
    text += node.status + "\n";

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
        return node(link.from)->label + "  ->  " + node(link.to)->label +
               "\nDrag it by one end to move that end; pull it away, or Ctrl+click it, to remove it.";
    }
    return {};
}

}  // namespace wolfe
