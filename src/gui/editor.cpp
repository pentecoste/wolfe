#include "editor.h"

#include "shell.h"

#include "imnodes.h"

#include <cmath>
#include <vector>

namespace wolfe {

namespace {

// Canvas ids: a node uses its uid, its input pin uid * 2, its output pin
// uid * 2 + 1, a link its own uid.
int inputPin(int nodeUid) { return nodeUid * 2; }
int outputPin(int nodeUid) { return nodeUid * 2 + 1; }
int nodeOfPin(int pin) { return pin / 2; }
bool isOutputPin(int pin) { return pin % 2 == 1; }

const float kNodeWidth = 190.0f;

ImU32 titleColour(const Node& node, float brighten) {
    float r, g, b;
    switch (node.kind) {
        case NodeKind::Input: r = 0.20f, g = 0.42f, b = 0.72f; break;     // blue
        case NodeKind::App: r = 0.45f, g = 0.33f, b = 0.70f; break;       // violet
        case NodeKind::Loopback: r = 0.16f, g = 0.50f, b = 0.52f; break;  // teal
        case NodeKind::Effect: r = 0.72f, g = 0.45f, b = 0.16f; break;    // amber
        default: r = 0.22f, g = 0.56f, b = 0.32f; break;                  // green
    }
    if (!node.present) r = g = b = 0.30f;
    r = std::min(1.0f, r + brighten), g = std::min(1.0f, g + brighten), b = std::min(1.0f, b + brighten);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

// Windows calls a device "Name (Adapter)"; the two halves read better on two lines.
void splitDeviceName(const std::string& full, std::string& name, std::string& adapter) {
    size_t open = full.rfind(" (");
    if (open != std::string::npos && !full.empty() && full.back() == ')') {
        name = full.substr(0, open);
        adapter = full.substr(open + 2, full.size() - open - 3);
    } else {
        name = full;
        adapter.clear();
    }
}

// Shortens text that would be wider than `width`, without cutting a UTF-8 character in half.
std::string fit(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string shortened = text;
    while (!shortened.empty()) {
        do {
            shortened.pop_back();
        } while (!shortened.empty() && ((unsigned char)shortened.back() & 0xC0) == 0x80);
        if (ImGui::CalcTextSize((shortened + "...").c_str()).x <= width) break;
    }
    return shortened + "...";
}

// A level meter: -60 dB at the left edge, 0 dB at the right.
void drawMeter(float level, float width, float height, bool enabled) {
    ImGui::Dummy(ImVec2(width, height));
    const ImVec2 from = ImGui::GetItemRectMin(), to = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(from, to, IM_COL32(0, 0, 0, enabled ? 110 : 50), height * 0.5f);
    if (!enabled || level <= 0.001f) return;
    const float db = 20.0f * std::log10(level);
    const float filled = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
    const ImU32 colour = db > -1.0f ? IM_COL32(230, 80, 70, 255) : db > -12.0f ? IM_COL32(230, 200, 80, 255) : IM_COL32(90, 200, 120, 255);
    draw->AddRectFilled(from, ImVec2(from.x + (to.x - from.x) * filled, to.y), colour, height * 0.5f);
}

}  // namespace

Editor::Editor(Controller& controller, float scale) : controller_(controller), scale_(scale) {
    ImNodesStyle& style = ImNodes::GetStyle();
    style.GridSpacing *= scale;
    style.NodeCornerRounding = 6.0f * scale;
    style.NodePadding = ImVec2(10.0f * scale, 7.0f * scale);
    style.NodeBorderThickness *= scale;
    style.LinkThickness = 3.0f * scale;
    style.LinkHoverDistance = 8.0f * scale;
    style.PinCircleRadius = 5.0f * scale;
    style.PinHoverRadius = 11.0f * scale;
    style.PinLineThickness *= scale;
    style.MiniMapPadding = ImVec2(8.0f * scale, 8.0f * scale);
    style.Colors[ImNodesCol_GridBackground] = IM_COL32(24, 26, 31, 255);
    style.Colors[ImNodesCol_GridLine] = IM_COL32(40, 43, 51, 255);
    style.Colors[ImNodesCol_NodeBackground] = IM_COL32(44, 47, 56, 255);
    style.Colors[ImNodesCol_NodeBackgroundHovered] = IM_COL32(52, 56, 66, 255);
    style.Colors[ImNodesCol_NodeBackgroundSelected] = IM_COL32(56, 60, 72, 255);
    style.Colors[ImNodesCol_Link] = IM_COL32(120, 200, 255, 255);
    style.Colors[ImNodesCol_LinkHovered] = IM_COL32(180, 225, 255, 255);
    style.Colors[ImNodesCol_LinkSelected] = IM_COL32(255, 255, 255, 255);

    // Without a middle button: Alt + drag moves the view.
    ImNodes::GetIO().EmulateThreeButtonMouse.Modifier = &ImGui::GetIO().KeyAlt;
    // The canvas can take a link off its pin when it is clicked "with a
    // modifier held". Here the modifier is not a key but a decision made
    // every frame: see drawCanvas().
    ImNodes::GetIO().LinkDetachWithModifierClick.Modifier = &grabLinks_;
}

void Editor::notify(const std::string& message) {
    notice_ = message;
    while (!notice_.empty() && notice_.back() == '\n') notice_.pop_back();
    noticeUntil_ = now_ + 8.0;
}

void Editor::draw(double now) {
    now_ = now;
    const std::string news = controller_.takeNotice();
    if (!news.empty()) notify(news);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Wolfe", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleVar(3);

    drawMenuBar();

    // While Wolfe carries no audio at all, say so and say what to do.
    if (controller_.links().empty()) {
        const char* text = !anythingShown_ ? "Looking for audio devices..."
                                            : "Wolfe is not routing anything yet. Drag from a pin on the right of a box to a "
                                              "pin on the left of another, or use Wolfe > Import a graph file.";
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.21f, 0.30f, 1.0f));
        ImGui::BeginChild("banner", ImVec2(0, ImGui::GetFrameHeight() + 4.0f * scale_));
        ImGui::SetCursorPos(ImVec2(12.0f * scale_, 2.0f * scale_));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    const float statusHeight = ImGui::GetFrameHeightWithSpacing() + 2.0f * scale_;
    ImGui::BeginChild("canvas", ImVec2(0, -statusHeight));
    canvasCentre_ = ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f,
                           ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.4f);
    drawCanvas();
    drawMenus();
    ImGui::EndChild();

