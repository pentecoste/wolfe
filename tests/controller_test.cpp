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

const Node* byLabel(const Controller& controller, const std::string& label, NodeKind kind) {
    for (const Node& node : controller.nodes()) {
        if (node.label == label && node.kind == kind) return &node;
    }
    return nullptr;
}

int uidOf(const Controller& controller, const std::string& label, NodeKind kind) {
    const Node* node = byLabel(controller, label, kind);
    return node ? node->uid : 0;
}

bool hasSystemLink(const Controller& controller, int from, int to, bool active) {
    for (const SystemLink& link : controller.systemLinks()) {
        if (link.from == from && link.to == to && link.active == active) return true;
    }
    return false;
}

int linkBetween(const Controller& controller, int from, int to) {
    for (const Connection& link : controller.links()) {
        if (link.from == from && link.to == to) return link.uid;
    }
    for (const SystemLink& link : controller.systemLinks()) {
        if (link.from == from && link.to == to) return link.uid;
    }
    return 0;
}

void testEditing() {
    printf("discovering devices, connecting and disconnecting\n");
    const std::string document = tempFile("wolfe-test-editing.json");
    Controller controller(document, false);
    controller.apply(system());

    const int mic = uidOf(controller, kMic, NodeKind::Input);
    const int phones = uidOf(controller, kPhones, NodeKind::Output);
    expect(controller.nodes().size() == 4 && mic && phones, "one box per device");
    expect(!controller.node(mic)->active && !controller.node(phones)->active, "a device that is only shown is not opened");
    expect(controller.showsOutput(*controller.node(mic)) && !controller.showsInput(*controller.node(mic)),
           "a recording device is a source: a pin on its right only");
    expect(controller.showsInput(*controller.node(phones)) && !controller.showsOutput(*controller.node(phones)),
           "a playback device is a sink: a pin on its left only");

    expect(controller.connect(mic, phones).empty(), "microphone -> headphones connects");
    expect(controller.node(mic)->active && controller.node(phones)->active, "both are now in use");
    expect(!controller.connect(mic, phones).empty(), "the same link twice is refused");
    expect(!controller.connect(phones, mic).empty(), "a playback device cannot feed a recording device");
    expect(!controller.connect(mic, mic).empty(), "a node cannot feed itself");

    const int first = controller.addEffect("mix", 300, 40), second = controller.addEffect("rnnoise", 300, 200);
    expect(controller.connect(first, second).empty(), "effect -> effect connects");
    expect(!controller.connect(second, first).empty(), "a loop is refused");
    expect(controller.links().size() == 2, "the refused links left nothing behind");

    controller.setGainDb(controller.links()[0].uid, -6.0f);
    controller.moveNode(mic, 123, 456);
    controller.save();

    controller.disconnect(controller.links()[0].uid);
    expect(!controller.node(mic)->active && !controller.node(phones)->active, "disconnected devices are closed");
    controller.removeNode(first);
    expect(controller.node(first) == nullptr && controller.links().empty(), "removing an effect removes its links");
    expect(!controller.removable(*controller.node(mic)), "a device that is plugged in cannot be removed");

    printf("saving and loading\n");
    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the saved file loads without complaints");
    reloaded.apply(system());
    const Node* micAgain = byLabel(reloaded, kMic, NodeKind::Input);
    expect(reloaded.links().size() == 2, "both links are back");
    expect(micAgain && micAgain->x == 123 && micAgain->y == 456, "the microphone is where it was put");
    expect(micAgain && micAgain->active, "linked nodes are in use right after loading");
    bool gainKept = false;
    for (const Connection& link : reloaded.links()) gainKept |= link.gainDb == -6.0f;
    expect(gainKept, "the gain is back");
    expect(reloaded.nodes().size() == 6, "no device appears twice");
}

