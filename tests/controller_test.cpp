// Checks the logic behind the graphical editor without drawing anything and
// without touching real devices: the devices it is told about do not exist,
// so the engine only ever fails to open them, and Wolfe's own devices are
// never given to Windows. Names are made up for the same reason: a real one
// would open the real device.
#include "app/controller.h"
#include "app_devices.h"
#include "default_devices.h"

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
        const int first = controller.addEffect("gain", 400, 100), second = controller.addEffect("gain", 400, 300);
        expect(first && second && controller.node(first)->parameters[0] == 0.0f, "a gain starts at 0 dB");
        expect(controller.node(first)->givesLinks() && controller.node(first)->takesLinks(), "audio goes in and comes out");

        expect(controller.connect(mic, first).empty() && controller.connect(first, phones).empty(),
               "microphone -> gain -> headphones");
        expect(controller.connect(first, second).empty(), "gain -> gain connects");
        expect(!controller.connect(second, first).empty(), "a loop is refused");

        controller.setParameter(first, 0, -6.0f);
        expect(controller.node(first)->parameters[0] == -6.0f, "the gain changes");
        controller.setParameter(second, 0, 1000.0f);
        expect(controller.node(second)->parameters[0] == 24.0f, "within limits");

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
        if (node.kind == NodeKind::Effect && node.parameters[0] == -6.0f) gain = &node;
    }
    expect(gain && gain->x == 400 && gain->y == 100, "the gain is back where it was, with its value");
    expect(reloaded.links().size() == 3 && reloaded.nodes().size() == 4, "everything else is back too");
}

// Which applications Wolfe moves back to its devices when they come back.
void testStrayStreams() {
    printf("applications that lost Wolfe's device\n");
    const std::set<std::wstring> wolfe = {L"{wolfe-music}", L"{wolfe-chat}"};
    auto stray = [&](std::vector<AppStream> streams) { return strayStreams(streams, wolfe); };

    std::vector<AppStream> found = stray({{10, L"{phones}", false, true, L"{wolfe-music}"}});
    expect(found.size() == 1 && found[0].pid == 10 && found[0].chosen == L"{wolfe-music}",
           "given Wolfe's speaker, playing on the headphones: moved back");
    expect(stray({{10, L"{phones}", false, true, L""}}).empty(), "put back on the default device by the user: left alone");
    expect(stray({{10, L"{phones}", false, true, L"{speakers}"}}).empty(), "given another device: left alone");
    expect(stray({{10, L"{phones}", false, true, L"{wolfe-gone}"}}).empty(), "given a device of Wolfe's that is not there: left alone");
    expect(stray({{10, L"{wolfe-music}", false, true, L"{wolfe-music}"}}).empty(), "already where it was put: left alone");
    expect(stray({{10, L"{phones}", false, false, L"{wolfe-music}"}}).empty(), "a silent stream elsewhere: left alone");
    expect(stray({{10, L"{wolfe-music}", false, true, L"{wolfe-music}"}, {10, L"{phones}", false, true, L"{wolfe-music}"}}).empty(),
           "playing where it was put and also elsewhere, on purpose: left alone");
    found = stray({{10, L"{phones}", false, true, L"{wolfe-music}"}, {10, L"{speakers}", false, true, L"{wolfe-music}"},
                   {10, L"{mic}", true, true, L"{wolfe-chat}"}});
    expect(found.size() == 2, "once per process and direction");
}

void testMuting() {
    printf("muting\n");
    const std::string document = tempFile("wolfe-test-mute.json");
    int gainUid = 0;
    {
        Controller controller(document, false);
        controller.apply(system());
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        const int gain = controller.addEffect("gain", 400, 100);
        gainUid = gain;
        expect(!controller.node(mic)->muted && !controller.node(gain)->muted, "boxes start unmuted");
        controller.setMuted(mic, true);
        expect(controller.node(mic)->muted && !controller.node(mic)->active, "a box can be muted before it is in use");
        expect(controller.connect(mic, gain).empty() && controller.node(mic)->active && controller.node(mic)->muted, "and stays muted once it is");
        controller.setMuted(gain, true);
        controller.setMuted(gain, false);
        expect(!controller.node(gain)->muted, "unmuted again");
        controller.save();
    }
    expect(readFile(document).find("\"muted\": true") != std::string::npos, "muting is written in the file");
    Controller reloaded(document, false);
    reloaded.load();
    int muted = 0;
    for (const Node& node : reloaded.nodes()) muted += node.muted ? 1 : 0;
    expect(muted == 1 && firstOf(reloaded, NodeKind::Input)->muted, "and is back after loading, only where it was");
    (void)gainUid;
}

