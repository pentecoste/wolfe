// imnodes' internals (used here to ask where a pin was drawn) need this before imgui.h.
#define IMGUI_DEFINE_MATH_OPERATORS
#include "selftest.h"

#include "imnodes.h"
#include "imnodes_internal.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace wolfe {

namespace {

// Where the canvas drew a pin (pins are numbered as in editor.cpp).
ImVec2 pinPosition(int nodeUid, bool output) {
    ImNodesEditorContext& editor = ImNodes::EditorContextGet();
    const int index = ImNodes::ObjectPoolFind(editor.Pins, nodeUid * 2 + (output ? 1 : 0));
    return index < 0 ? ImVec2(-1000.0f, -1000.0f) : editor.Pins.Pool[index].Pos;
}

bool anyMenuOpen() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

// A point on a link just before it reaches an input pin: links arrive level.
std::function<ImVec2()> justBefore(std::function<ImVec2()> pin) {
    return [pin] {
        const ImVec2 position = pin();
        return ImVec2(position.x - 14.0f * ImGui::GetStyle().FontScaleDpi, position.y);
    };
}

}  // namespace

SelfTest::SelfTest(Controller& controller, std::string reportPath)
    : controller_(controller), reportPath_(std::move(reportPath)) {
    const Place firstOut = [this] { return pinPosition(first_, true); };
    const Place secondIn = [this] { return pinPosition(second_, false); };
    const Place secondOut = [this] { return pinPosition(second_, true); };
    const Place thirdIn = [this] { return pinPosition(third_, false); };
    const Place onLink = [=] {
        const ImVec2 a = firstOut(), b = secondIn();
        return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    };
    const Place aLittleOff = [=] {
        const ImVec2 middle = onLink();
        return ImVec2(middle.x, middle.y + 12.0f);
    };
    const Place emptySpot = [=] {
        const ImVec2 middle = onLink();
        return ImVec2(middle.x, middle.y - 110.0f);
    };
    const Place onFirstTitle = [this] {
        const ImVec2 corner = ImNodes::GetNodeScreenSpacePos(first_);
        return ImVec2(corner.x + 40.0f, corner.y + 12.0f);
    };

    wait(20);  // let the window settle and the first look at the system arrive
    run([this] {
        // Work in a far corner of the canvas, clear of whatever the graph
        // file already has on it.
        const float corner = 5000.0f;
        ImNodes::EditorContextResetPanning(ImVec2(-corner, -corner));
        // Two gains side by side, at the same height so that the link
        // between them passes through the point halfway between the pins,
        // and a third one below.
        const float scale = ImGui::GetStyle().FontScaleDpi;
        first_ = controller_.addGain((corner + 150.0f) / scale, (corner + 200.0f) / scale);
        second_ = controller_.addGain((corner + 520.0f) / scale, (corner + 200.0f) / scale);
        third_ = controller_.addGain((corner + 520.0f) / scale, (corner + 480.0f) / scale);
    });
    wait(5);

    drag(firstOut, secondIn);
    check("dragging from an output pin to an input pin connects them", [this] { return hasLink(first_, second_); });

    click(onLink, 1);
    check("right-clicking a link opens its menu", [] { return anyMenuOpen(); });
    tapKey(ImGuiKey_Escape);
    check("Escape closes the menu", [] { return !anyMenuOpen(); });

    ctrlClick(onLink);
    check("Ctrl + click on a link removes it", [this] { return controller_.links().empty(); });

    drag(secondIn, firstOut);
    check("dragging the other way round makes the same link", [this] { return hasLink(first_, second_); });

    drag(firstOut, thirdIn);
    check("a second link from the same pin leaves the first one in place",
          [this] { return controller_.links().size() == 2 && linked(first_, second_) && linked(first_, third_); });
    drag(secondOut, thirdIn);
    check("several links can end at the same pin",
          [this] { return controller_.links().size() == 3 && linked(second_, third_); });
    run([this] {
        std::vector<int> extra;
        for (const Connection& link : controller_.links()) {
            if (link.to == third_) extra.push_back(link.uid);
        }
        for (int uid : extra) controller_.disconnect(uid);
    });
    wait(3);

    drag(justBefore(secondIn), thirdIn);
    check("dragging a link by one end to another pin moves it", [this] { return hasLink(first_, third_); });
    drag(justBefore(thirdIn), secondIn);
    check("and back again", [this] { return hasLink(first_, second_); });
    drag(onLink, aLittleOff);
    check("a link nudged and let go stays where it was", [this] { return hasLink(first_, second_); });

    click(onLink);
    check("a click on a link leaves it in place", [this] { return hasLink(first_, second_); });
    tapKey(ImGuiKey_Delete);
    check("and selects it: Delete removes it", [this] { return controller_.links().empty(); });

    drag(firstOut, secondIn);
    drag(onLink, emptySpot);
    check("a link pulled away and let go is removed", [this] { return controller_.links().empty(); });

    drag(firstOut, emptySpot);
    check("dropping a new link on nothing connects nothing", [this] { return controller_.links().empty(); });

    click(emptySpot, 1);
    check("right-clicking the canvas opens the Add menu", [] { return anyMenuOpen(); });
    tapKey(ImGuiKey_Escape);

    drag(firstOut, secondIn);
    click(onFirstTitle);
    tapKey(ImGuiKey_Delete);
    check("Delete removes the selected box and its links",
          [this] { return controller_.node(first_) == nullptr && controller_.links().empty(); });

    // The gain's value, between its two pins.
    const Place onValue = [=] {
        const ImVec2 in = secondIn(), out = secondOut();
        return ImVec2((in.x + out.x) * 0.5f, in.y);
    };
    const Place rightOfValue = [=] {
        const ImVec2 middle = onValue();
        return ImVec2(middle.x + 60.0f * ImGui::GetStyle().FontScaleDpi, middle.y);
    };
    auto where = std::make_shared<ImVec2>();
    run([this, where] { *where = ImVec2(controller_.node(second_)->x, controller_.node(second_)->y); });
    drag(onValue, rightOfValue);
    check("dragging a gain's value to the right raises it", [this] { return controller_.node(second_)->gainDb > 0.0f; });
    check("and leaves the box where it was", [this, where] {
        const Node* node = controller_.node(second_);
        return node->x == where->x && node->y == where->y;
    });

    auto gains = std::make_shared<int>(0);
    const auto countGains = [this] {
        int count = 0;
        for (const Node& node : controller_.nodes()) count += node.kind == NodeKind::Gain ? 1 : 0;
        return count;
    };
    run([gains, countGains] { *gains = countGains(); });
    moveTo(emptySpot);
    ctrlKey(ImGuiKey_A);
    check("Ctrl+A opens the search", [] { return anyMenuOpen(); });
    tapKey(ImGuiKey_Escape);
    check("Escape closes it", [] { return !anyMenuOpen(); });
    check("without adding anything", [gains, countGains] { return countGains() == *gains; });
    ctrlKey(ImGuiKey_A);
    wait(3);
    run([] { ImGui::GetIO().AddInputCharactersUTF8("gain"); });
    wait(3);
    tapKey(ImGuiKey_Enter);
    check("typing \"gain\" and Enter adds a gain", [gains, countGains] { return countGains() == *gains + 1; });
    check("and closes the search", [] { return !anyMenuOpen(); });

    run([this, countGains] {
        controller_.removeNode(second_);
        controller_.removeNode(third_);
        std::vector<int> added;
        for (const Node& node : controller_.nodes()) {
            if (node.kind == NodeKind::Gain) added.push_back(node.uid);
        }
        for (int uid : added) controller_.removeNode(uid);
        char line[80];
        snprintf(line, sizeof(line), failures_ ? "%d check(s) FAILED\n" : "all checks passed\n", failures_);
        report_ += line;
        if (FILE* file = fopen(reportPath_.c_str(), "wb")) {
            fputs(report_.c_str(), file);
            fclose(file);
        }
    });
}