    drawStatusLine();
    ImGui::End();
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * scale_);
    if (ImGui::BeginMenu("Wolfe")) {
        if (ImGui::MenuItem("Import a graph file...")) {
            const std::string path = askForGraphFile(window);
            if (!path.empty()) {
                const std::string problems = controller_.importFrom(path);
                notify(problems.empty() ? "Imported." : problems);
            }
        }
        // Where an application goes when it is taken off every playback device.
        if (ImGui::BeginMenu("Silent device")) {
            const int silent = controller_.silentDevice();
            int chosen = -1;
            for (const Node& node : controller_.nodes()) {
                if (node.kind != NodeKind::Output || !node.present) continue;
                if (ImGui::MenuItem(node.label.c_str(), nullptr, node.uid == silent)) chosen = node.uid;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("None", nullptr, silent == 0)) chosen = 0;
            if (chosen >= 0) controller_.setSilentDevice(chosen);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        const bool autostart = startsWithWindows();
        if (ImGui::MenuItem("Start with Windows", nullptr, autostart)) setStartsWithWindows(!autostart);
        const bool tray = controller_.closeToTray();
        if (ImGui::MenuItem("Keep running when the window is closed", nullptr, tray)) controller_.setCloseToTray(!tray);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) exitRequested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        addItems(canvasCentre_);
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

// The things a user can put on the canvas; devices and applications appear by themselves.
void Editor::addItems(ImVec2 screenPosition) {
    if (ImGui::MenuItem("Noise suppression")) addEffectAt("rnnoise", screenPosition);
    if (ImGui::MenuItem("Mix")) addEffectAt("mix", screenPosition);
}

void Editor::addEffectAt(const std::string& type, ImVec2 screenPosition) {
    const int uid = controller_.addEffect(type, 0, 0);
    if (!uid) return;
    // The canvas converts the screen position; the controller learns it when
    // positions are read back at the end of the frame.
    ImNodes::SetNodeScreenSpacePos(uid, screenPosition);
    placed_.insert(uid);
}

void Editor::drawNode(const Node& node) {
    const float width = kNodeWidth * scale_;
    const bool trouble = node.used() && !node.healthy;

    ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColour(node, 0.0f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColour(node, 0.08f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColour(node, 0.14f));
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, trouble ? IM_COL32(235, 150, 60, 255) : IM_COL32(18, 19, 23, 255));
    ImNodes::BeginNode(node.uid);

    std::string title = node.label, second = node.detail;
    if (node.kind == NodeKind::Input || node.kind == NodeKind::Output) splitDeviceName(node.label, title, second);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(fit(title, width).c_str());
    ImNodes::EndNodeTitleBar();
    if (!second.empty()) ImGui::TextUnformatted(fit(second, width).c_str());

    // A few words saying what it is doing.
    std::string state;
    ImVec4 colour(0.55f, 0.57f, 0.62f, 1.0f);
    if (!node.present) {
        state = node.kind == NodeKind::App ? "not running" : "not plugged in";
    } else if (!node.used()) {
        // Not part of anything made in Wolfe; Windows may still be using it.
        // (Being one end of a cable does not by itself mean that anything passes through.)
        bool flowing = false, connected = false;
        for (const SystemLink& link : controller_.systemLinks()) {
            if (link.kind == SystemLink::Cable || (link.from != node.uid && link.to != node.uid)) continue;
            connected = true;
            flowing |= link.active;
        }
        state = flowing ? "in use" : "idle";
        if (flowing) colour = ImVec4(0.55f, 0.70f, 0.88f, 1.0f);
        if (node.kind == NodeKind::App && (node.parked || node.silenced) && !connected) state = "heard nowhere";
        if (node.kind == NodeKind::Output && node.uid == controller_.silentDevice() && !flowing) state = "silent device";
    } else if (node.healthy) {
        colour = ImVec4(0.45f, 0.85f, 0.55f, 1.0f);
        // "capturing java.exe (pid 9312)" -> which process was picked.
        state = node.kind == NodeKind::App ? node.status.substr(std::min(node.status.size(), (size_t)10)) : "live";
        if (node.kind == NodeKind::Effect) state = "on";
    } else {
        colour = ImVec4(0.95f, 0.65f, 0.30f, 1.0f);
        state = node.status;
    }

    // The pins sit on that line: where audio goes in on the left, where it
    // comes out on the right. A side has a pin when Wolfe can link there, or
    // when there is a connection made outside Wolfe to show.
    const bool pinIn = controller_.showsInput(node), pinOut = controller_.showsOutput(node);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (pinIn && pinOut) {
        const float in = ImGui::CalcTextSize("in").x, out = ImGui::CalcTextSize("out").x;
        const float text = std::min(ImGui::CalcTextSize(state.c_str()).x, width - in - out - 2.0f * spacing);
        ImNodes::BeginInputAttribute(inputPin(node.uid));
        ImGui::TextUnformatted("in");
        ImNodes::EndInputAttribute();
        ImGui::SameLine();
        const float gap = (width - in - out - text - 2.0f * spacing) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
        ImGui::TextColored(colour, "%s", fit(state, text + 1.0f).c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
        ImNodes::BeginOutputAttribute(outputPin(node.uid));
        ImGui::TextUnformatted("out");
        ImNodes::EndOutputAttribute();
    } else if (pinIn) {
        ImNodes::BeginInputAttribute(inputPin(node.uid));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextColored(colour, "%s", state.c_str());
        ImGui::PopTextWrapPos();
        ImNodes::EndInputAttribute();
    } else {
        ImNodes::BeginOutputAttribute(outputPin(node.uid));
        // Right-aligned when it fits on one line, wrapped from the left when it does not.
        const float text = ImGui::CalcTextSize(state.c_str()).x;
        if (text < width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width - text);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (text < width ? text + 1.0f : width));
        ImGui::TextColored(colour, "%s", state.c_str());
        ImGui::PopTextWrapPos();
        ImNodes::EndOutputAttribute();
    }
    if (node.kind == NodeKind::Effect) ImGui::Dummy(ImVec2(width, 1.0f));
    else drawMeter(node.level, width, 5.0f * scale_, node.present);

    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

void Editor::drawCanvas() {
    // A press on a pin starts a new link, however many already end there. A
    // press anywhere else on a link takes hold of the end nearer to the
    // mouse. (To the canvas a link is under the mouse also when the pin it
    // ends at is, so over a pin taking hold is switched off; Ctrl forces it.)
    grabLinks_ = ImGui::GetIO().KeyCtrl || !overPin_;

    ImNodes::BeginNodeEditor();

    int shown = 0;
    for (const Node& node : controller_.nodes()) {
        if (!controller_.visible(node)) {
            placed_.erase(node.uid);
            continue;
        }
        if (!placed_.count(node.uid) && node.hasPosition) {
            ImNodes::SetNodeGridSpacePos(node.uid, ImVec2(node.x * scale_, node.y * scale_));
            placed_.insert(node.uid);
        }
        drawNode(node);
        shown++;
    }
    // Links made in Wolfe, bright.
    // (A link that was pulled off its pin is in the user's hand, not here.)
    for (const Connection& link : controller_.links()) {
        if (link.uid == heldLink_) continue;
        // One that is paused (Windows already plays the same thing there) is drawn washed out.
        if (link.suspended) ImNodes::PushColorStyle(ImNodesCol_Link, IM_COL32(120, 200, 255, 80));
        ImNodes::Link(link.uid, outputPin(link.from), inputPin(link.to));
        if (link.suspended) ImNodes::PopColorStyle();
    }
    // The ones Windows makes, pale: paler still when nothing is flowing.
    for (const SystemLink& link : controller_.systemLinks()) {
        const Node* from = controller_.node(link.from);
        const Node* to = controller_.node(link.to);
        if (!from || !to || !controller_.visible(*from) || !controller_.visible(*to) || link.uid == heldLink_) continue;
        ImNodes::PushColorStyle(ImNodesCol_Link, link.active ? IM_COL32(125, 150, 185, 200) : IM_COL32(100, 108, 125, 130));
        ImNodes::PushColorStyle(ImNodesCol_LinkHovered, IM_COL32(150, 175, 210, 230));
        ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(150, 175, 210, 230));
        ImNodes::Link(link.uid, outputPin(link.from), inputPin(link.to));
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }

    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    anythingShown_ = shown > 0;
    int pinUnderMouse = 0;
    overPin_ = ImNodes::IsPinHovered(&pinUnderMouse);

    // A link that was clicked without being taken anywhere is selected, now
    // that it is drawn again.
    if (selectNext_ && selectNext_ != heldLink_) {
        bool drawn = false;
        for (const Connection& link : controller_.links()) drawn |= link.uid == selectNext_;
        if (const SystemLink* link = controller_.systemLink(selectNext_)) {
            const Node* from = controller_.node(link->from);
            const Node* to = controller_.node(link->to);
            drawn = from && to && controller_.visible(*from) && controller_.visible(*to);
        }
        if (drawn) {
            ImNodes::ClearNodeSelection();
            ImNodes::ClearLinkSelection();
            ImNodes::SelectLink(selectNext_);
        }
        selectNext_ = 0;
    }

    // Taking hold of a link changes nothing until it is let go: on another
    // pin it is moved there, well away from where it was it is removed, and
    // right where it was (a click, or a slip of the hand) it stays.
    int pickedUp = 0;
    if (ImNodes::IsLinkDestroyed(&pickedUp)) {
        heldLink_ = pickedUp;
        heldAt_ = ImGui::GetMousePos();
        heldWithCtrl_ = ImGui::GetIO().KeyCtrl;
        ImNodes::ClearLinkSelection();
    }

    // Dragging from one pin to another, with or without a link in hand.
    int startPin = 0, endPin = 0;
    if (ImNodes::IsLinkCreated(&startPin, &endPin)) {
        if (!isOutputPin(startPin)) std::swap(startPin, endPin);
        const std::string problem = controller_.connect(nodeOfPin(startPin), nodeOfPin(endPin), heldLink_);
        if (!problem.empty()) notify(problem);
        heldLink_ = 0;
    } else if (heldLink_ && ImNodes::IsLinkDropped(nullptr, true)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float travelled = std::hypot(mouse.x - heldAt_.x, mouse.y - heldAt_.y);
        if (heldWithCtrl_ || travelled > 24.0f * scale_) {
            const std::string problem = controller_.disconnect(heldLink_);
            if (!problem.empty()) notify(problem);
        } else {
            selectNext_ = heldLink_;
        }
        heldLink_ = 0;
    } else if (heldLink_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        heldLink_ = 0;  // let go where it already was
    }

    // Delete removes whatever is selected (devices and running applications stay).
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        std::vector<int> ids(ImNodes::NumSelectedLinks());
        if (!ids.empty()) {
            ImNodes::GetSelectedLinks(ids.data());
            ImNodes::ClearLinkSelection();
            for (int id : ids) {
                const std::string problem = controller_.disconnect(id);
                if (!problem.empty()) notify(problem);
            }
        }
        ids.assign(ImNodes::NumSelectedNodes(), 0);
        if (!ids.empty()) {
            ImNodes::GetSelectedNodes(ids.data());
            ImNodes::ClearNodeSelection();
            for (int id : ids) {
                const Node* node = controller_.node(id);
                if (node && controller_.removable(*node)) controller_.removeNode(id);
            }
        }
    }

    // Where the user has dragged things to.
    for (const Node& node : controller_.nodes()) {
        if (!placed_.count(node.uid) || !controller_.visible(node)) continue;
        const ImVec2 position = ImNodes::GetNodeGridSpacePos(node.uid);
        controller_.moveNode(node.uid, position.x / scale_, position.y / scale_);
    }

    // Tooltips and right-click menus.
    int hoveredNode = 0, hoveredLink = 0;
    const bool overNode = ImNodes::IsNodeHovered(&hoveredNode);
    const bool overLink = ImNodes::IsLinkHovered(&hoveredLink);
    if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;

    // (The canvas draws inside a child window of its own, hence ChildWindows.)
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        menuPosition_ = ImGui::GetMousePos();
        if (overLink) {
            menuLink_ = hoveredLink;
            ImGui::OpenPopup("link menu");
        } else if (overNode) {
            menuNode_ = hoveredNode;
            ImGui::OpenPopup("node menu");
        } else {
            ImGui::OpenPopup("canvas menu");
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (overNode) ImGui::SetTooltip("%s", controller_.describeNode(hoveredNode).c_str());
        else if (overLink) ImGui::SetTooltip("%s", controller_.describeLink(hoveredLink).c_str());
    }
}