void testEffects() {
    printf("effects\n");
    const std::string document = tempFile("wolfe-test-effects.json");
    {
        Controller controller(document, false);
        int made = 0, offered = 0;
        for (const EffectType& type : effectTypes()) {
            if (!type.offered) continue;
            offered++;
            const int uid = controller.addEffect(type.type, 400, 100.0f * offered);
            const Node* node = controller.node(uid);
            made += node && node->label == type.name && node->parameters.size() == type.parameters.size() ? 1 : 0;
        }
        expect(offered >= 10 && made == offered, "every effect offered can be added, with its settings");
        expect(controller.addEffect("nonsense", 0, 0) == 0, "one that does not exist cannot");
        for (const EffectType& type : effectTypes()) {
            if (type.offered && std::string(type.help).empty()) printf("    (%s has no help)\n", type.type);
        }
        bool helped = true;
        for (const EffectType& type : effectTypes()) helped &= !type.offered || std::string(type.help).size() > 20;
        expect(helped, "every effect offered says what it is for");

        const int filter = controller.addEffect("lowpass", 700, 100);
        const int reverb = controller.addEffect("reverb", 700, 300);
        expect(controller.node(filter)->parameters[0] == 8000.0f, "settings start at their initial values");
        controller.setParameter(filter, 0, 1.0f);
        expect(controller.node(filter)->parameters[0] == 200.0f, "a setting stays within its range");
        controller.setParameter(reverb, 0, 85.0f);
        controller.setParameter(reverb, 9, 1.0f);  // no such setting: nothing happens
        controller.resetParameters(filter);
        expect(controller.node(filter)->parameters[0] == 8000.0f, "and can be put back");
        controller.save();
    }
    const std::string text = readFile(document);
    expect(text.find("\"size\": 85.0") != std::string::npos, "settings are written in the file by name");

    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads without complaints");
    const Node* reverb = nullptr;
    for (const Node& node : reloaded.nodes()) {
        if (node.type == "reverb") reverb = &node;
    }
    expect(reverb && reverb->parameters[0] == 85.0f && reverb->parameters[2] == 25.0f,
           "the reverb is back with what was changed, the rest as it was");
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
    const Node* quiet = firstOf(controller, NodeKind::Effect);
    const Node* phones = firstOf(controller, NodeKind::Output);
    const Node* chat = firstOf(controller, NodeKind::WolfeMic);
    expect(quiet && quiet->parameters[0] == -3.0f, "the gain has its value");
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
        expect(controller.node(chat)->takesLinks() && controller.node(chat)->givesLinks(),
               "Wolfe's microphone is a sink, and passes on what goes into it");
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
        expect(controller.connect(chat, phones).empty(), "what goes into Wolfe's microphone can go on to the headphones");
        expect(!controller.connect(chat, chat).empty() && !controller.connect(phones, chat).empty(),
               "but not back into itself, and nothing comes out of the headphones");
        const int gain = controller.addEffect("gain", 600, 500);
        expect(controller.connect(chat, gain).empty() && !controller.connect(gain, chat).empty(),
               "a loop through Wolfe's microphone is refused");
        controller.removeNode(gain);
        controller.disconnect(linkBetween(controller, chat, phones));

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
        const int gain = controller.addEffect("gain", 400, 300);

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
        expect(controller.describeNode(phones) == "Game (playing)\nChat (silent)",
               "the tooltip lists them all, saying which are silent");
        expect(controller.describeNode(mic) == "Chat (recording)", "on a recording device, who records");

        controller.apply(system());
        expect(controller.node(phones)->apps.empty(), "an application that closes its stream goes away");
        expect(controller.describeNode(phones).empty() && controller.describeNode(gain).empty(),
               "with nothing to list there is no tooltip");

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
    const int gain = controller.addEffect("gain", 400, 100);
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
    expect(junction && junction->kind == NodeKind::Effect && junction->type == "mix" && junction->label == "Junction",
           "a junction is a junction");
    expect(denoise && denoise->type == "rnnoise" && denoise->parameters.size() == 2 && denoise->parameters[0] == 100.0f &&
               denoise->parameters[1] == 50.0f,
           "noise suppression is noise suppression, at full amount, with the voice gate it did not have");
    expect(controller.nodes().size() == 4 && controller.links().size() == 3, "the rest is there");
    const Node* mic = byLabel(controller, kMic);
    expect(mic && mic->x < junction->x, "a box with no place of its own is laid out with the others");
}

