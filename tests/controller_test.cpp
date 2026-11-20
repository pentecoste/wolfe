// Checks the logic behind the graphical editor without drawing anything and
// without touching real devices: the devices it is told about do not exist,
// so the engine only ever fails to open them, and Wolfe's own devices are
// never given to Windows. Names are made up for the same reason: a real one
// would open the real device.
#include "app/controller.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wolfe;

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
    printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

std::string tempFile(const char* name) {
    std::string path = toUtf8((std::filesystem::temp_directory_path() / name).wstring());
    std::filesystem::remove(std::filesystem::path(toWide(path)));
    return path;
}

void writeFile(const std::string& path, const char* text) {
    std::ofstream file(std::filesystem::path(toWide(path)));
    file << text;
}

std::string readFile(const std::string& path) {
    std::ifstream file(std::filesystem::path(toWide(path)));
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

const char* const kMic = "Test Microphone (Nowhere Audio)";
const char* const kPhones = "Test Headphones (Nowhere Audio)";
const char* const kCableIn = "CABLE Input (VB-Audio Virtual Cable)";
const char* const kCableOut = "CABLE Output (VB-Audio Virtual Cable)";

Discovery system() {
    Discovery discovery;
    discovery.inputs.push_back({kMic, L"{fake-mic}", true, {}});
    discovery.inputs.push_back({kCableOut, L"{fake-cable-out}", false, {}});
    discovery.outputs.push_back({kCableIn, L"{fake-cable-in}", false, {}});
    discovery.outputs.push_back({kPhones, L"{fake-phones}", true, {}});
    return discovery;
}

const Node* byLabel(const Controller& controller, const std::string& label) {
    for (const Node& node : controller.nodes()) {
        if (node.label == label) return &node;
    }
    return nullptr;
}

const Node* firstOf(const Controller& controller, NodeKind kind) {
    for (const Node& node : controller.nodes()) {
        if (node.kind == kind) return &node;
    }
    return nullptr;
}

bool hasLink(const Controller& controller, int from, int to) {
    for (const Connection& link : controller.links()) {
        if (link.from == from && link.to == to) return true;
    }
    return false;
}

int linkBetween(const Controller& controller, int from, int to) {
    for (const Connection& link : controller.links()) {
        if (link.from == from && link.to == to) return link.uid;
    }
    return 0;
}

void testDevices() {
    printf("devices of the computer\n");
    const std::string document = tempFile("wolfe-test-devices.json");
    Controller controller(document, false);
    Discovery discovery = system();
    discovery.outputs.push_back({"Speakers (Wolfe Music)", L"{wolfe-music}", false, "0123456789AB"});
    controller.apply(discovery);

    expect(controller.nodes().empty(), "nothing appears on the canvas by itself");
    expect(controller.devicesToAdd(true).size() == 2 && controller.devicesToAdd(false).size() == 2,
           "every device can be added, Wolfe's own left out");

    const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
    const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
    expect(mic && phones, "a microphone and headphones are added");
    const Node* micNode = controller.node(mic);
    expect(micNode->kind == NodeKind::Input && micNode->label == kMic && micNode->present,
           "the microphone is a source, named as Windows names it");
    expect(controller.node(phones)->kind == NodeKind::Output, "the headphones are a sink");
    expect(!micNode->active && !controller.node(phones)->active, "a device that is only shown is not opened");
    expect(controller.devicesToAdd(true).size() == 1, "a device on the canvas is not offered again");
    expect(controller.addDevice(true, "{fake-mic}", 0, 0) == 0, "nor can it be added twice");
    expect(controller.addDevice(false, "{fake-mic}", 0, 0) == 0, "a recording device is not a playback device");
    expect(controller.addDevice(false, "{wolfe-music}", 0, 0) == 0, "Wolfe's own devices are not added this way");

    expect(controller.connect(mic, phones).empty(), "microphone -> headphones connects");
    expect(controller.node(mic)->active && controller.node(phones)->active, "both are now in use");
    expect(!controller.connect(mic, phones).empty(), "the same link twice is refused");
    expect(!controller.connect(phones, mic).empty(), "nothing comes out of the headphones");
    expect(!controller.connect(mic, mic).empty(), "a node cannot feed itself");
    expect(controller.links().size() == 1, "the refused links left nothing behind");

    controller.disconnect(controller.links()[0].uid);
    expect(!controller.node(mic)->active && !controller.node(phones)->active, "disconnected devices are closed");

    controller.connect(mic, phones);
    controller.apply(Discovery());
    expect(controller.node(mic) && !controller.node(mic)->present && controller.node(mic)->label == kMic,
           "an unplugged device stays on the canvas, with its name");
    controller.removeNode(mic);
    expect(controller.node(mic) == nullptr && controller.links().empty(), "any box can be removed, with its links");
}

void testGains() {
    printf("gains\n");
    const std::string document = tempFile("wolfe-test-gains.json");
    {
        Controller controller(document, false);
        controller.apply(system());
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
        const int first = controller.addGain(400, 100), second = controller.addGain(400, 300);
        expect(first && second && controller.node(first)->gainDb == 0.0f, "a gain starts at 0 dB");
        expect(controller.node(first)->givesLinks() && controller.node(first)->takesLinks(), "audio goes in and comes out");

        expect(controller.connect(mic, first).empty() && controller.connect(first, phones).empty(),
               "microphone -> gain -> headphones");
        expect(controller.connect(first, second).empty(), "gain -> gain connects");
        expect(!controller.connect(second, first).empty(), "a loop is refused");

        controller.setGainDb(first, -6.0f);
        expect(controller.node(first)->gainDb == -6.0f, "the gain changes");
        controller.setGainDb(second, 1000.0f);
        expect(controller.node(second)->gainDb == Controller::kMaxGainDb, "within limits");

        const int link = linkBetween(controller, mic, first);
        expect(controller.move(link, mic, second).empty() && hasLink(controller, mic, second) &&
                   !hasLink(controller, mic, first),
               "a link taken by one end goes onto another pin");
        const int back = linkBetween(controller, first, second);
        expect(!controller.move(back, second, first).empty() && hasLink(controller, first, second),
               "a move that would make a loop leaves the link where it was");
        controller.save();
    }
    const std::string text = readFile(document);
    expect(text.find("\"gain_db\": -6.0") != std::string::npos, "the gain is written in the file");

    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads without complaints");
    reloaded.apply(system());
    const Node* gain = nullptr;
    for (const Node& node : reloaded.nodes()) {
        if (node.kind == NodeKind::Gain && node.gainDb == -6.0f) gain = &node;
    }
    expect(gain && gain->x == 400 && gain->y == 100, "the gain is back where it was, with its value");
    expect(reloaded.links().size() == 3 && reloaded.nodes().size() == 4, "everything else is back too");
}

void testHandWrittenFile() {
    printf("a file written by hand\n");
    const std::string document = tempFile("wolfe-test-hand.json");
    writeFile(document, R"({
      // comments are allowed
      "nodes": [
        { "id": "mic",    "type": "input",      "device": "test microphone" },
        { "id": "quiet",  "type": "gain",       "gain_db": -3 },
        { "id": "phones", "type": "output",     "device": "default" },
        { "id": "chat",   "type": "virtual_mic", "name": "Wolfe Chat" }
      ],
      "links": [
        { "from": "mic",   "to": "quiet" },
        { "from": "quiet", "to": "chat" }
      ]
    })");
    Controller controller(document, false);
    expect(controller.load().empty(), "it loads without complaints");
    // (Nothing is linked to "default": that would open the real device.)
    expect(controller.nodes().size() == 4 && controller.links().size() == 2, "four boxes, two links");
    const Node* mic = firstOf(controller, NodeKind::Input);
    const Node* quiet = firstOf(controller, NodeKind::Gain);
    const Node* phones = firstOf(controller, NodeKind::Output);
    const Node* chat = firstOf(controller, NodeKind::WolfeMic);
    expect(quiet && quiet->gainDb == -3.0f, "the gain has its value");
    expect(chat && !chat->serial.empty() && chat->active, "Wolfe's microphone gets a serial number and exists at once");
    expect(mic && quiet && phones && mic->x < quiet->x && quiet->x < phones->x,
           "boxes without a place are laid out: sources, gains, sinks");
    expect(phones && chat && phones->x == chat->x && phones->y != chat->y, "one under the other in a column");

    controller.apply(system());
    mic = firstOf(controller, NodeKind::Input);
    phones = firstOf(controller, NodeKind::Output);
    expect(mic && mic->id == "mic" && mic->target == "{fake-mic}" && mic->label == kMic,
           "a device named by part of its name is tied to the device itself");
    expect(phones && phones->target == "{fake-phones}", "\"default\" is the default device");
}