void Editor::drawMenus() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale_, 8.0f * scale_));

    if (ImGui::BeginPopup("canvas menu")) {
        ImGui::TextDisabled("Add");
        ImGui::Separator();
        addItems(menuPosition_);
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("node menu")) {
        const Node* node = controller_.node(menuNode_);
        if (!node) {
            ImGui::CloseCurrentPopup();
        } else {
            const int uid = node->uid;
            const bool isApp = node->kind == NodeKind::App;
            const bool isOutput = node->kind == NodeKind::Output && node->present;
            const bool isSilent = uid == controller_.silentDevice();
            const bool canRemove = controller_.removable(*node);
            const bool canGiveBack = controller_.managed(*node);
            // An application is also connected to the device Windows plays it on.
            bool connected = controller_.linked(uid);
            for (const SystemLink& link : controller_.systemLinks()) {
                connected |= isApp && link.kind == SystemLink::Plays && link.from == uid;
            }
            ImGui::TextDisabled("%s", node->label.c_str());
            ImGui::Separator();
            // Each of these may change the list of nodes: `node` is not used after them.
            if (ImGui::MenuItem("Disconnect everything", nullptr, false, connected)) {
                const std::string problem = controller_.disconnectAll(uid);
                if (!problem.empty()) notify(problem);
            } else if (isApp && ImGui::MenuItem("Let Windows choose its devices again", nullptr, false, canGiveBack)) {
                controller_.giveBack(uid);
            } else if (isOutput && ImGui::MenuItem("Use as the silent device", nullptr, isSilent)) {
                controller_.setSilentDevice(isSilent ? 0 : uid);
            } else if (ImGui::MenuItem("Remove", "Del", false, canRemove)) {
                controller_.removeNode(uid);
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("link menu")) {
        const Connection* link = nullptr;
        for (const Connection& candidate : controller_.links()) {
            if (candidate.uid == menuLink_) link = &candidate;
        }
        if (const SystemLink* theirs = link ? nullptr : controller_.systemLink(menuLink_)) {
            // One that Windows makes: what it is, and the one thing a menu can do to it.
            const int uid = theirs->uid;
            const bool removable = theirs->kind == SystemLink::Plays || theirs->kind == SystemLink::Listens;
            const bool canHandOver = controller_.canHandOver(uid);
            ImGui::TextUnformatted(controller_.describeLink(uid).c_str());
            if (removable) ImGui::Separator();
            std::string problem;
            if (canHandOver && ImGui::MenuItem("Let Wolfe carry it instead (gain, less delay)")) {
                problem = controller_.handOver(uid);
            } else if (removable && ImGui::MenuItem("Remove this link", "Ctrl+click")) {
                problem = controller_.disconnect(uid);
            }
            if (!problem.empty()) notify(problem);
        } else if (!link) {
            ImGui::CloseCurrentPopup();
        } else {
            // (The link may be gone after any of the items below: nothing reads it after them.)
            const int uid = link->uid;
            float gain = link->gainDb;
            ImGui::Text("%s  ->  %s", controller_.node(link->from)->label.c_str(), controller_.node(link->to)->label.c_str());
            if (link->suspended) ImGui::TextDisabled("Paused: Windows already plays this there.");
            ImGui::SetNextItemWidth(260.0f * scale_);
            if (ImGui::SliderFloat("##gain", &gain, -40.0f, 12.0f, "%+.1f dB")) controller_.setGainDb(uid, gain);
            const bool canHandOver = controller_.canHandOver(uid);
            if (ImGui::MenuItem("Back to 0 dB", nullptr, false, gain != 0.0f)) {
                controller_.setGainDb(uid, 0.0f);
            } else if (canHandOver && ImGui::MenuItem("Let Windows carry it instead (also without Wolfe)")) {
                const std::string problem = controller_.handOver(uid);
                if (!problem.empty()) notify(problem);
            } else if (ImGui::MenuItem("Remove this link", "Ctrl+click")) {
                controller_.disconnect(uid);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
}

void Editor::drawStatusLine() {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f * scale_);
    ImGui::AlignTextToFramePadding();
    if (!notice_.empty() && now_ < noticeUntil_) {
        std::string oneLine = notice_;
        for (char& c : oneLine) {
            if (c == '\n') c = ' ';
        }
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), "%s", oneLine.c_str());
    } else {
        ImGui::TextDisabled("Drag from a pin to connect, as many times as you like.  Drag a link to move the end you took; "
                            "pull it away, or Ctrl+click, to remove it.  Bright links are carried by Wolfe, pale ones by Windows.");
    }
}

}  // namespace wolfe
