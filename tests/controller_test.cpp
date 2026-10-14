// Checks the logic behind the graphical editor without drawing anything and
// without touching real devices: the devices it is told about do not exist,
// so the engine only ever fails to open them. Names are made up for the same
// reason: a real one would open the real device.
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
    return toUtf8((std::filesystem::temp_directory_path() / name).wstring());
}

Discovery system() {
    Discovery discovery;
    discovery.inputs.push_back({"Test Microphone (Nowhere Audio)", L"{fake-mic}", true});
    discovery.outputs.push_back({"Test Cable Input (Nowhere Audio)", L"{fake-cable}", false});
    discovery.outputs.push_back({"Test Headphones (Nowhere Audio)", L"{fake-phones}", true});
    return discovery;
}

const Node* byLabel(const Controller& controller, const std::string& label) {
    for (const Node& node : controller.nodes()) {
        if (node.label == label) return &node;
    }
    return nullptr;
}

int countVisible(const Controller& controller) {
    int count = 0;
    for (const Node& node : controller.nodes()) count += controller.visible(node) ? 1 : 0;
    return count;
}

void testEditing() {
    printf("discovering devices, connecting and disconnecting\n");
    const std::string document = tempFile("wolfe-test-editing.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);
    controller.apply(system());

    const Node* mic = byLabel(controller, "Test Microphone (Nowhere Audio)");
    const Node* cable = byLabel(controller, "Test Cable Input (Nowhere Audio)");
    expect(controller.nodes().size() == 3 && mic && cable, "one box per device");
    expect(mic && !mic->active && cable && !cable->active, "a device that is only shown is not opened");
    if (!mic || !cable) return;
    const int micUid = mic->uid, cableUid = cable->uid;

    expect(controller.connect(micUid, cableUid).empty(), "microphone -> cable connects");
    expect(controller.node(micUid)->active && controller.node(cableUid)->active, "both are now in use");
    expect(!controller.connect(micUid, cableUid).empty(), "the same link twice is refused");
    expect(!controller.connect(cableUid, micUid).empty(), "an output cannot feed an input");
    expect(!controller.connect(micUid, micUid).empty(), "a node cannot feed itself");

    const int first = controller.addEffect("mix", 300, 40), second = controller.addEffect("rnnoise", 300, 200);
    expect(controller.connect(first, second).empty(), "effect -> effect connects");
    expect(!controller.connect(second, first).empty(), "a loop is refused");
    expect(controller.links().size() == 2, "the refused links left nothing behind");

    controller.setGainDb(controller.links()[0].uid, -6.0f);
    controller.moveNode(micUid, 123, 456);
    controller.save();

    controller.disconnect(controller.links()[0].uid);
    expect(!controller.node(micUid)->active && !controller.node(cableUid)->active, "disconnected devices are closed");
    controller.removeNode(first);
    expect(controller.node(first) == nullptr && controller.links().empty(), "removing an effect removes its links");

    printf("saving and loading\n");
    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the saved file loads without complaints");
    reloaded.apply(system());
    const Node* micAgain = byLabel(reloaded, "Test Microphone (Nowhere Audio)");
    expect(reloaded.links().size() == 2, "both links are back");
    expect(micAgain && micAgain->x == 123 && micAgain->y == 456, "the microphone is where it was put");
    expect(micAgain && micAgain->active, "linked nodes are in use right after loading");
    bool gainKept = false;
    for (const Connection& link : reloaded.links()) gainKept |= link.gainDb == -6.0f;
    expect(gainKept, "the gain is back");
    expect(reloaded.nodes().size() == 5, "no device appears twice");
}

void testHandWrittenFile() {
    printf("a file written by hand, with devices named by part of their name\n");
    const std::string source = tempFile("wolfe-test-source.json");
    {
        std::ofstream file(std::filesystem::path(toWide(source)));
        file << R"({
          // comments are allowed
          "nodes": [
            { "id": "mic",         "type": "input",   "device": "test microphone" },
            { "id": "denoise",     "type": "rnnoise" },
            { "id": "soundrio",    "type": "app",     "process": "\\WolfeTestApp\\" },
            { "id": "virtual_mic", "type": "output",  "device": "Test Cable" }
          ],
          "links": [
            { "from": "mic",      "to": "denoise" },
            { "from": "denoise",  "to": "virtual_mic" },
            { "from": "soundrio", "to": "virtual_mic", "gain_db": -3 }
          ]
        })";
    }
    const std::string document = tempFile("wolfe-test-imported.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);
    expect(controller.importFrom(source).empty(), "it is imported without complaints");
    expect(controller.links().size() == 3 && controller.nodes().size() == 4, "four nodes, three links");
    expect(countVisible(controller) == 4, "everything it connects is shown, plugged in or not");
    expect(byLabel(controller, "WolfeTestApp") != nullptr, "the application gets a readable name");

    Discovery discovery = system();
    discovery.running["\\WolfeTestApp\\"] = true;
    controller.apply(discovery);
    expect(controller.nodes().size() == 5, "only the headphones are new: the others were recognised");
    const Node* mic = byLabel(controller, "Test Microphone (Nowhere Audio)");
    expect(mic && mic->id == "mic" && mic->target == "{fake-mic}", "the microphone is now tied to the device itself");
    const Node* app = byLabel(controller, "WolfeTestApp");
    expect(app && app->present, "the application is reported as running");

    printf("devices and applications that come and go\n");
    discovery.apps.push_back("C:\\Games\\Thing\\thing.exe");
    controller.apply(discovery);
    const Node* thing = byLabel(controller, "thing.exe");
    expect(thing && thing->detail == "Games\\Thing" && controller.visible(*thing), "a playing application shows up");

    Discovery later = system();
    later.inputs.clear();
    later.running["\\WolfeTestApp\\"] = false;
    later.running["C:\\Games\\Thing\\thing.exe"] = false;
    controller.apply(later);
    expect(byLabel(controller, "thing.exe") == nullptr, "and is forgotten when it exits untouched");
    mic = byLabel(controller, "Test Microphone (Nowhere Audio)");
    expect(mic && !mic->present && controller.visible(*mic), "an unplugged device stays while it is connected");
    app = byLabel(controller, "WolfeTestApp");
    expect(app && !app->present && controller.visible(*app), "so does a closed application");
    expect(mic && controller.removable(*mic), "and can be removed by hand");
}