void testWolfeDevices() {
    printf("Wolfe's own devices\n");
    const std::string document = tempFile("wolfe-test-wolfe.json");
    std::string serial;
    {
        Controller controller(document, false);
        controller.apply(system());
        std::string problem;
        expect(controller.freeWolfeName(false) == "Wolfe Speaker", "a new speaker gets a name");
        const int music = controller.addWolfeDevice(false, " Wolfe Music ", 300, 300, &problem);
        expect(music && problem.empty(), "a speaker is made");
        const int chat = controller.addWolfeDevice(true, "Wolfe Chat", 900, 300, &problem);
        expect(chat && problem.empty(), "and a microphone");
        expect(!controller.addWolfeDevice(false, "wolfe music", 0, 0, &problem) && !problem.empty(),
               "two devices of Wolfe's cannot have the same name");
        expect(!controller.addWolfeDevice(true, "  ", 0, 0, &problem) && !problem.empty(), "nor none");
        const Node* speaker = controller.node(music);
        expect(speaker->label == "Wolfe Music" && speaker->active && !speaker->serial.empty(),
               "it exists in the engine at once, with a serial number of its own");
        expect(controller.node(chat)->serial != speaker->serial, "every device has its own");
        expect(speaker->givesLinks() && !speaker->takesLinks(), "Wolfe's speaker is a source");
        expect(controller.node(chat)->takesLinks() && !controller.node(chat)->givesLinks(), "Wolfe's microphone is a sink");
        serial = speaker->serial;

        Discovery discovery = system();
        discovery.outputs.push_back({"Speakers (Wolfe Music)", L"{wolfe-music}", false, serial});
        discovery.inputs.push_back({"Line (Wolfe Chat)", L"{wolfe-chat}", true, controller.node(chat)->serial});
        controller.apply(discovery);
        expect(controller.node(music)->present && controller.node(music)->endpoint == "{wolfe-music}",
               "Windows has them: they are found by their serial number");
        expect(controller.nodes().size() == 2, "and they are not boxes a second time");

        const int phones = controller.addDevice(false, "{fake-phones}", 900, 100);
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        expect(controller.connect(music, phones).empty(), "Wolfe's speaker -> headphones connects");
        expect(controller.connect(mic, chat).empty(), "microphone -> Wolfe's microphone connects");
        expect(controller.connect(music, chat).empty(), "and Wolfe's speaker into it too");
        expect(!controller.connect(phones, music).empty(), "nothing goes into Wolfe's speaker");
        expect(!controller.connect(chat, phones).empty(), "nothing comes out of Wolfe's microphone");

        controller.disconnectAll(music);
        expect(controller.node(music)->active, "unlinked, Wolfe's device stays: applications may use it");
        expect(controller.connect(music, phones).empty(), "linked again");
        controller.save();
    }

    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads");
    const Node* music = firstOf(reloaded, NodeKind::WolfeSpeaker);
    expect(music && music->serial == serial && music->active,
           "Wolfe's speaker is back, with the same serial number, before Windows is even looked at");
    expect(reloaded.links().size() == 2, "and so are the links");
    const int uid = music ? music->uid : 0;
    reloaded.removeNode(uid);
    expect(reloaded.node(uid) == nullptr && reloaded.links().size() == 1, "removed, it takes its links with it");
}