void testHandWrittenFile() {
    printf("a file written by hand, with devices named by part of their name\n");
    const std::string source = tempFile("wolfe-test-source.json");
    writeFile(source, R"({
      // comments are allowed
      "nodes": [
        { "id": "mic",      "type": "input",   "device": "test microphone" },
        { "id": "denoise",  "type": "rnnoise" },
        { "id": "soundrio", "type": "app",     "process": "\\WolfeTestApp\\" },
        { "id": "cable",    "type": "output",  "device": "CABLE Input" }
      ],
      "links": [
        { "from": "mic",      "to": "denoise" },
        { "from": "denoise",  "to": "cable" },
        { "from": "soundrio", "to": "cable", "gain_db": -3 }
      ]
    })");
    const std::string document = tempFile("wolfe-test-imported.json");
    Controller controller(document, false);
    const std::string problems = controller.importFrom(source);
    expect(problems.find("soundrio") != std::string::npos && problems.find("Wolfe's speakers") != std::string::npos,
           "an application captured by itself is left out, and the user is told what to do instead");
    expect(controller.links().size() == 2 && controller.nodes().size() == 3, "the rest is there: three nodes, two links");
    int visible = 0;
    for (const Node& node : controller.nodes()) visible += controller.visible(node) ? 1 : 0;
    expect(visible == 3, "everything it connects is shown, plugged in or not");

    controller.apply(system());
    expect(controller.nodes().size() == 5, "only the headphones and the cable's far end are new");
    const Node* mic = byLabel(controller, kMic, NodeKind::Input);
    expect(mic && mic->id == "mic" && mic->target == "{fake-mic}", "the microphone is now tied to the device itself");

    controller.apply(Discovery());
    mic = byLabel(controller, kMic, NodeKind::Input);
    expect(mic && !mic->present && controller.visible(*mic), "an unplugged device stays while it is connected");
    expect(mic && controller.removable(*mic), "and can be removed by hand");
}

Discovery withApplications() {
    Discovery discovery = system();
    // A player on the headphones; a chat program that records from the
    // microphone and has a silent stream open on the headphones; a game that
    // moved from the cable to the headphones (Windows still lists the old one).
    discovery.sessions.push_back({"C:\\Players\\player.exe", "{fake-phones}", false, true});
    discovery.sessions.push_back({"C:\\Chat\\chat.exe", "{fake-mic}", true, true});
    discovery.sessions.push_back({"C:\\Chat\\chat.exe", "{fake-phones}", false, false});
    discovery.sessions.push_back({"D:\\Games\\Thing\\thing.exe", "{fake-cable-in}", false, false});
    discovery.sessions.push_back({"D:\\Games\\Thing\\thing.exe", "{fake-phones}", false, true});
    return discovery;
}

