#include "config.h"

#include "nlohmann/json.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace wolfe {

namespace {

using Json = nlohmann::ordered_json;

std::string requireString(const Json& object, const char* key, const std::string& where) {
    auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        throw std::runtime_error(where + ": missing \"" + key + "\"");
    }
    return found->get<std::string>();
}

std::string optionalString(const Json& object, const char* key) {
    auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

}  // namespace

bool isDeviceType(const std::string& type) { return type == "input" || type == "loopback" || type == "output"; }
bool isAppType(const std::string& type) { return type == "app"; }
bool isVirtualType(const std::string& type) { return type == "virtual_speaker" || type == "virtual_mic"; }

GraphDescription loadDescription(const std::string& path) {
    std::ifstream file(std::filesystem::path(toWide(path)), std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    std::stringstream text;
    text << file.rdbuf();

    Json root;
    try {
        root = Json::parse(text.str(), nullptr, true, /* ignore_comments */ true);
    } catch (const Json::parse_error& error) {
        throw std::runtime_error(path + ": " + error.what());
    }
    if (!root.is_object()) throw std::runtime_error(path + ": expected an object at the top");

    GraphDescription description;
    if (root.contains("margin_ms") && root["margin_ms"].is_number()) description.marginMs = root["margin_ms"].get<double>();
    if (root.contains("close_to_tray") && root["close_to_tray"].is_boolean()) {
        description.closeToTray = root["close_to_tray"].get<bool>();
    }
    description.silentDevice = optionalString(root, "silent_device");

    for (const Json& item : root.value("nodes", Json::array())) {
        NodeDescription node;
        node.id = requireString(item, "id", "node");
        const std::string where = "node \"" + node.id + "\"";
        node.type = requireString(item, "type", where);
        if (isDeviceType(node.type)) node.target = requireString(item, "device", where);
        else if (isAppType(node.type)) node.target = requireString(item, "process", where);
        else if (isVirtualType(node.type)) {
            node.target = requireString(item, "name", where);
            node.serial = optionalString(item, "serial");
        }
        node.label = optionalString(item, "label");
        if (item.contains("x") && item.contains("y") && item["x"].is_number() && item["y"].is_number()) {
            node.hasPosition = true;
            node.x = item["x"].get<float>();
            node.y = item["y"].get<float>();
        }
        node.parked = item.contains("parked") && item["parked"].is_boolean() && item["parked"].get<bool>();
        description.nodes.push_back(node);
    }

    for (const Json& item : root.value("links", Json::array())) {
        LinkDescription link;
        link.from = requireString(item, "from", "link");
        link.to = requireString(item, "to", "link");
        if (item.contains("gain_db") && item["gain_db"].is_number()) link.gainDb = item["gain_db"].get<double>();
        description.links.push_back(link);
    }
    return description;
}

void saveDescription(const std::string& path, const GraphDescription& description) {
    Json root = Json::object();
    root["margin_ms"] = description.marginMs;
    root["close_to_tray"] = description.closeToTray;
    if (!description.silentDevice.empty()) root["silent_device"] = description.silentDevice;

    Json nodes = Json::array();
    for (const NodeDescription& node : description.nodes) {
        Json item = Json::object();
        item["id"] = node.id;
        item["type"] = node.type;
        if (isDeviceType(node.type)) item["device"] = node.target;
        else if (isAppType(node.type)) item["process"] = node.target;
        else if (isVirtualType(node.type)) {
            item["name"] = node.target;
            if (!node.serial.empty()) item["serial"] = node.serial;
        }
        if (!node.label.empty()) item["label"] = node.label;
        if (node.hasPosition) {
            item["x"] = std::round(node.x);
            item["y"] = std::round(node.y);
        }
        if (node.parked) item["parked"] = true;
        nodes.push_back(item);
    }
    root["nodes"] = nodes;

    Json links = Json::array();
    for (const LinkDescription& link : description.links) {
        Json item = Json::object();
        item["from"] = link.from;
        item["to"] = link.to;
        if (link.gainDb != 0.0) item["gain_db"] = std::round(link.gainDb * 10.0) / 10.0;
        links.push_back(item);
    }
    root["links"] = links;

    // Written next to the real file and swapped in, so that a crash in the
    // middle never leaves half a file behind.
    const std::filesystem::path target(toWide(path));
    std::filesystem::path temporary = target;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("cannot write " + path);
        file << root.dump(2) << "\n";
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        throw std::runtime_error("cannot replace " + path);
    }
}

void addNode(const NodeDescription& node, Graph& graph) {
    if (node.type == "input") graph.addInput(node.id, {InputKind::Device, node.target});
    else if (node.type == "loopback") graph.addInput(node.id, {InputKind::Loopback, node.target});
    else if (node.type == "app") graph.addInput(node.id, {InputKind::Process, node.target});
    else if (node.type == "output") graph.addOutput(node.id, node.target);
    else if (isVirtualType(node.type)) {
        VirtualDeviceSpec spec;
        spec.recording = node.type == "virtual_mic";
        spec.name = node.target;
        spec.serial = node.serial.empty() ? serialFor(node.id) : node.serial;
        graph.addVirtualDevice(node.id, spec);
    } else {
        graph.addEffect(node.id, node.type);
    }
}

void buildGraph(const GraphDescription& description, Graph& graph) {
    graph.setMarginMs(description.marginMs);

    std::set<std::string> linked;
    for (const LinkDescription& link : description.links) {
        linked.insert(link.from);
        linked.insert(link.to);
    }
    for (const NodeDescription& node : description.nodes) {
        if (linked.count(node.id) || isVirtualType(node.type)) addNode(node, graph);
    }
    for (const LinkDescription& link : description.links) {
        graph.addLink(link.from, link.to, (float)std::pow(10.0, link.gainDb / 20.0));
    }
}

}  // namespace wolfe