// What Windows has connected on its own must appear on the canvas too.
void testSystemLinks() {
    printf("connections that exist outside Wolfe\n");
    const std::string document = tempFile("wolfe-test-system.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);

    Discovery discovery = system();
    discovery.inputs.push_back({"Test Cable Output (Nowhere Audio Virtual Cable)", L"{fake-cable-out}", false});
    discovery.outputs.push_back({"Test Cable In (Nowhere Audio Virtual Cable)", L"{fake-cable-in}", false});
    const std::string player = "C:\\Games\\Thing\\thing.exe", chat = "C:\\Apps\\Chat\\chat.exe";
    discovery.apps = {player, chat};
    discovery.sessions.push_back({player, "{fake-phones}", false, true});      // playing on the headphones
    discovery.sessions.push_back({chat, "{fake-phones}", false, false});       // a stream open there, silent
    discovery.sessions.push_back({chat, "{fake-cable-out}", true, true});      // recording from the cable
    controller.apply(discovery);

    const Node* thing = byLabel(controller, "thing.exe");
    const Node* chatNode = byLabel(controller, "chat.exe");
    const Node* phones = byLabel(controller, "Test Headphones (Nowhere Audio)");
    const Node* cableIn = byLabel(controller, "Test Cable In (Nowhere Audio Virtual Cable)");
    const Node* cableOut = byLabel(controller, "Test Cable Output (Nowhere Audio Virtual Cable)");
    expect(thing && chatNode && phones && cableIn && cableOut, "every application and device has a box");
    if (!thing || !chatNode || !phones || !cableIn || !cableOut) return;

    auto has = [&](int from, int to, SystemLink::Kind kind, bool active) {
        for (const SystemLink& link : controller.systemLinks()) {
            if (link.from == from && link.to == to && link.kind == kind && link.active == active) return true;
        }
        return false;
    };
    expect(controller.systemLinks().size() == 4, "four connections are shown");
    expect(has(thing->uid, phones->uid, SystemLink::Plays, true), "the application playing on the headphones");
    expect(has(chatNode->uid, phones->uid, SystemLink::Plays, false), "the silent stream, marked as not flowing");
    expect(has(cableOut->uid, chatNode->uid, SystemLink::Records, true), "the application recording from the cable");
    expect(has(cableIn->uid, cableOut->uid, SystemLink::Cable, true), "the two ends of the virtual cable");
    expect(controller.links().empty(), "none of them is a link of Wolfe's");
    expect(controller.showsInput(*chatNode) && controller.showsOutput(*cableIn), "boxes get a pin to show them from");
    expect(!controller.showsInput(*thing), "and no pin where there is nothing to show");

    const int firstUid = controller.systemLinks()[0].uid;
    controller.apply(discovery);
    expect(controller.systemLinks()[0].uid == firstUid, "a connection that stays keeps its identity");

    expect(controller.connect(thing->uid, cableIn->uid).empty(), "audio can be sent to the cable an application records from");

    discovery.sessions.erase(discovery.sessions.begin());
    controller.apply(discovery);
    expect(controller.systemLinks().size() == 3, "a connection that ends disappears");
}

// What the controller asked of Windows, in place of asking it.
struct Asked {
    std::string app;
    bool recording;
    std::string device;
};
std::vector<Asked> g_asked;

AppMover recorder() {
    return [](const std::string& app, bool recording, const std::string& device) {
        g_asked.push_back({app, recording, device});
        return 1;
    };
}

bool askedOnly(const std::string& app, bool recording, const std::string& device) {
    return g_asked.size() == 1 && g_asked[0].app == app && g_asked[0].recording == recording && g_asked[0].device == device;
}

// The devices where `app` is shown as playing, e.g. "{fake-phones} {fake-cable}".
std::string playingOn(const Controller& controller, int app) {
    std::string result;
    for (const SystemLink& link : controller.systemLinks()) {
        if (link.kind != SystemLink::Plays || link.from != app) continue;
        result += (result.empty() ? "" : " ") + controller.node(link.to)->target;
    }
    return result;
}

int linkBetween(const Controller& controller, int from, int to) {
    for (const SystemLink& link : controller.systemLinks()) {
        if (link.from == from && link.to == to) return link.uid;
    }
    for (const Connection& link : controller.links()) {
        if (link.from == from && link.to == to) return link.uid;
    }
    return 0;
}

// The device an application plays on is a link like any other: it can be
// moved, removed and made again, and Windows is the one that carries it out.
void testMovingApplications() {
    printf("moving an application between playback devices\n");
    const std::string document = tempFile("wolfe-test-moving.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    const std::string player = "C:\\Games\\Thing\\thing.exe";
    g_asked.clear();

    Discovery discovery = system();
    discovery.apps = {player};
    discovery.running[player] = true;
    discovery.sessions = {{player, "{fake-phones}", false, true}};
    discovery.playsOn[player] = {"{fake-phones}", false};
    {
        Controller controller(document, false);
        controller.setAppMover(recorder());
        controller.update(100.0);
        controller.apply(discovery);
        const int thing = byLabel(controller, "thing.exe")->uid;
        const int phones = byLabel(controller, "Test Headphones (Nowhere Audio)")->uid;
        const int cable = byLabel(controller, "Test Cable Input (Nowhere Audio)")->uid;

        expect(!controller.disconnect(linkBetween(controller, thing, phones)).empty() && g_asked.empty(),
               "with no device to send it to, an application cannot be taken off its device");

        discovery.outputs.push_back({"Speakers (Steam Streaming Speakers)", L"{fake-silent}", false});
        controller.apply(discovery);
        const Node* silent = byLabel(controller, "Speakers (Steam Streaming Speakers)");
        expect(silent && controller.silentDevice() == silent->uid, "a device nobody listens to is found by itself");

        expect(controller.connect(thing, cable).empty() && controller.links().size() == 1 && g_asked.empty(),
               "a second device gets a copy, carried by Wolfe");
        expect(controller.connect(thing, cable, linkBetween(controller, thing, phones)).empty() &&
                   askedOnly(player, false, "{fake-cable}") && controller.links().size() == 1,
               "dragging its own link there asks Windows to move it");

        // Windows does it; the stream on the old device stays listed, stopped.
        discovery.sessions = {{player, "{fake-phones}", false, false}, {player, "{fake-cable}", false, true}};
        discovery.playsOn[player] = {"{fake-cable}", true};
        controller.update(100.5);
        controller.apply(discovery);
        expect(playingOn(controller, thing) == "{fake-cable}", "afterwards it is shown on the new device only");
        expect(controller.links().empty(), "and the copy, which would now be heard twice, is gone");
        expect(controller.node(thing)->ownOutput && controller.takeNotice().empty(), "and nothing needs saying");

        g_asked.clear();
        expect(controller.disconnect(linkBetween(controller, thing, cable)).empty() &&
                   askedOnly(player, false, "{fake-silent}") && controller.node(thing)->parked,
               "removing that link sends it to the silent device");
        discovery.sessions = {{player, "{fake-cable}", false, false}, {player, "{fake-silent}", false, true}};
        discovery.playsOn[player] = {"{fake-silent}", true};
        controller.update(101.0);
        controller.apply(discovery);
        expect(playingOn(controller, thing).empty() && controller.node(thing)->silenced, "where it is shown as connected to nothing");

        g_asked.clear();
        expect(controller.connect(thing, phones).empty() && askedOnly(player, false, "{fake-phones}") &&
                   controller.links().empty() && !controller.node(thing)->parked,
               "connecting it again gives it a device of its own, not a copy");

        // An application that does not follow: it keeps playing where it was.
        controller.update(106.0);
        controller.apply(discovery);
        expect(controller.takeNotice().find("still playing") != std::string::npos, "an application that does not move is reported");

        // Nothing flowing anywhere: only the device it would play on next.
        discovery.sessions = {{player, "{fake-cable}", false, false}, {player, "{fake-phones}", false, false}};
        discovery.playsOn[player] = {"{fake-phones}", true};
        controller.apply(discovery);
        expect(playingOn(controller, thing) == "{fake-phones}", "a stopped application is shown on the device it would use");
        discovery.sessions = {{player, "{fake-cable}", false, false}};
        controller.apply(discovery);
        expect(playingOn(controller, thing) == "{fake-phones}", "also before it has opened anything there");

        printf("copies next to the application's own link\n");
        discovery.sessions = {{player, "{fake-phones}", false, true}};
        controller.update(110.0);
        controller.apply(discovery);
        expect(controller.connect(thing, cable).empty() && controller.links().size() == 1, "a copy to a second device");
        // Windows starts playing it on that device by itself (changed from Windows' own settings, say).
        discovery.sessions = {{player, "{fake-cable}", false, true}};
        discovery.playsOn[player] = {"{fake-cable}", true};
        controller.apply(discovery);
        expect(controller.links().size() == 1 && controller.links()[0].suspended,
               "a copy onto the device Windows plays it on is paused, not lost");
        discovery.sessions = {{player, "{fake-phones}", false, true}};
        discovery.playsOn[player] = {"{fake-phones}", true};
        controller.apply(discovery);
        expect(controller.links().size() == 1 && !controller.links()[0].suspended, "and carries on when that is over");

        g_asked.clear();
        expect(controller.disconnect(linkBetween(controller, thing, phones)).empty() && askedOnly(player, false, "{fake-cable}") &&
                   !controller.node(thing)->parked && controller.links().size() == 1,
               "removing its own link asks Windows to play it where the copy goes");
        discovery.sessions = {{player, "{fake-phones}", false, false}, {player, "{fake-cable}", false, true}};
        discovery.playsOn[player] = {"{fake-cable}", true};
        controller.update(111.0);
        controller.apply(discovery);
        expect(controller.links().empty() && playingOn(controller, thing) == "{fake-cable}",
               "and the copy has become the application's own link");

        expect(controller.connect(thing, phones).empty() && controller.links().size() == 1, "a copy again, this time with a gain");
        controller.setGainDb(controller.links()[0].uid, -6.0f);
        g_asked.clear();
        expect(controller.disconnect(linkBetween(controller, thing, cable)).empty() && askedOnly(player, false, "{fake-silent}") &&
                   controller.node(thing)->parked && controller.links().size() == 1,
               "Windows' own link has no gain: that copy stays a copy, and the application leaves its device");

        printf("an application kept off every device, across restarts\n");
        controller.save();
        g_asked.clear();
    }
    expect(askedOnly(player, false, ""), "when Wolfe stops it is given back to Windows");

    g_asked.clear();
    Controller reloaded(document, false);
    reloaded.setAppMover(recorder());
    expect(reloaded.load().empty(), "the file loads");
    reloaded.update(200.0);
    reloaded.apply(discovery);
    const Node* thing = byLabel(reloaded, "thing.exe");
    expect(thing && thing->parked && askedOnly(player, false, "{fake-silent}"), "and when Wolfe starts again it is taken off again");
    if (thing) reloaded.giveBack(thing->uid);
    expect(thing && !thing->parked, "until the user gives it back");
}

// An application records from one device. Another device can be put in its
// place; anything else gets there through a virtual cable.
void testRecordingApplications() {
    printf("connecting things to an application that records\n");
    const std::string document = tempFile("wolfe-test-recording.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);
    controller.setAppMover(recorder());
    g_asked.clear();

    Discovery discovery = system();
    discovery.inputs.push_back({"Test Cable Output (Nowhere Audio Virtual Cable)", L"{fake-cable-out}", false});
    discovery.outputs.push_back({"Test Cable In (Nowhere Audio Virtual Cable)", L"{fake-cable-in}", false});
    const std::string player = "C:\\Games\\Thing\\thing.exe", chat = "C:\\Apps\\Chat\\chat.exe", other = "C:\\Apps\\Other\\other.exe";
    discovery.apps = {player, chat, other};
    discovery.sessions = {{player, "{fake-phones}", false, true}, {chat, "{fake-mic}", true, true}, {other, "{fake-mic}", true, true}};
    controller.update(100.0);
    controller.apply(discovery);

    const int thing = byLabel(controller, "thing.exe")->uid, chatUid = byLabel(controller, "chat.exe")->uid;
    const int otherUid = byLabel(controller, "other.exe")->uid;
    const int mic = byLabel(controller, "Test Microphone (Nowhere Audio)")->uid;
    const int cableIn = byLabel(controller, "Test Cable In (Nowhere Audio Virtual Cable)")->uid;
    const int cableOut = byLabel(controller, "Test Cable Output (Nowhere Audio Virtual Cable)")->uid;

    expect(!controller.disconnect(linkBetween(controller, mic, chatUid)).empty() && g_asked.empty(),
           "an application cannot be left recording from nothing");
    expect(!controller.connect(mic, chatUid).empty(), "nor connected twice to the device it records from");

    const int effect = controller.addEffect("mix", 300, 40);
    expect(controller.connect(effect, chatUid).empty() && askedOnly(chat, true, "{fake-cable-out}") &&
               linkBetween(controller, effect, cableIn) != 0,
           "an effect dragged onto it goes into a free cable, and it is told to record from the other end");

    discovery.sessions[1] = {chat, "{fake-cable-out}", true, true};
    controller.apply(discovery);
    g_asked.clear();
    expect(controller.connect(thing, chatUid).empty() && g_asked.empty() && linkBetween(controller, thing, cableIn) != 0,
           "anything further goes into the cable it is already on");

    expect(!controller.connect(effect, otherUid).empty() && g_asked.empty() && controller.links().size() == 2,
           "a second application finds that cable taken");
    expect(controller.connect(cableIn, otherUid).empty() && askedOnly(other, true, "{fake-cable-out}"),
           "but can be given the same cable, from either end");
    g_asked.clear();
    expect(controller.connect(mic, otherUid, linkBetween(controller, mic, otherUid)).empty() && g_asked.empty(),
           "putting a link back where it was changes nothing");
    expect(controller.connect(cableOut, otherUid, linkBetween(controller, mic, otherUid)).empty() &&
               askedOnly(other, true, "{fake-cable-out}"),
           "dragging its link to another recording device asks Windows for that device");
}

// What the controller asked of Windows about "Listen to this device".
struct Heard {
    std::string input;
    bool on;
    std::string output;
};
std::vector<Heard> g_heard;

bool heardAt(size_t index, const std::string& input, bool on, const std::string& output) {
    return index < g_heard.size() && g_heard[index].input == input && g_heard[index].on == on && g_heard[index].output == output;
}

// A recording device that Windows itself plays on a playback device is a link
// too: shown, moved, removed, and handed between Windows and Wolfe.
void testListening() {
    printf("a recording device that Windows plays on a playback device\n");
    const std::string document = tempFile("wolfe-test-listening.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);
    controller.setListenSetter([](const std::string& input, bool on, const std::string& output) {
        g_heard.push_back({input, on, output});
        return true;
    });
    g_heard.clear();

    Discovery discovery = system();
    discovery.inputs.push_back({"Test Cable Output (Nowhere Audio Virtual Cable)", L"{fake-cable-out}", false});
    discovery.outputs.push_back({"Test Cable In (Nowhere Audio Virtual Cable)", L"{fake-cable-in}", false});
    discovery.listens = {{"{fake-cable-out}", "{fake-phones}"}};
    controller.apply(discovery);
    const int mic = byLabel(controller, "Test Microphone (Nowhere Audio)")->uid;
    const int phones = byLabel(controller, "Test Headphones (Nowhere Audio)")->uid;
    const int other = byLabel(controller, "Test Cable Input (Nowhere Audio)")->uid;
    const int cableIn = byLabel(controller, "Test Cable In (Nowhere Audio Virtual Cable)")->uid;
    const int cableOut = byLabel(controller, "Test Cable Output (Nowhere Audio Virtual Cable)")->uid;

    const int heard = linkBetween(controller, cableOut, phones);
    expect(heard && controller.systemLink(heard) && controller.systemLink(heard)->kind == SystemLink::Listens,
           "it is shown as a link from the one to the other");
    expect(!controller.connect(cableOut, phones).empty() && controller.links().empty(), "Wolfe does not make the same link again");
    expect(!controller.connect(phones, cableIn).empty(), "a loop that would close through it is refused");

    expect(controller.connect(cableOut, other, heard).empty() && g_heard.size() == 1 &&
               heardAt(0, "{fake-cable-out}", true, "{fake-cable}"),
           "dragging it to another playback device asks Windows to play it there");
    g_heard.clear();
    expect(controller.connect(mic, phones, heard).empty() && g_heard.size() == 2 && heardAt(0, "{fake-cable-out}", false, "") &&
               heardAt(1, "{fake-mic}", true, "{fake-phones}"),
           "dragging its other end to another recording device stops the one and starts the other");
    g_heard.clear();
    expect(controller.disconnect(heard).empty() && g_heard.size() == 1 && heardAt(0, "{fake-cable-out}", false, ""),
           "removing it switches it off");

    g_heard.clear();
    expect(controller.canHandOver(heard) && controller.handOver(heard).empty() && g_heard.size() == 1 &&
               heardAt(0, "{fake-cable-out}", false, "") && controller.links().size() == 1,
           "it can be handed to Wolfe: a link of Wolfe's takes its place");
    controller.apply(discovery);  // Windows has not stopped yet
    expect(controller.links()[0].suspended, "which carries nothing until Windows has stopped");
    discovery.listens.clear();
    controller.apply(discovery);
    expect(!controller.links()[0].suspended && controller.systemLink(heard) == nullptr, "and then carries on alone");

    g_heard.clear();
    const int carried = controller.links()[0].uid;
    expect(controller.canHandOver(carried) && controller.handOver(carried).empty() && g_heard.size() == 1 &&
               heardAt(0, "{fake-cable-out}", true, "{fake-phones}") && controller.links().empty(),
           "and a link of Wolfe's between two devices can be handed to Windows");

    controller.setListenSetter([](const std::string&, bool, const std::string&) { return false; });
    discovery.listens = {{"{fake-cable-out}", "{fake-phones}"}};
    controller.apply(discovery);
    expect(!controller.disconnect(linkBetween(controller, cableOut, phones)).empty(), "a refusal by Windows is reported");
}

// One source into several things, several sources into one.
void testFanOut() {
    printf("more than one link at a pin\n");
    const std::string document = tempFile("wolfe-test-fanout.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, false);
    controller.apply(system());
    const int mic = byLabel(controller, "Test Microphone (Nowhere Audio)")->uid;
    const int phones = byLabel(controller, "Test Headphones (Nowhere Audio)")->uid;
    const int cable = byLabel(controller, "Test Cable Input (Nowhere Audio)")->uid;
    const int effect = controller.addEffect("mix", 300, 40);

    expect(controller.connect(mic, cable).empty() && controller.connect(mic, phones).empty() &&
               controller.connect(mic, effect).empty() && controller.links().size() == 3,
           "one source feeds two devices and an effect");
    expect(controller.connect(effect, cable).empty() && controller.links().size() == 4, "and a device takes two sources");
    expect(controller.connect(mic, phones, linkBetween(controller, mic, cable)).find("already") != std::string::npos &&
               controller.links().size() == 4,
           "a link moved onto a pair that is already connected goes back where it was");
}

// What is played on a device can be taken from it like from any other source.
void testMonitors() {
    printf("taking what is played on a playback device\n");
    const std::string document = tempFile("wolfe-test-monitor.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Discovery discovery = system();
    discovery.inputs.push_back({"Test Cable Output (Nowhere Audio Virtual Cable)", L"{fake-cable-out}", false});
    discovery.outputs.push_back({"Test Cable In (Nowhere Audio Virtual Cable)", L"{fake-cable-in}", false});
    {
        Controller controller(document, false);
        controller.apply(discovery);
        const int phones = byLabel(controller, "Test Headphones (Nowhere Audio)")->uid;
        const int cable = byLabel(controller, "Test Cable Input (Nowhere Audio)")->uid;
        const int cableIn = byLabel(controller, "Test Cable In (Nowhere Audio Virtual Cable)")->uid;
        const int cableOut = byLabel(controller, "Test Cable Output (Nowhere Audio Virtual Cable)")->uid;
        const int effect = controller.addEffect("mix", 300, 40);

        expect(controller.connect(phones, effect).empty(), "playback device -> effect connects");
        expect(controller.node(phones)->monitoring && !controller.node(phones)->active, "the device is listened to, not played on");
        expect(controller.connect(effect, cable).empty(), "and on to another device");
        expect(!controller.connect(effect, phones).empty(), "sending it back into the same device is refused");
        expect(!controller.connect(cable, phones).empty(), "also the long way round");
        expect(!controller.connect(phones, phones).empty(), "and directly");
        expect(!controller.connect(cableOut, cableIn).empty(), "one end of a cable cannot feed the other");
        expect(controller.links().size() == 2, "the refused links left nothing behind");

        controller.disconnect(linkBetween(controller, phones, effect));
        expect(!controller.node(phones)->monitoring, "with the link gone the device is left alone");
        controller.connect(phones, effect);
        controller.setGainDb(linkBetween(controller, phones, effect), -9.0f);
    }  // saved here
    Controller reloaded(document, false);
    expect(reloaded.load().empty(), "the saved file loads without complaints");
    reloaded.apply(discovery);
    const Node* phones = byLabel(reloaded, "Test Headphones (Nowhere Audio)");
    expect(reloaded.links().size() == 2 && phones && phones->monitoring, "the link from the device is back");
    bool gainKept = false;
    for (const Connection& link : reloaded.links()) gainKept |= phones && link.from == phones->uid && link.gainDb == -9.0f;
    expect(gainKept, "with its gain");
    int boxes = 0;
    for (const Node& node : reloaded.nodes()) boxes += node.kind == NodeKind::Loopback ? 1 : 0;
    expect(boxes == 0, "and without a separate box for the monitor");
}

bool onTopOfEachOther(const Node& a, const Node& b) { return std::fabs(a.x - b.x) < 190.0f && std::fabs(a.y - b.y) < 60.0f; }

// A box remembers its place also while it is off the canvas, and another one
// can be given the same place meanwhile. Seen for real: two applications
// drawn exactly on top of each other, and every drag from the pin of the one
// in view acting on the one underneath.
void testPlacing() {
    printf("boxes that were left in the same place\n");
    const std::string source = tempFile("wolfe-test-stacked-source.json");
    {
        std::ofstream file(std::filesystem::path(toWide(source)));
        file << R"({
          "nodes": [
            { "id": "one",   "type": "app", "process": "C:\\Apps\\One\\one.exe",     "x": 40, "y": 138 },
            { "id": "two",   "type": "app", "process": "C:\\Apps\\Two\\two.exe",     "x": 40, "y": 138 },
            { "id": "three", "type": "app", "process": "C:\\Apps\\Three\\three.exe", "x": 40, "y": 138 }
          ],
          "links": []
        })";
    }
    const std::string document = tempFile("wolfe-test-stacked.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    const std::string one = "C:\\Apps\\One\\one.exe", two = "C:\\Apps\\Two\\two.exe", three = "C:\\Apps\\Three\\three.exe";
    Controller controller(document, false);
    expect(controller.importFrom(source).empty(), "the file loads");

    Discovery discovery = system();
    discovery.running[one] = discovery.running[two] = true;
    discovery.running[three] = false;
    controller.apply(discovery);
    const Node* a = byLabel(controller, "one.exe");
    const Node* b = byLabel(controller, "two.exe");
    const Node* c = byLabel(controller, "three.exe");
    if (!a || !b || !c) return expect(false, "the three applications are there");
    expect(controller.visible(*a) && controller.visible(*b) && !onTopOfEachOther(*a, *b), "two boxes on one spot are pulled apart");
    expect(a->x == 40 && a->y == 138, "the first one stays where it was");

    discovery.running[three] = true;
    controller.apply(discovery);
    expect(!onTopOfEachOther(*c, *a) && !onTopOfEachOther(*c, *b), "one that appears later does not land on either");

    // Away and back, with its place still free: nothing changes.
    controller.moveNode(b->uid, 700, 500);
    discovery.running[two] = false;
    controller.apply(discovery);
    discovery.running[two] = true;
    controller.apply(discovery);
    expect(b->x == 700 && b->y == 500, "a box whose place is free comes back to it");

    // What the user does with boxes that are both in view is left alone.
    controller.moveNode(c->uid, a->x + 10, a->y + 10);
    controller.apply(discovery);
    expect(c->x == a->x + 10 && c->y == a->y + 10, "boxes overlapped by hand stay as they were put");
}

// Not a check of logic but of Windows: moves a real application around.
//   wolfe-controllertest live <part of the application's name> <first device> <second device>
// The application should be playing something inaudible, and following the
// default device. Nothing is ever linked in Wolfe, so no device is opened.
int live(const std::string& appName, const std::string& first, const std::string& second) {
    const std::string document = tempFile("wolfe-test-live.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, true);
    const auto settle = [&](double seconds) {
        for (const double end = nowSeconds() + seconds; nowSeconds() < end; Sleep(50)) controller.update(nowSeconds());
    };
    const auto find = [&](NodeKind kind, const std::string& part) -> const Node* {
        for (const Node& node : controller.nodes()) {
            if (node.kind == kind && node.present && toLower(node.label).find(toLower(part)) != std::string::npos) return &node;
        }
        return nullptr;
    };
    const auto where = [&](int app) {
        std::string result;
        for (const SystemLink& link : controller.systemLinks()) {
            if (link.kind != SystemLink::Plays || link.from != app) continue;
            result += (result.empty() ? "" : " + ") + controller.node(link.to)->label + (link.active ? "" : " (stopped)");
        }
        return result.empty() ? std::string("nothing") : result;
    };
    const auto said = [&] {
        for (std::string text = controller.takeNotice(); !text.empty(); text = controller.takeNotice()) printf("    Wolfe says: %s\n", text.c_str());
    };

    settle(2.5);
    const Node* app = find(NodeKind::App, appName);
    const Node* a = find(NodeKind::Output, first);
    const Node* b = find(NodeKind::Output, second);
    const Node* silent = controller.node(controller.silentDevice());
    if (!app || !a || !b || !silent) {
        printf("missing: application %d, first device %d, second device %d, silent device %d\n", !!app, !!a, !!b, !!silent);
        return 1;
    }
    const int appUid = app->uid, aUid = a->uid, bUid = b->uid;
    printf("silent device: %s\n", silent->label.c_str());
    printf("at the start: %s is connected to %s\n", app->label.c_str(), where(appUid).c_str());

    printf("removing that link\n");
    std::string problem = controller.disconnectAll(appUid);
    settle(1.5);
    said();
    expect(problem.empty() && where(appUid) == "nothing" && controller.node(appUid)->silenced, "it is connected to nothing");

    printf("connecting it to %s\n", controller.node(aUid)->label.c_str());
    problem = controller.connect(appUid, aUid);
    settle(1.5);
    said();
    expect(problem.empty() && where(appUid) == controller.node(aUid)->label && controller.links().empty(),
           "Windows plays it there, with no link of Wolfe's");

    printf("dragging that link to %s\n", controller.node(bUid)->label.c_str());
    problem = controller.connect(appUid, bUid, linkBetween(controller, appUid, aUid));
    settle(1.5);
    said();
    expect(problem.empty() && where(appUid) == controller.node(bUid)->label, "Windows plays it there instead");
    printf("at the end: %s is connected to %s\n", controller.node(appUid)->label.c_str(), where(appUid).c_str());
    return g_failures ? 1 : 0;
}

// The same for recording:
//   wolfe-controllertest live-recording <part of the application's name> <recording device>
// The application should be recording from some other device. One that does
// not follow the default device stays where it is, and Wolfe has to say so.
int liveRecording(const std::string& appName, const std::string& deviceName) {
    const std::string document = tempFile("wolfe-test-live.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, true);
    const auto settle = [&](double seconds) {
        for (const double end = nowSeconds() + seconds; nowSeconds() < end; Sleep(50)) controller.update(nowSeconds());
    };
    const auto find = [&](NodeKind kind, const std::string& part) -> const Node* {
        for (const Node& node : controller.nodes()) {
            if (node.kind == kind && node.present && toLower(node.label).find(toLower(part)) != std::string::npos) return &node;
        }
        return nullptr;
    };
    const auto where = [&](int app) {
        std::string result;
        for (const SystemLink& link : controller.systemLinks()) {
            if (link.kind == SystemLink::Records && link.to == app) result += (result.empty() ? "" : " + ") + controller.node(link.from)->label;
        }
        return result.empty() ? std::string("nothing") : result;
    };

    settle(2.5);
    const Node* app = find(NodeKind::App, appName);
    const Node* device = find(NodeKind::Input, deviceName);
    if (!app || !device) {
        printf("missing: application %d, device %d\n", !!app, !!device);
        return 1;
    }
    const int appUid = app->uid;
    printf("at the start: %s is recording from %s\n", app->label.c_str(), where(appUid).c_str());
    expect(controller.showsInput(*app), "it has a pin to drop things on");
    printf("dragging %s onto it\n", device->label.c_str());
    const std::string problem = controller.connect(device->uid, appUid);
    settle(6.0);
    expect(problem.empty(), "the request is accepted");
    const std::string told = controller.takeNotice();
    printf("    Wolfe says: %s\n", told.empty() ? "(nothing)" : told.c_str());
    printf("afterwards: it is recording from %s\n", where(appUid).c_str());
    return g_failures ? 1 : 0;
}

// And for "Listen to this device":
//   wolfe-controllertest live-listening <recording device> <first playback device> <second playback device>
// Use devices nobody listens to: the recording device really is played on them, by Wolfe and by Windows in turn.
int liveListening(const std::string& inputName, const std::string& first, const std::string& second) {
    const std::string document = tempFile("wolfe-test-live.json");
    std::filesystem::remove(std::filesystem::path(toWide(document)));
    Controller controller(document, true);
    const auto settle = [&](double seconds) {
        for (const double end = nowSeconds() + seconds; nowSeconds() < end; Sleep(50)) controller.update(nowSeconds());
    };
    const auto find = [&](NodeKind kind, const std::string& part) -> const Node* {
        for (const Node& node : controller.nodes()) {
            if (node.kind == kind && node.present && toLower(node.label).find(toLower(part)) != std::string::npos) return &node;
        }
        return nullptr;
    };
    settle(2.5);
    const Node* input = find(NodeKind::Input, inputName);
    const Node* a = find(NodeKind::Output, first);
    const Node* b = find(NodeKind::Output, second);
    if (!input || !a || !b) {
        printf("missing: recording device %d, first %d, second %d\n", !!input, !!a, !!b);
        return 1;
    }
    const int in = input->uid, aUid = a->uid, bUid = b->uid;
    // Whether Windows is playing the recording device on that playback device (0: on any).
    const auto listening = [&](int output) {
        for (const SystemLink& link : controller.systemLinks()) {
            if (link.kind == SystemLink::Listens && link.from == in && (output == 0 || link.to == output)) return true;
        }
        return false;
    };
    const auto wolfeLink = [&](int output) {
        for (const Connection& link : controller.links()) {
            if (link.from == in && link.to == output) return link.uid;
        }
        return 0;
    };
    expect(!listening(0), "at the start Windows is not playing it anywhere");

    std::string problem = controller.connect(in, aUid);
    settle(1.0);
    expect(problem.empty() && wolfeLink(aUid) && !listening(0), "a link drawn in Wolfe is carried by Wolfe");

    problem = controller.handOver(wolfeLink(aUid));
    settle(1.5);
    expect(problem.empty() && listening(aUid) && controller.links().empty(), "handed over, Windows carries it and Wolfe does not");

    problem = controller.connect(in, bUid, linkBetween(controller, in, aUid));
    settle(1.5);
    expect(problem.empty() && listening(bUid) && !listening(aUid), "dragged to the other device, Windows plays it there instead");

    problem = controller.handOver(linkBetween(controller, in, bUid));
    settle(1.5);
    expect(problem.empty() && !listening(0) && wolfeLink(bUid) && !controller.links()[0].suspended,
           "handed back, Wolfe carries it and Windows has stopped");

    problem = controller.handOver(wolfeLink(bUid));
    settle(1.5);
    expect(problem.empty() && listening(bUid), "to Windows once more");
    problem = controller.disconnect(linkBetween(controller, in, bUid));
    settle(1.5);
    expect(problem.empty() && !listening(0) && controller.links().empty(), "removed, nothing carries it any more");
    for (std::string text = controller.takeNotice(); !text.empty(); text = controller.takeNotice()) printf("    Wolfe says: %s\n", text.c_str());
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    ComApartment com;
    if (argc >= 5 && std::string(argv[1]) == "live") return live(argv[2], argv[3], argv[4]);
    if (argc >= 5 && std::string(argv[1]) == "live-listening") return liveListening(argv[2], argv[3], argv[4]);
    if (argc >= 4 && std::string(argv[1]) == "live-recording") return liveRecording(argv[2], argv[3]);
    testEditing();
    testHandWrittenFile();
    testSystemLinks();
    testMovingApplications();
    testRecordingApplications();
    testListening();
    testFanOut();
    testMonitors();
    testPlacing();
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