void testApplications() {
    printf("applications on each device\n");
    const std::string document = tempFile("wolfe-test-apps.json");
    {
        Controller controller(document, false);
        controller.apply(system());
        std::string problem;
        const int music = controller.addWolfeDevice(false, "Wolfe Music", 300, 100, &problem);
        const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        const int gain = controller.addGain(400, 300);

        Discovery discovery = system();
        discovery.outputs.push_back({"Speakers (Wolfe Music)", L"{wolfe-music}", false, controller.node(music)->serial});
        // A player and a browser on Wolfe's speaker, the browser with two
        // streams of which one is silent; a chat program that records from the
        // microphone and keeps a silent stream on the headphones; a game on
        // the headphones.
        discovery.sessions.push_back({"Player", "{wolfe-music}", false, true});
        discovery.sessions.push_back({"Browser", "{wolfe-music}", false, false});
        discovery.sessions.push_back({"Browser", "{wolfe-music}", false, true});
        discovery.sessions.push_back({"Chat", "{fake-mic}", true, true});
        discovery.sessions.push_back({"Chat", "{fake-phones}", false, false});
        discovery.sessions.push_back({"Game", "{fake-phones}", false, true});
        controller.apply(discovery);

        const std::vector<AppUse>& onMusic = controller.node(music)->apps;
        expect(onMusic.size() == 2 && onMusic[0].name == "Browser" && onMusic[0].active && onMusic[1].name == "Player",
               "Wolfe's speaker: each program once, active if any of its streams is");
        const std::vector<AppUse>& onPhones = controller.node(phones)->apps;
        expect(onPhones.size() == 2 && onPhones[0].name == "Game" && onPhones[1].name == "Chat" && !onPhones[1].active,
               "the headphones: those with audio flowing first, a silent stream after");
        const std::vector<AppUse>& onMic = controller.node(mic)->apps;
        expect(onMic.size() == 1 && onMic[0].name == "Chat", "the microphone: who records from it, not who plays");
        expect(controller.node(gain)->apps.empty(), "a gain has none");
        const std::string tooltip = controller.describeNode(phones);
        expect(tooltip.find("Game  (playing)") != std::string::npos &&
                   tooltip.find("Chat  (stream open") != std::string::npos,
               "the tooltip lists them all, saying which are silent");

        controller.apply(system());
        expect(controller.node(phones)->apps.empty(), "an application that closes its stream goes away");
        expect(controller.describeNode(phones).find("No application plays on it") != std::string::npos,
               "and the tooltip says nobody is there");

        expect(controller.showApps(), "boxes list applications unless told not to");
        controller.setShowApps(false);
        controller.save();
    }
    Controller reloaded(document, false);
    reloaded.load();
    expect(!reloaded.showApps(), "not listing them is remembered");
}