void testApplications() {
    printf("applications, shown where Windows has them\n");
    const std::string document = tempFile("wolfe-test-apps.json");
    Controller controller(document, false);
    controller.apply(withApplications());

    const int player = uidOf(controller, "player.exe", NodeKind::PlayingApp);
    const int chatPlays = uidOf(controller, "chat.exe", NodeKind::PlayingApp);
    const int chatRecords = uidOf(controller, "chat.exe", NodeKind::RecordingApp);
    const int thing = uidOf(controller, "thing.exe", NodeKind::PlayingApp);
    const int mic = uidOf(controller, kMic, NodeKind::Input);
    const int phones = uidOf(controller, kPhones, NodeKind::Output);
    const int cableIn = uidOf(controller, kCableIn, NodeKind::Output);
    expect(player && chatPlays && chatRecords && thing, "one box per application and direction: chat.exe has two");
    expect(byLabel(controller, "thing.exe", NodeKind::PlayingApp)->detail == "Games\\Thing", "with where it lives");

    expect(controller.systemLinks().size() == 4, "four connections are shown");
    expect(hasSystemLink(controller, player, phones, true), "the player on the headphones");
    expect(hasSystemLink(controller, chatPlays, phones, false), "the silent stream, marked as not flowing");
    expect(hasSystemLink(controller, mic, chatRecords, true), "the microphone into the chat program");
    expect(hasSystemLink(controller, thing, phones, true) && !hasSystemLink(controller, thing, cableIn, false),
           "the game where it plays now, not where it played before");
    expect(controller.links().empty(), "none of them is a link of Wolfe's");
    expect(controller.showsOutput(*controller.node(player)) && !controller.showsInput(*controller.node(player)),
           "an application that plays is a source");
    expect(controller.showsInput(*controller.node(chatRecords)) && !controller.showsOutput(*controller.node(chatRecords)),
           "an application that records is a sink");

    const int firstUid = controller.systemLinks()[0].uid;
    controller.apply(withApplications());
    expect(controller.systemLinks()[0].uid == firstUid, "a connection that stays keeps its identity");

    expect(!controller.connect(player, cableIn).empty() && controller.links().empty(),
           "Wolfe does not take audio from an application directly");
    expect(!controller.connect(mic, chatRecords).empty() && controller.links().empty(),
           "nor send audio into one");
    expect(!controller.disconnect(linkBetween(controller, player, phones)).empty() &&
               controller.systemLinks().size() == 4,
           "nor move one: a link Windows makes stays");
    expect(!controller.connect(player, cableIn, linkBetween(controller, player, phones)).empty() &&
               controller.links().empty(),
           "not even when it is dragged elsewhere");
    expect(!controller.removable(*controller.node(player)), "an application cannot be removed");

    controller.moveNode(player, 10, 900);
    Discovery quiet = system();
    controller.apply(quiet);
    expect(byLabel(controller, "player.exe", NodeKind::PlayingApp) == nullptr && controller.systemLinks().empty(),
           "applications that close their audio leave the canvas");
    controller.apply(withApplications());
    const Node* back = byLabel(controller, "player.exe", NodeKind::PlayingApp);
    expect(back && back->x == 10 && back->y == 900, "and come back where they were put");
    controller.save();

    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the file loads");
    reloaded.apply(withApplications());
    back = byLabel(reloaded, "player.exe", NodeKind::PlayingApp);
    expect(back && back->x == 10 && back->y == 900, "where they were put is kept in the file too");
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
        serial = speaker->serial;

        // Windows has them now: they are not boxes of their own.
        Discovery discovery = withApplications();
        discovery.outputs.push_back({"Speakers (Wolfe Music)", L"{wolfe-music}", false, serial});
        discovery.inputs.push_back({"Microphone (Wolfe Chat)", L"{wolfe-chat}", true, controller.node(chat)->serial});
        discovery.inputs[0].isDefault = false;
        discovery.sessions.push_back({"C:\\Players\\player.exe", "{wolfe-music}", false, true});
        discovery.sessions.push_back({"C:\\Chat\\chat.exe", "{wolfe-chat}", true, true});
        controller.apply(discovery);
        const std::string told = controller.takeNotice();
        expect(told.find("Wolfe Chat") != std::string::npos && told.find("default recording") != std::string::npos,
               "the user is told when Windows makes Wolfe's microphone the default");
        controller.apply(discovery);
        expect(controller.takeNotice().empty(), "once");
        expect(byLabel(controller, "Speakers (Wolfe Music)", NodeKind::Output) == nullptr &&
                   byLabel(controller, "Microphone (Wolfe Chat)", NodeKind::Input) == nullptr,
               "Wolfe's devices are not shown as ordinary devices");
        expect(controller.node(music)->present && controller.node(music)->endpoint == "{wolfe-music}",
               "they are found by their serial number");
        const int player = uidOf(controller, "player.exe", NodeKind::PlayingApp);
        const int recorder = uidOf(controller, "chat.exe", NodeKind::RecordingApp);
        expect(hasSystemLink(controller, player, music, true), "an application playing on Wolfe's speaker is shown on it");
        expect(hasSystemLink(controller, chat, recorder, true), "one recording from Wolfe's microphone likewise");
        expect(controller.showsInput(*controller.node(music)) && controller.showsOutput(*controller.node(chat)),
               "with pins to show that from");

        const int phones = uidOf(controller, kPhones, NodeKind::Output);
        const int mic = uidOf(controller, kMic, NodeKind::Input);
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
    int music = 0;
    for (const Node& node : reloaded.nodes()) {
        if (node.kind == NodeKind::WolfeSpeaker) music = node.uid;
    }
    expect(music && reloaded.node(music)->serial == serial && reloaded.node(music)->active,
           "Wolfe's speaker is back, with the same serial number, before Windows is even looked at");
    expect(reloaded.links().size() == 2, "and so are the links");
    reloaded.removeNode(music);
    expect(reloaded.node(music) == nullptr && reloaded.links().size() == 1, "removed, it takes its links with it");
}