void testLinkMuting() {
    printf("muting a link\n");
    const std::string document = tempFile("wolfe-test-linkmute.json");
    {
        Controller controller(document, false);
        controller.apply(system());
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        const int junction = controller.addEffect("mix", 400, 100);
        const int first = controller.addEffect("gain", 700, 100), second = controller.addEffect("gain", 700, 300);
        controller.connect(mic, junction);
        controller.connect(junction, first);
        controller.connect(junction, second);
        const int toSecond = linkBetween(controller, junction, second);
        expect(controller.silentLinks().empty(), "nothing muted, every link carries sound");
        controller.setLinkMuted(toSecond, true);
        expect(controller.silentLinks() == std::set<int>{toSecond}, "a muted link is silent, the others are not");
        controller.save();
    }
    expect(readFile(document).find("\"muted\": true") != std::string::npos, "a muted link is written in the file");
    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads");
    int muted = 0;
    for (const Connection& link : reloaded.links()) muted += link.muted ? 1 : 0;
    expect(muted == 1, "and the link is muted again after loading");
}

void testSilence() {
    printf("which links carry silence\n");
    const std::string document = tempFile("wolfe-test-silence.json");
    Controller controller(document, false);
    controller.apply(system());
    const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
    const int a = controller.addEffect("gain", 300, 100), b = controller.addEffect("gain", 500, 100);
    const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
    const int other = controller.addEffect("gain", 300, 300);
    controller.connect(mic, a);
    controller.connect(a, b);
    controller.connect(b, phones);
    controller.connect(other, b);  // nothing goes into this one
    const int micToA = linkBetween(controller, mic, a), aToB = linkBetween(controller, a, b);
    const int bToPhones = linkBetween(controller, b, phones), otherToB = linkBetween(controller, other, b);
    expect(controller.silentLinks() == std::set<int>{otherToB}, "a box with nothing coming in gives silence");
    controller.setMuted(a, true);
    expect(controller.silentLinks() == std::set<int>{micToA, aToB, bToPhones, otherToB},
           "a muted box silences the links into it and everything after it");
    controller.setMuted(a, false);
    controller.setMuted(mic, true);
    expect(controller.silentLinks().size() == 4, "and so does a muted source");
    controller.setMuted(mic, false);
    controller.setLinkMuted(micToA, true);
    expect(controller.silentLinks().count(bToPhones) == 1, "a muted link silences what comes after it too");
    controller.setLinkMuted(micToA, false);
    controller.setMuted(phones, true);
    expect(controller.silentLinks() == std::set<int>{bToPhones, otherToB}, "a muted sink only silences the links into it");
}

