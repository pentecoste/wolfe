#include "editor.h"

#include "shell.h"

#include "imnodes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <sstream>
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
const ImVec4 kProblemColour(0.95f, 0.65f, 0.30f, 1.0f);

ImU32 titleColour(const Node& node, float brighten) {
    float r, g, b;
    switch (node.kind) {
        case NodeKind::Input: r = 0.20f, g = 0.42f, b = 0.72f; break;  // blue
        case NodeKind::Gain: r = 0.72f, g = 0.45f, b = 0.16f; break;   // amber
        case NodeKind::WolfeSpeaker:
        case NodeKind::WolfeMic: r = 0.66f, g = 0.27f, b = 0.40f; break;  // wine
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

// "Spotify, Firefox +2": as many names as fit in `width`, and how many more there are.
std::string namesThatFit(const std::vector<std::string>& names, float width) {
    if (names.empty()) return {};
    for (size_t shown = names.size(); shown >= 1; shown--) {
        std::string text;
        for (size_t i = 0; i < shown; i++) text += (i ? ", " : "") + names[i];
        if (shown < names.size()) text += " +" + std::to_string(names.size() - shown);
        if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    }
    const std::string more = names.size() > 1 ? " +" + std::to_string(names.size() - 1) : "";
    return fit(names[0], width - ImGui::CalcTextSize(more.c_str()).x) + more;
}

// Every word of the query is somewhere in the choice's words.
bool matches(const std::string& words, const char* query) {
    std::istringstream parts(toLower(query));
    std::string part;
    while (parts >> part) {
        if (words.find(part) == std::string::npos) return false;
    }
    return true;
}

bool anyPopupOpen() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

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

void Editor::flash(const std::string& message) {
    flash_ = message;
    flashUntil_ = now_ + 4.0;
}

void Editor::draw(double now) {
    now_ = now;
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

    ImGui::BeginChild("canvas");
    canvasCentre_ = ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5f,
                           ImGui::GetWindowPos().y + ImGui::GetWindowSize().y * 0.4f);
    // Ctrl+A: search for something to add. (Not while text is being typed,
    // where it selects the text.)
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && !anyPopupOpen() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        openSearch(overCanvas_ ? io.MousePos : canvasCentre_);
    }
    drawCanvas();
    drawMenus();
    drawSearch();
    drawDialogs();
    ImGui::EndChild();

    drawFlash();
    ImGui::End();
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * scale_);
    if (ImGui::BeginMenu("Wolfe")) {
        const bool autostart = startsWithWindows();
        if (ImGui::MenuItem("Start with Windows", nullptr, autostart)) setStartsWithWindows(!autostart);
        const bool tray = controller_.closeToTray();
        if (ImGui::MenuItem("Keep running when the window is closed", nullptr, tray)) controller_.setCloseToTray(!tray);
        const bool apps = controller_.showApps();
        if (ImGui::MenuItem("Show applications in boxes", nullptr, apps)) controller_.setShowApps(!apps);
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

// ---------------------------------------------------------------------------
// Adding things
// ---------------------------------------------------------------------------

std::vector<Editor::Choice> Editor::choices() const {
    std::vector<Choice> list;
    list.push_back({Choice::Gain, "Gain", "louder or quieter", "gain volume louder quieter level", {}});
    list.push_back({Choice::WolfeSpeaker, "Wolfe speaker", "new device: applications play on it",
                    "wolfe speaker new virtual playback output device", {}});
    list.push_back({Choice::WolfeMic, "Wolfe microphone", "new device: applications record from it",
                    "wolfe microphone mic new virtual recording input device", {}});
    for (const DeviceInfo& device : controller_.devicesToAdd(true)) {
        list.push_back({Choice::Microphone, "Microphone: " + device.name, {},
                        "microphone mic recording input " + toLower(device.name), toUtf8(device.id)});
    }
    for (const DeviceInfo& device : controller_.devicesToAdd(false)) {
        list.push_back({Choice::Speaker, "Speaker: " + device.name, {},
                        "speaker headphones playback output " + toLower(device.name), toUtf8(device.id)});
    }
    return list;
}

void Editor::choose(const Choice& choice, ImVec2 screenPosition) {
    switch (choice.kind) {
        case Choice::Gain: placeNew(controller_.addGain(0, 0), screenPosition); break;
        case Choice::Microphone:
        case Choice::Speaker:
            placeNew(controller_.addDevice(choice.kind == Choice::Microphone, choice.deviceId, 0, 0), screenPosition);
            break;
        case Choice::WolfeSpeaker:
        case Choice::WolfeMic: {
            // It needs a name first.
            naming_ = choice.kind == Choice::WolfeMic ? 2 : 1;
            namingOpened_ = false;
            namingAt_ = screenPosition;
            nameProblem_.clear();
            const std::string name = controller_.freeWolfeName(naming_ == 2);
            snprintf(name_, sizeof(name_), "%s", name.c_str());
            break;
        }
    }
}

void Editor::openSearch(ImVec2 screenPosition) {
    searchWanted_ = true;
    searchAt_ = screenPosition;
}

// The Add menu, in the menu bar and on a right click on the canvas: the
// search, and the boxes that are not a device of the computer.
void Editor::addItems(ImVec2 screenPosition) {
    if (ImGui::MenuItem("Search...", "Ctrl+A")) openSearch(screenPosition);
    ImGui::Separator();
    const std::vector<Choice> all = choices();
    for (const Choice& choice : all) {
        if (choice.kind != Choice::Gain) continue;
        if (ImGui::MenuItem("Gain")) choose(choice, screenPosition);
    }
    // (The computer's devices are added from the search.)
    ImGui::Separator();
    for (const Choice& choice : all) {
        if (choice.kind != Choice::WolfeSpeaker && choice.kind != Choice::WolfeMic) continue;
        if (ImGui::MenuItem((choice.title + "...").c_str())) choose(choice, screenPosition);
    }
}

// Up and Down in the search box move through the list; typing starts it again from the top.
int Editor::searchKeys(ImGuiInputTextCallbackData* data) {
    Editor* editor = (Editor*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (data->EventKey == ImGuiKey_UpArrow) editor->selected_ = std::max(0, editor->selected_ - 1);
        if (data->EventKey == ImGuiKey_DownArrow) editor->selected_ = std::min(editor->shownChoices_ - 1, editor->selected_ + 1);
        editor->scrollToSelected_ = true;
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
        editor->selected_ = 0;
        editor->scrollToSelected_ = true;
    }
    return 0;
}

void Editor::drawSearch() {
    if (searchWanted_) {
        searchWanted_ = false;
        query_[0] = 0;
        selected_ = 0;
        ImGui::OpenPopup("search");
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.3f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(480.0f * scale_, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * scale_, 10.0f * scale_));
    const bool open = ImGui::BeginPopup("search");
    ImGui::PopStyleVar();
    if (!open) return;

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = ImGui::InputTextWithHint(
        "##query", "Add: gain, microphone, speaker, Wolfe speaker...", query_, sizeof(query_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit,
        searchKeys, this);

    const std::vector<Choice> all = choices();
    std::vector<const Choice*> shown;
    for (const Choice& choice : all) {
        if (matches(toLower(choice.title) + " " + choice.words, query_)) shown.push_back(&choice);
    }
    shownChoices_ = (int)shown.size();
    selected_ = std::max(0, std::min(selected_, shownChoices_ - 1));

    int chosen = -1;
    if (shown.empty()) {
        ImGui::TextDisabled("Nothing to add by that name.");
    } else {
        const float row = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("choices", ImVec2(0.0f, row * std::min((int)shown.size(), 10) + 2.0f * scale_));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text), quiet = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        for (int i = 0; i < (int)shown.size(); i++) {
            // The text goes where a line of text would, a little in from the highlight's edge.
            const ImVec2 at(ImGui::GetCursorScreenPos().x + 6.0f * scale_, ImGui::GetCursorScreenPos().y);
            ImGui::PushID(i);
            if (ImGui::Selectable("##choice", i == selected_)) chosen = i;
            ImGui::PopID();
            if (i == selected_ && scrollToSelected_) ImGui::SetScrollHereY();
            const float room = ImGui::GetItemRectMax().x - at.x - 6.0f * scale_;
            const std::string title = fit(shown[i]->title, room);
            draw->AddText(at, text, title.c_str());
            if (!shown[i]->hint.empty()) {
                const float after = ImGui::CalcTextSize(title.c_str()).x + 14.0f * scale_;
                draw->AddText(ImVec2(at.x + after, at.y), quiet, fit(shown[i]->hint, room - after).c_str());
            }
        }
        scrollToSelected_ = false;
        ImGui::EndChild();
    }

    if ((entered || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        !shown.empty()) {
        chosen = selected_;
    }
    if (chosen >= 0) {
        const Choice choice = *shown[chosen];
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        choose(choice, searchAt_);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::placeNew(int uid, ImVec2 screenPosition) {
    if (!uid || placed_.count(uid)) return;
    // The canvas converts the screen position; the controller learns it when
    // positions are read back at the end of the frame. Two boxes on the same
    // spot would look like one: a new one goes below whatever is there.
    ImNodes::SetNodeScreenSpacePos(uid, screenPosition);
    ImVec2 position = ImNodes::GetNodeGridSpacePos(uid);
    for (int tries = 0; tries < 30; tries++) {
        bool covered = false;
        for (const Node& other : controller_.nodes()) {
            if (other.uid == uid || !placed_.count(other.uid)) continue;
            const ImVec2 there = ImNodes::GetNodeGridSpacePos(other.uid);
            covered |= std::fabs(there.x - position.x) < kNodeWidth * scale_ && std::fabs(there.y - position.y) < 70.0f * scale_;
        }
        if (!covered) break;
        position.y += 90.0f * scale_;
    }
    ImNodes::SetNodeGridSpacePos(uid, position);
    placed_.insert(uid);
}

// Removing one of Wolfe's devices takes it away from Windows, and with it the
// applications' choice of it: that is asked first.
void Editor::askToRemove(int nodeUid) {
    const Node* node = controller_.node(nodeUid);
    if (!node) return;
    if (node->isWolfeDevice()) {
        removing_ = nodeUid;
        removingOpened_ = false;
    } else {
        controller_.removeNode(nodeUid);
    }
}

// The windows that ask something before going on.
void Editor::drawDialogs() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    // The title says which kind; the id after ### keeps it one window.
    const char* const namingTitle = naming_ == 2 ? "New Wolfe microphone###naming" : "New Wolfe speaker###naming";
    if (naming_ && !namingOpened_) {
        ImGui::OpenPopup(namingTitle);
        namingOpened_ = true;
    }
    if (ImGui::BeginPopupModal(namingTitle, nullptr, flags)) {
        const bool recording = naming_ == 2;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(320.0f * scale_);
        const bool entered = ImGui::InputText("Name", name_, sizeof(name_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!nameProblem_.empty()) ImGui::TextColored(kProblemColour, "%s", nameProblem_.c_str());
        if (ImGui::Button("Create") || entered) {
            const int uid = controller_.addWolfeDevice(recording, name_, 0, 0, &nameProblem_);
            if (uid) {
                placeNew(uid, namingAt_);
                naming_ = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            naming_ = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (namingOpened_) {
        naming_ = 0;
        namingOpened_ = false;
    }

    if (removing_ && !removingOpened_) {
        ImGui::OpenPopup("Remove the device");
        removingOpened_ = true;
    }
    if (ImGui::BeginPopupModal("Remove the device", nullptr, flags)) {
        const Node* node = controller_.node(removing_);
        if (!node) {
            removing_ = 0;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::Text("Remove \"%s\"?", node->label.c_str());
            ImGui::TextDisabled("Applications using it go back to the default device.");
            ImGui::Spacing();
            if (ImGui::Button("Remove")) {
                controller_.removeNode(removing_);
                removing_ = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep it") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                removing_ = 0;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    } else if (removingOpened_) {
        removing_ = 0;
        removingOpened_ = false;
    }
}

void Editor::drawFlash() {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) flash_.clear();
    if (flash_.empty() || now_ >= flashUntil_) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextColored(kProblemColour, "%s", flash_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------------------
// The canvas
// ---------------------------------------------------------------------------

void Editor::drawNode(const Node& node) {
    const float width = kNodeWidth * scale_;
    const bool trouble = node.active && !node.healthy;

    ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColour(node, 0.0f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColour(node, 0.08f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColour(node, 0.14f));
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, trouble ? IM_COL32(235, 150, 60, 255) : IM_COL32(18, 19, 23, 255));
    ImNodes::BeginNode(node.uid);

    std::string title = node.label, second = node.detail;
    if (node.isDevice()) splitDeviceName(node.label, title, second);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(fit(title, width).c_str());
    ImNodes::EndNodeTitleBar();
    if (!second.empty()) ImGui::TextUnformatted(fit(second, width).c_str());

    // The applications that have audio flowing on its device right now (the
    // tooltip lists the others too). The line is there even when there are
    // none, so that the box does not change size as they come and go.
    if (controller_.showApps() && node.kind != NodeKind::Gain) {
        std::vector<std::string> names;
        for (const AppUse& app : node.apps) {
            if (app.active) names.push_back(app.name);
        }
        const bool recording = node.kind == NodeKind::Input || node.kind == NodeKind::WolfeMic;
        if (names.empty()) ImGui::TextDisabled("%s", recording ? "no app recording" : "no app playing");
        else ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.88f, 1.0f), "%s", namesThatFit(names, width).c_str());
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (node.kind == NodeKind::Gain) {
        // in [value] out: the value is dragged left or right, or typed after a double click.
        const float in = ImGui::CalcTextSize("in").x, out = ImGui::CalcTextSize("out").x;
        ImNodes::BeginInputAttribute(inputPin(node.uid));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("in");
        ImNodes::EndInputAttribute();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width - in - out - 2.0f * spacing);
        float gain = node.gainDb;
        ImGui::PushID(node.uid);
        if (ImGui::DragFloat("##gain", &gain, 0.1f, Controller::kMinGainDb, Controller::kMaxGainDb, "%+.1f dB")) {
            controller_.setGainDb(node.uid, gain);
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImNodes::BeginOutputAttribute(outputPin(node.uid));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("out");
        ImNodes::EndOutputAttribute();
    } else {
        // A few words saying what it is doing, on the line with the pin.
        std::string state;
        ImVec4 colour(0.55f, 0.57f, 0.62f, 1.0f);
        const ImVec4 live(0.45f, 0.85f, 0.55f, 1.0f);
        if (node.isWolfeDevice()) {
            if (node.active && !node.healthy) {
                state = node.status;
                colour = kProblemColour;
            } else if (!node.present) {
                state = "being added to Windows";
            } else if (node.status.find("recording from it") != std::string::npos) {
                state = "in use";
                colour = live;
            } else {
                state = "ready";
            }
        } else if (!node.present) {
            state = "not plugged in";
        } else if (!node.active) {
            state = "not linked";
        } else if (node.healthy) {
            colour = live;
            state = "live";
        } else {
            colour = kProblemColour;
            state = node.status;
        }

        if (node.takesLinks()) {
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
        drawMeter(node.level, width, 5.0f * scale_, node.present);
    }

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
    for (const Node& node : controller_.nodes()) {
        if (!placed_.count(node.uid) && node.hasPosition) {
            ImNodes::SetNodeGridSpacePos(node.uid, ImVec2(node.x * scale_, node.y * scale_));
            placed_.insert(node.uid);
        }
        drawNode(node);
    }
    // (A link that was pulled off its pin is in the user's hand, not here.)
    for (const Connection& link : controller_.links()) {
        if (link.uid == heldLink_) continue;
        ImNodes::Link(link.uid, outputPin(link.from), inputPin(link.to));
    }
    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    overCanvas_ = ImNodes::IsEditorHovered();
    int pinUnderMouse = 0;
    overPin_ = ImNodes::IsPinHovered(&pinUnderMouse);

    // An empty canvas says how to start.
    if (controller_.nodes().empty() && !anyPopupOpen()) {
        const char* hint = "Press Ctrl+A, or right-click, to add something";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        ImGui::GetForegroundDrawList()->AddText(ImVec2(canvasCentre_.x - size.x * 0.5f, canvasCentre_.y - size.y * 0.5f),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
    }

    // A link that was clicked without being taken anywhere is selected, now
    // that it is drawn again.
    if (selectNext_ && selectNext_ != heldLink_) {
        bool drawn = false;
        for (const Connection& link : controller_.links()) drawn |= link.uid == selectNext_;
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
        const std::string problem = heldLink_ ? controller_.move(heldLink_, nodeOfPin(startPin), nodeOfPin(endPin))
                                              : controller_.connect(nodeOfPin(startPin), nodeOfPin(endPin));
        if (!problem.empty()) flash(problem);
        heldLink_ = 0;
    } else if (heldLink_ && ImNodes::IsLinkDropped(nullptr, true)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const float travelled = std::hypot(mouse.x - heldAt_.x, mouse.y - heldAt_.y);
        if (heldWithCtrl_ || travelled > 24.0f * scale_) controller_.disconnect(heldLink_);
        else selectNext_ = heldLink_;
        heldLink_ = 0;
    } else if (heldLink_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        heldLink_ = 0;  // let go where it already was
    }

    // Delete removes whatever is selected.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        std::vector<int> ids(ImNodes::NumSelectedLinks());
        if (!ids.empty()) {
            ImNodes::GetSelectedLinks(ids.data());
            ImNodes::ClearLinkSelection();
            for (int id : ids) controller_.disconnect(id);
        }
        ids.assign(ImNodes::NumSelectedNodes(), 0);
        if (!ids.empty()) {
            ImNodes::GetSelectedNodes(ids.data());
            ImNodes::ClearNodeSelection();
            for (int id : ids) askToRemove(id);
        }
    }

    // Where the user has dragged things to.
    for (const Node& node : controller_.nodes()) {
        if (!placed_.count(node.uid)) continue;
        const ImVec2 position = ImNodes::GetNodeGridSpacePos(node.uid);
        controller_.moveNode(node.uid, position.x / scale_, position.y / scale_);
    }

    // Tooltips and right-click menus.
    int hoveredNode = 0, hoveredLink = 0;
    const bool overNode = ImNodes::IsNodeHovered(&hoveredNode);
    const bool overLink = ImNodes::IsLinkHovered(&hoveredLink);
    if (anyPopupOpen()) return;

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
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && (flash_.empty() || now_ >= flashUntil_)) {
        const std::string text = overNode ? controller_.describeNode(hoveredNode)
                                 : overLink ? controller_.describeLink(hoveredLink)
                                            : std::string();
        if (!text.empty()) ImGui::SetTooltip("%s", text.c_str());
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
            const bool isGain = node->kind == NodeKind::Gain;
            const bool unity = node->gainDb == 0.0f;
            const bool connected = controller_.linked(uid);
            ImGui::TextDisabled("%s", node->label.c_str());
            ImGui::Separator();
            // Each of these may change the list of nodes: `node` is not used after them.
            if (isGain && ImGui::MenuItem("Back to 0 dB", nullptr, false, !unity)) {
                controller_.setGainDb(uid, 0.0f);
            } else if (ImGui::MenuItem("Disconnect everything", nullptr, false, connected)) {
                controller_.disconnectAll(uid);
            } else if (ImGui::MenuItem("Remove", "Del")) {
                askToRemove(uid);
            }
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("link menu")) {
        const Connection* link = nullptr;
        for (const Connection& candidate : controller_.links()) {
            if (candidate.uid == menuLink_) link = &candidate;
        }
        if (!link) {
            ImGui::CloseCurrentPopup();
        } else {
            const int uid = link->uid;
            ImGui::TextDisabled("%s  ->  %s", controller_.node(link->from)->label.c_str(),
                                controller_.node(link->to)->label.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Remove this link", "Ctrl+click")) controller_.disconnect(uid);
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
}

}  // namespace wolfe