void testLoops() {
    printf("loops through devices\n");
    const std::string document = tempFile("wolfe-test-loops.json");
    Controller controller(document, false);
    controller.apply(system());
    const int mic = uidOf(controller, kMic, NodeKind::Input);
    const int phones = uidOf(controller, kPhones, NodeKind::Output);
    const int cableIn = uidOf(controller, kCableIn, NodeKind::Output);
    const int cableOut = uidOf(controller, kCableOut, NodeKind::Input);
    const int played = controller.addLoopback(phones, 300, 600);
    const int effect = controller.addEffect("mix", 600, 600);

    expect(played && controller.node(played)->kind == NodeKind::Loopback && controller.node(played)->present,
           "a box for what is played on the headphones");
    expect(controller.addLoopback(phones, 0, 0) == played, "only one per device");
    expect(controller.addLoopback(mic, 0, 0) == 0, "and only for playback devices");
    expect(!controller.connect(played, phones).empty(), "it cannot go back into the same device");
    expect(controller.connect(played, effect).empty(), "it can go into an effect");
    expect(!controller.connect(effect, phones).empty(), "but not from there into the device either");
    expect(controller.connect(effect, cableIn).empty(), "into another device it can");
    expect(!controller.connect(cableOut, phones).empty(), "and then not back from the cable's far end");
    expect(!controller.connect(cableOut, cableIn).empty(), "one end of a cable cannot feed the other");
    expect(controller.connect(mic, phones).empty(), "the microphone to the headphones is fine");
    expect(controller.links().size() == 3, "the refused links left nothing behind");
    controller.save();

    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the saved file loads without complaints");
    reloaded.apply(system());
    int loopbacks = 0;
    for (const Node& node : reloaded.nodes()) loopbacks += node.kind == NodeKind::Loopback ? 1 : 0;
    expect(loopbacks == 1 && reloaded.links().size() == 3, "the box and its links are back");
}

void testOldFile() {
    printf("a file from the version that moved applications\n");
    const std::string source = tempFile("wolfe-test-old.json");
    writeFile(source, R"({
      "margin_ms": 4,
      "close_to_tray": true,
      "silent_device": "{fake-cable-in}",
      "nodes": [
        { "id": "out|{fake-phones}", "type": "output", "device": "{fake-phones}", "x": 620, "y": 40 },
        { "id": "app|C:\\Players\\player.exe", "type": "app", "process": "C:\\Players\\player.exe", "parked": true },
        { "id": "mon|out|{fake-phones}", "type": "loopback", "device": "{fake-phones}" },
        { "id": "out|{fake-cable-in}", "type": "output", "device": "{fake-cable-in}" }
      ],
      "links": [
        { "from": "app|C:\\Players\\player.exe", "to": "out|{fake-phones}" },
        { "from": "mon|out|{fake-phones}", "to": "out|{fake-cable-in}", "gain_db": -3 }
      ]
    })");
    const std::string document = tempFile("wolfe-test-old-copy.json");
    Controller controller(document, false);
    const std::string problems = controller.importFrom(source);
    expect(!problems.empty() && problems.find("a link uses") == std::string::npos,
           "the application is reported, and its link goes with it quietly");
    controller.apply(system());
    const int phones = uidOf(controller, kPhones, NodeKind::Output);
    const int played = uidOf(controller, kPhones, NodeKind::Loopback);
    const int cableIn = uidOf(controller, kCableIn, NodeKind::Output);
    expect(played && phones && played != phones, "what was taken from the headphones is a box of its own");
    expect(linkBetween(controller, played, cableIn) && controller.links()[0].gainDb == -3.0f, "with its link and gain");
}

bool onTopOfEachOther(const Node& a, const Node& b) { return std::fabs(a.x - b.x) < 190.0f && std::fabs(a.y - b.y) < 60.0f; }

void testPlacing() {
    printf("where boxes go\n");
    const std::string document = tempFile("wolfe-test-placing.json");
    Controller controller(document, false);
    std::string problem;
    controller.addWolfeDevice(false, "Wolfe Music", 0, 0, &problem);
    Discovery discovery = withApplications();
    controller.apply(discovery);

    const Node* player = byLabel(controller, "player.exe", NodeKind::PlayingApp);
    const Node* mic = byLabel(controller, kMic, NodeKind::Input);
    const Node* phones = byLabel(controller, kPhones, NodeKind::Output);
    const Node* recorder = byLabel(controller, "chat.exe", NodeKind::RecordingApp);
    expect(player && mic && phones && recorder, "everything is there");
    if (!player || !mic || !phones || !recorder) return;
    expect(player->x < mic->x && mic->x < phones->x && phones->x < recorder->x,
           "applications that play, sources, sinks, applications that record: left to right");
    bool overlap = false;
    for (const Node& a : controller.nodes()) {
        for (const Node& b : controller.nodes()) {
            if (&a != &b && controller.visible(a) && controller.visible(b) && onTopOfEachOther(a, b)) overlap = true;
        }
    }
    expect(!overlap, "no box covers another");
}

}  // namespace

int main() {
    testEditing();
    testHandWrittenFile();
    testApplications();
    testWolfeDevices();
    testLoops();
    testOldFile();
    testPlacing();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