bool SelfTest::step() {
    if (current_ >= steps_.size()) return false;
    if (steps_[current_]()) current_++;
    return true;
}

bool SelfTest::linked(int fromUid, int toUid) const {
    for (const Connection& link : controller_.links()) {
        if (link.from == fromUid && link.to == toUid) return true;
    }
    return false;
}

bool SelfTest::hasLink(int fromUid, int toUid) const { return linked(fromUid, toUid) && controller_.links().size() == 1; }

void SelfTest::wait(int frames) {
    auto left = std::make_shared<int>(frames);
    steps_.push_back([left] { return --*left <= 0; });
}

void SelfTest::waitSeconds(double seconds) {
    auto until = std::make_shared<double>(-1.0);
    steps_.push_back([until, seconds] {
        if (*until < 0) *until = nowSeconds() + seconds;
        return nowSeconds() >= *until;
    });
}

void SelfTest::run(std::function<void()> action) {
    steps_.push_back([action] {
        action();
        return true;
    });
}

void SelfTest::moveTo(Place where) {
    run([where] {
        const ImVec2 position = where();
        ImGui::GetIO().AddMousePosEvent(position.x, position.y);
    });
    wait(3);
}

void SelfTest::press(int button, bool down) {
    run([button, down] { ImGui::GetIO().AddMouseButtonEvent(button, down); });
    wait(3);
}

void SelfTest::click(Place where, int button) {
    moveTo(where);
    press(button, true);
    press(button, false);
}

void SelfTest::ctrlClick(Place where) {
    run([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); });
    wait(2);
    click(where);
    run([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); });
    wait(2);
}

// Presses on `from`, moves to `to` in small steps as a hand would, releases.
void SelfTest::drag(Place from, Place to) {
    moveTo(from);
    press(0, true);
    for (int i = 1; i <= 10; i++) {
        run([from, to, i] {
            const ImVec2 a = from(), b = to();
            const float t = i / 10.0f;
            ImGui::GetIO().AddMousePosEvent(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
        });
        wait(1);
    }
    wait(3);
    press(0, false);
}

void SelfTest::ctrlKey(ImGuiKey key) {
    run([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); });
    wait(2);
    tapKey(key);
    run([] { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false); });
    wait(2);
}

void SelfTest::tapKey(ImGuiKey key) {
    run([key] { ImGui::GetIO().AddKeyEvent(key, true); });
    wait(2);
    run([key] { ImGui::GetIO().AddKeyEvent(key, false); });
    wait(3);
}

void SelfTest::check(const char* what, std::function<bool()> condition) {
    run([this, what, condition] {
        const bool ok = condition();
        if (!ok) failures_++;
        report_ += std::string(ok ? "ok    " : "FAIL  ") + what + "\n";
    });
}

}  // namespace wolfe