void testSelector() {
    printf("a selector\n");
    const std::string document = tempFile("wolfe-test-selector.json");
    int chosenUid = 0;
    {
        Controller controller(document, false);
        controller.apply(system());
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 100);
        const int cable = controller.addDevice(true, "{fake-cable-out}", 100, 300);
        const int third = controller.addEffect("gain", 100, 500);
        const int selector = controller.addEffect("select", 400, 100);
        const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
        expect(selector && controller.node(selector)->isSelector(), "a selector can be added");
        expect(controller.selectedInput(selector) == 0, "with nothing linked it lets nothing through");
        controller.connect(mic, selector);
        controller.connect(cable, selector);
        controller.connect(third, selector);
        controller.connect(selector, phones);
        expect(controller.inputsOf(selector) == std::vector<int>({mic, cable, third}), "its inputs, in the order they were linked");
        expect(controller.selectedInput(selector) == mic, "the first one goes through until another is chosen");
        const std::set<int> silent = controller.silentLinks();
        expect(!silent.count(linkBetween(controller, mic, selector)) && silent.count(linkBetween(controller, cable, selector)),
               "the others carry silence");
        controller.select(selector, cable);
        expect(controller.selectedInput(selector) == cable, "choosing another switches to it");
        expect(controller.silentLinks().count(linkBetween(controller, mic, selector)) == 1, "and the first is silent now");
        controller.select(selector, phones);  // not one of its inputs
        expect(controller.selectedInput(selector) == cable, "something that is not linked into it cannot be chosen");
        controller.disconnect(linkBetween(controller, cable, selector));
        expect(controller.selectedInput(selector) == mic, "its choice unlinked, the first one goes through");
        controller.connect(cable, selector);
        expect(controller.selectedInput(selector) == mic, "and stays when the other comes back");
        controller.select(selector, third);
        chosenUid = third;

        expect(controller.inputName(selector, mic) == "Test Microphone" && controller.inputName(selector, third) == "Gain",
               "an input is called by its source's name, a device's without its adapter");
        controller.renameInput(selector, mic, "  Voice  ");
        controller.renameInput(selector, third, "Musica più bassa");
        expect(controller.inputName(selector, mic) == "Voice" && controller.inputRenamed(selector, mic),
               "it can be given a name of the user's, trimmed");
        expect(controller.inputName(selector, third) == "Musica più bassa", "in any language");
        controller.renameInput(selector, cable, std::string(60, 'x'));
        expect(controller.inputName(selector, cable).size() == 40, "of at most 40 letters");
        controller.renameInput(selector, cable, "");
        expect(!controller.inputRenamed(selector, cable) && controller.inputName(selector, cable) == "CABLE Output",
               "an empty name gives back the source's own");
        controller.renameInput(selector, phones, "Nope");
        controller.save();
    }
    expect(readFile(document).find("\"selected\"") != std::string::npos, "the choice is written in the file");
    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads");
    const Node* selector = nullptr;
    for (const Node& node : reloaded.nodes()) {
        if (node.isSelector()) selector = &node;
    }
    const int chosen = selector ? reloaded.selectedInput(selector->uid) : 0;
    expect(chosen && reloaded.node(chosen)->type == "gain", "and it is the same one after loading");
    bool onlyChosenHeard = chosen != 0;
    for (int from : selector ? reloaded.inputsOf(selector->uid) : std::vector<int>()) {
        onlyChosenHeard &= reloaded.engineGain(linkBetween(reloaded, from, selector->uid)) == (from == chosen ? 1.0f : 0.0f);
    }
    expect(onlyChosenHeard, "the engine lets through that one alone, though it was linked last");
    expect(chosen && reloaded.inputName(selector->uid, chosen) == "Musica più bassa" && selector->inputNames.size() == 2,
           "with the names given to its inputs");
    const Node* mic = firstOf(reloaded, NodeKind::Input);
    reloaded.removeNode(mic->uid);
    for (const Node& node : reloaded.nodes()) {
        if (node.isSelector()) selector = &node;
    }
    expect(selector->inputNames.size() == 1, "a source removed takes its name with it");
    (void)chosenUid;
}

void testImportExport() {
    printf("taking a graph to another computer\n");
    const std::string document = tempFile("wolfe-test-export-home.json");
    const std::string exported = tempFile("wolfe-test-exported.json");
    std::string serial;
    {
        Controller controller(document, false);
        controller.apply(system());
        std::string problem;
        const int music = controller.addWolfeDevice(false, "Wolfe Music", 100, 100, &problem);
        const int mic = controller.addDevice(true, "{fake-mic}", 100, 300);
        const int phones = controller.addDevice(false, "{fake-phones}", 700, 100);
        const int gain = controller.addEffect("gain", 400, 300);
        controller.setParameter(gain, 0, -9.0f);
        controller.connect(music, phones);
        controller.connect(mic, gain);
        controller.connect(gain, phones);
        serial = controller.node(music)->serial;
        expect(controller.exportTo(exported).empty(), "the graph is exported to a file");
    }

    // Another computer: the same microphone under another id, no headphones,
    // a graph of its own and other preferences.
    const std::string elsewhere = tempFile("wolfe-test-export-away.json");
    Controller away(elsewhere, false);
    Discovery there;
    there.inputs.push_back({kMic, L"{another-mic-id}", true, {}});
    away.apply(there);
    away.setCloseToTray(false);
    away.addEffect("reverb", 0, 0);
    bool imported = false;
    expect(!away.importFrom(tempFile("wolfe-test-missing.json"), &imported).empty() && !imported &&
               away.nodes().size() == 1,
           "a file that cannot be read changes nothing");
    away.importFrom(exported, &imported);
    expect(imported, "the exported file is imported");
    expect(away.nodes().size() == 4 && away.links().size() == 3, "it replaces the graph that was there");
    const Node* mic = firstOf(away, NodeKind::Input);
    expect(mic && mic->present && mic->target == "{another-mic-id}", "the microphone is found by its name");
    const Node* phones = firstOf(away, NodeKind::Output);
    expect(phones && !phones->present && phones->label == kPhones, "missing headphones wait under their name");
    const Node* music = firstOf(away, NodeKind::WolfeSpeaker);
    expect(music && music->serial == serial, "Wolfe's speaker keeps its identity");
    expect(!away.closeToTray(), "this computer's preferences stay");
    const Node* gain = nullptr;
    for (const Node& node : away.nodes()) {
        if (node.type == "gain") gain = &node;
    }
    expect(gain && gain->parameters[0] == -9.0f, "settings come along");
}