void testLoops() {
    printf("loops through a virtual cable\n");
    const std::string document = tempFile("wolfe-test-loops.json");
    Controller controller(document, false);
    controller.apply(system());
    const int cableIn = controller.addDevice(false, "{fake-cable-in}", 700, 100);
    const int cableOut = controller.addDevice(true, "{fake-cable-out}", 100, 100);
    const int mic = controller.addDevice(true, "{fake-mic}", 100, 300);
    const int gain = controller.addGain(400, 100);
    expect(!controller.connect(cableOut, cableIn).empty(), "one end of a cable cannot feed the other");
    expect(controller.connect(cableOut, gain).empty(), "the cable's far end into a gain");
    expect(!controller.connect(gain, cableIn).empty(), "and from there into its near end neither");
    expect(controller.connect(mic, cableIn).empty(), "something else into the cable is fine");
    expect(controller.links().size() == 2, "the refused links left nothing behind");
}

void testOldFile() {
    printf("a file from an earlier version\n");
    const std::string document = tempFile("wolfe-test-old.json");
    writeFile(document, R"json({
      "margin_ms": 4,
      "close_to_tray": true,
      "nodes": [
        { "id": "out|{fake-phones}", "type": "output", "device": "{fake-phones}", "label": "Test Headphones (Nowhere Audio)", "x": 620, "y": 40 },
        { "id": "app|C:\\Players\\player.exe", "type": "app", "process": "C:\\Players\\player.exe" },
        { "id": "loop|{fake-phones}", "type": "loopback", "device": "{fake-phones}" },
        { "id": "fx|1", "type": "rnnoise" },
        { "id": "fx|2", "type": "mix", "x": 300, "y": 40 },
        { "id": "in|{fake-mic}", "type": "input", "device": "{fake-mic}", "label": "Test Microphone (Nowhere Audio)" }
      ],
      "links": [
        { "from": "app|C:\\Players\\player.exe", "to": "out|{fake-phones}" },
        { "from": "in|{fake-mic}", "to": "fx|1" },
        { "from": "in|{fake-mic}", "to": "fx|2", "gain_db": -3 },
        { "from": "fx|2", "to": "out|{fake-phones}" }
      ],
      "layout": { "play|C:\\Players\\player.exe": [10, 900] }
    })json");
    Controller controller(document, false);
    const std::string problems = controller.load();
    expect(problems.find("app|") != std::string::npos && problems.find("loop|") != std::string::npos,
           "what Wolfe no longer has is left out, and said so");
    expect(problems.find("a link uses") == std::string::npos, "its links go with it quietly");
    expect(problems.find("had a gain") != std::string::npos, "a link's gain is mentioned");
    const Node* junction = nullptr;
    const Node* denoise = nullptr;
    for (const Node& node : controller.nodes()) {
        if (node.id == "fx|2") junction = &node;
        if (node.id == "fx|1") denoise = &node;
    }
    expect(junction && junction->kind == NodeKind::Gain && junction->gainDb == 0.0f, "a junction is a gain at 0 dB");
    expect(denoise && denoise->kind == NodeKind::Gain && problems.find("noise suppression") != std::string::npos,
           "so is noise suppression, so that its links stay; the user is told");
    expect(controller.nodes().size() == 4 && controller.links().size() == 3, "the rest is there");
    const Node* mic = byLabel(controller, kMic);
    expect(mic && mic->x < junction->x, "a box with no place of its own is laid out with the others");
}

}  // namespace

int main() {
    testDevices();
    testGains();
    testHandWrittenFile();
    testWolfeDevices();
    testApplications();
    testLoops();
    testOldFile();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