void testExample() {
    printf("the example to start from\n");
    const std::string document = tempFile("wolfe-test-example.json");
    Controller controller(document, false);
    controller.apply(system());
    const std::string problems = controller.buildExample();
    expect(problems.empty(), "it is made without problems");
    expect(firstOf(controller, NodeKind::WolfeSpeaker) && firstOf(controller, NodeKind::WolfeMic),
           "with a Wolfe speaker and a Wolfe microphone");
    const Node* mic = firstOf(controller, NodeKind::Input);
    const Node* phones = firstOf(controller, NodeKind::Output);
    expect(mic && mic->target == "{fake-mic}" && phones && phones->target == "{fake-phones}", "and the default devices");
    expect(controller.nodes().size() == 6 && controller.links().size() == 5, "six boxes, five links");
    expect(controller.silentLinks().empty(), "nothing silent");
    Controller again(tempFile("wolfe-test-example2.json"), false);
    expect(!again.buildExample().empty(), "without devices to use it says what is missing");
}

void testDefaultKeeper() {
    printf("Windows' default device and a new device of Wolfe's\n");
    DefaultKeeper keeper;
    std::map<std::wstring, std::string> present = {{L"{phones}", ""}, {L"{speakers}", ""}};
    std::map<ERole, std::wstring> phones = {{eConsole, L"{phones}"}, {eMultimedia, L"{phones}"}, {eCommunications, L"{speakers}"}};
    expect(keeper.decide(eRender, present, phones, 0).empty(), "nothing new, nothing done");
    keeper.expect("NEW1");
    expect(keeper.decide(eRender, present, phones, 1).empty(), "before Windows has the new device, nothing done");
    present[L"{wolfe}"] = "new1";
    std::map<ERole, std::wstring> taken = {{eConsole, L"{wolfe}"}, {eMultimedia, L"{wolfe}"}, {eCommunications, L"{speakers}"}};
    std::vector<DefaultKeeper::Change> changes = keeper.decide(eRender, present, taken, 2);
    expect(changes.size() == 2 && changes[0].device == L"{phones}" && changes[1].device == L"{phones}",
           "made the default by Windows: the headphones are put back, role by role");
    expect(keeper.decide(eRender, present, taken, 3).empty(), "only once: chosen again, it is the user's choice");

    DefaultKeeper other;
    other.expect("NEW2");
    std::map<std::wstring, std::string> withOld = {{L"{phones}", ""}, {L"{old}", "OLD"}};
    std::map<ERole, std::wstring> old = {{eConsole, L"{old}"}};
    other.decide(eRender, withOld, phones, 0);
    expect(other.decide(eRender, withOld, old, 1).empty(), "a device of Wolfe's that is not new is left as default");
    withOld[L"{new2}"] = "NEW2";
    std::map<ERole, std::wstring> late = {{eConsole, L"{new2}"}};
    other.decide(eRender, withOld, {{eConsole, L"{phones}"}}, 10);
    expect(other.decide(eRender, withOld, late, 80).empty(), "after its first minute a new device is left alone");
}

}  // namespace

int main() {
    testDevices();
    testGains();
    testEffects();
    testMuting();
    testStrayStreams();
    testHandWrittenFile();
    testWolfeDevices();
    testApplications();
    testLoops();
    testOldFile();
    testLinkMuting();
    testSilence();
    testSelector();
    testImportExport();
    testExample();
    testDefaultKeeper();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
