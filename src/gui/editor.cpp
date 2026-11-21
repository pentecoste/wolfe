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
        case NodeKind::Effect: r = 0.72f, g = 0.45f, b = 0.16f; break;   // amber
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

// A level meter, -60 dB at the left edge and 0 dB (full scale) at the right,
// with the level in numbers after it. `prefix` goes before the number ("in ");
// `markDb`, if given, is drawn as a line across the bar: a threshold.
void drawMeter(float level, float width, float height, bool enabled, const char* prefix = "", float markDb = NAN) {
    const float line = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(width, line));
    const ImVec2 corner = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Room for the longest number, so that bars line up from box to box.
    const float room = ImGui::CalcTextSize("in -60 dB").x + ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 from(corner.x, corner.y + (line - height) * 0.5f), to(corner.x + width - room, from.y + height);
    draw->AddRectFilled(from, to, IM_COL32(0, 0, 0, enabled ? 110 : 50), height * 0.5f);
    if (!enabled) return;

    const float db = level > 0.001f ? 20.0f * std::log10(level) : -100.0f;
    const auto across = [&](float decibels) { return from.x + (to.x - from.x) * std::max(0.0f, std::min(1.0f, (decibels + 60.0f) / 60.0f)); };
    if (db > -60.0f) {
        const ImU32 colour = db > -1.0f ? IM_COL32(230, 80, 70, 255) : db > -12.0f ? IM_COL32(230, 200, 80, 255) : IM_COL32(90, 200, 120, 255);
        draw->AddRectFilled(from, ImVec2(across(db), to.y), colour, height * 0.5f);
    }
    if (!std::isnan(markDb)) {
        const float x = across(markDb), reach = std::max(2.0f, height * 0.7f);
        draw->AddLine(ImVec2(x, from.y - reach), ImVec2(x, to.y + reach), IM_COL32(240, 240, 245, 230), std::max(1.0f, height * 0.35f));
    }
    char text[32];
    if (db > -60.0f) snprintf(text, sizeof(text), "%s%.0f dB", prefix, db);
    else snprintf(text, sizeof(text), "%s-- dB", prefix);
    const float textWidth = ImGui::CalcTextSize(text).x;
    draw->AddText(ImVec2(corner.x + width - textWidth, corner.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
}

// Where a setting's value lies along its range, 0..1, as dragging it goes.
float fractionOf(const EffectParameter& parameter, float value) {
    if (parameter.logarithmic && parameter.min > 0.0f) {
        return std::log(value / parameter.min) / std::log(parameter.max / parameter.min);
    }
    return (value - parameter.min) / (parameter.max - parameter.min);
}

// One setting of an effect: a field dragged left or right, or typed after a
// double click, with a bar behind it that shows where the value lies in its
// range. A setting that goes below and above zero fills from zero. Returns
// true when the user changed the value.
bool drawSetting(const EffectParameter& parameter, float& value, float width) {
    const ImVec2 from = ImGui::GetCursorScreenPos();
    const ImVec2 to(from.x + width, from.y + ImGui::GetFrameHeight());
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(from, to, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    const float zero = !parameter.logarithmic && parameter.min < 0.0f && parameter.max > 0.0f ? fractionOf(parameter, 0.0f) : 0.0f;
    const float at = std::max(0.0f, std::min(1.0f, fractionOf(parameter, value)));
    const float left = from.x + width * std::min(zero, at), right = from.x + width * std::max(zero, at);
    if (right - left >= 1.0f) {
        draw->AddRectFilled(ImVec2(left, from.y), ImVec2(right, to.y), IM_COL32(110, 150, 215, 110),
                            left <= from.x + 0.5f || right >= to.x - 0.5f ? rounding : 0.0f);
    }

    const float speed = parameter.logarithmic ? (parameter.max - parameter.min) / 250.0f : parameter.dragSpeed;
    const ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | (parameter.logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    // At the bottom, some settings mean "none": they say so instead of a number.
    const char* format = parameter.atMin && value <= parameter.min ? parameter.atMin : parameter.format;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 30));
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::DragFloat("##value", &value, speed, parameter.min, parameter.max, format, flags);
    ImGui::PopStyleColor(3);
    return changed;
}

// A "?" in a circle in a square of `size` at `at`.
void drawHelpIcon(ImVec2 at, float size, bool hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 colour = IM_COL32(255, 255, 255, hovered ? 255 : 150);
    const ImVec2 centre(at.x + size * 0.5f, at.y + size * 0.5f);
    if (hovered) draw->AddCircleFilled(centre, size * 0.46f, IM_COL32(0, 0, 0, 70));
    draw->AddCircle(centre, size * 0.42f, colour, 0, std::max(1.0f, size * 0.07f));
    const ImVec2 mark = ImGui::CalcTextSize("?");
    const float shrink = 0.75f;
    draw->AddText(nullptr, ImGui::GetFontSize() * shrink,
                  ImVec2(centre.x - mark.x * shrink * 0.5f, centre.y - mark.y * shrink * 0.5f), colour, "?");
}

// Effects whose meter shows what goes in, with their first setting (a level) marked on it.
bool metersItsInput(const std::string& type) { return type == "limiter" || type == "compressor" || type == "gate"; }

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

// A loudspeaker in a square of `size` at `at`: with sound waves while it
// plays, red and crossed out while muted.
void drawSpeakerIcon(ImVec2 at, float size, bool muted, bool hovered) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 colour = muted ? IM_COL32(255, 105, 105, 255) : IM_COL32(255, 255, 255, hovered ? 255 : 170);
    // Muted, it sits on a dark patch, so that the red reads on any title colour.
    if (muted || hovered) {
        draw->AddRectFilled(ImVec2(at.x - size * 0.1f, at.y - size * 0.05f), ImVec2(at.x + size * 1.1f, at.y + size * 1.05f),
                            muted ? IM_COL32(30, 12, 14, hovered ? 255 : 220) : IM_COL32(0, 0, 0, 60), size * 0.25f);
    }
    auto point = [&](float x, float y) { return ImVec2(at.x + x * size, at.y + y * size); };
    draw->AddRectFilled(point(0.10f, 0.37f), point(0.30f, 0.63f), colour);
    const ImVec2 cone[4] = {point(0.29f, 0.37f), point(0.52f, 0.16f), point(0.52f, 0.84f), point(0.29f, 0.63f)};
    draw->AddConvexPolyFilled(cone, 4, colour);
    const float thickness = std::max(1.0f, size * 0.08f);
    if (muted) {
        draw->AddLine(point(0.64f, 0.36f), point(0.92f, 0.64f), colour, thickness);
        draw->AddLine(point(0.64f, 0.64f), point(0.92f, 0.36f), colour, thickness);
    } else {
        for (float radius : {0.16f, 0.32f}) {
            draw->PathArcTo(point(0.56f, 0.5f), radius * size, -0.85f, 0.85f, 12);
            draw->PathStroke(colour, ImDrawFlags_None, thickness);
        }
    }
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
    style_ = style;
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

const int kZoomOut = -10, kZoomIn = 8;  // 1.1^-10 = 39%, 1.1^8 = 214%

void Editor::zoomBy(int steps, ImVec2 pivot) { zoomTo(zoomWanted_ + steps, pivot); }

void Editor::zoomTo(int step, ImVec2 pivot) {
    zoomWanted_ = std::max(kZoomOut, std::min(kZoomIn, step));
    zoomPivot_ = pivot;
}

// imnodes knows nothing of zoom: the canvas is laid out again at the new
// size, the boxes put where they now belong and the view moved so that the
// point under the mouse stays under it. Called before the canvas begins.
void Editor::applyZoom() {
    // (Not in the middle of a drag: the canvas holds on to where it started.)
    if (zoomWanted_ != zoomStep_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float zoom = std::pow(1.1f, (float)zoomWanted_);
        const float ratio = zoom / zoom_;
        const ImVec2 panning = ImNodes::EditorContextGetPanning();
        const ImVec2 under(zoomPivot_.x - gridOrigin_.x - panning.x, zoomPivot_.y - gridOrigin_.y - panning.y);
        ImNodes::EditorContextResetPanning(
            ImVec2(zoomPivot_.x - gridOrigin_.x - under.x * ratio, zoomPivot_.y - gridOrigin_.y - under.y * ratio));
        zoom_ = zoom;
        zoomStep_ = zoomWanted_;
        for (const Node& node : controller_.nodes()) {
            if (placed_.count(node.uid)) ImNodes::SetNodeGridSpacePos(node.uid, ImVec2(node.x * view(), node.y * view()));
        }
    }

    ImNodesStyle style = style_;
    const float z = zoom_;
    const float reach = std::max(z, 0.6f);  // what the mouse has to come near stays easy to hit
    style.GridSpacing *= z;
    style.NodeCornerRounding *= z;
    style.NodePadding = ImVec2(style_.NodePadding.x * z, style_.NodePadding.y * z);
    style.NodeBorderThickness *= std::max(z, 0.5f);
    style.LinkThickness *= std::max(z, 0.5f);
    style.LinkHoverDistance *= reach;
    style.PinCircleRadius *= z;
    style.PinQuadSideLength *= z;
    style.PinTriangleSideLength *= z;
    style.PinLineThickness *= std::max(z, 0.5f);
    style.PinHoverRadius *= reach;
    style.PinOffset *= z;
    ImNodes::GetStyle() = style;
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
    // Zoom: the wheel over the canvas, or Ctrl and + - 0.
    if (!anyPopupOpen()) {
        const ImVec2 pivot = overCanvas_ ? io.MousePos : canvasCentre_;
        // (Shift is allowed: on some keyboards + is Shift and =.)
        const auto ctrlAnd = [&io](ImGuiKey key, bool repeat) {
            return io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(key, repeat);
        };
        if (!io.WantTextInput) {
            if (ctrlAnd(ImGuiKey_Equal, true) || ctrlAnd(ImGuiKey_KeypadAdd, true)) zoomBy(1, pivot);
            if (ctrlAnd(ImGuiKey_Minus, true) || ctrlAnd(ImGuiKey_KeypadSubtract, true)) zoomBy(-1, pivot);
            if (ctrlAnd(ImGuiKey_0, false) || ctrlAnd(ImGuiKey_Keypad0, false)) zoomTo(0, pivot);
        }
        if (overCanvas_ && io.MouseWheel != 0.0f && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            wheel_ += io.MouseWheel;
            const int steps = (int)wheel_;  // whole notches; the rest waits for more
            wheel_ -= (float)steps;
            if (steps) zoomBy(steps, pivot);
        }
    }
    drawCanvas();
    drawMenus();
    drawSearch();
    drawDialogs();
    drawWelcome();
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
        if (ImGui::MenuItem("Import graph...")) fileRequest = FileRequest::Import;
        if (ImGui::MenuItem("Export graph...", nullptr, false, !controller_.nodes().empty())) fileRequest = FileRequest::Export;
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) exitRequested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        addItems(canvasCentre_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::TextDisabled("Zoom %d%%", (int)std::lround(zoom_ * 100.0f));
        ImGui::Separator();
        if (ImGui::MenuItem("Zoom in", "Ctrl++", false, zoomWanted_ < kZoomIn)) zoomBy(1, canvasCentre_);
        if (ImGui::MenuItem("Zoom out", "Ctrl+-", false, zoomWanted_ > kZoomOut)) zoomBy(-1, canvasCentre_);
        if (ImGui::MenuItem("Actual size", "Ctrl+0", false, zoomWanted_ != 0)) zoomTo(0, canvasCentre_);
        ImGui::Separator();
        if (ImGui::MenuItem("Show everything", nullptr, false, !controller_.nodes().empty())) showAll();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Getting started")) showWelcome();
        ImGui::TextDisabled("Each box explains itself: hover its \"?\".");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void Editor::showWelcome() {
    welcome_ = true;
    welcomeOpened_ = false;
}

// The window that explains the basics, at the first start and from the Help
// menu, with the example to start from.
void Editor::drawWelcome() {
    const char* const title = "Welcome to Wolfe";
    if (welcome_ && !welcomeOpened_) {
        ImGui::OpenPopup(title);
        welcomeOpened_ = true;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));  // (its height is known only once it is drawn)
    ImGui::SetNextWindowSize(ImVec2(560.0f * scale_, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * scale_, 14.0f * scale_));
    const bool open = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    if (!open) {
        if (welcomeOpened_) welcome_ = welcomeOpened_ = false;
        return;
    }

    const ImVec4 strong(0.85f, 0.88f, 0.95f, 1.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Wolfe makes speakers and microphones of its own, which Windows lists like any other, and "
                           "wires them to your real ones through effects.");
    ImGui::Spacing();
    ImGui::TextColored(strong, "1.  Add boxes");
    ImGui::TextUnformatted("Right-click the canvas, or press Ctrl+A and type. Add your microphone and headphones, the "
                           "effects you want, and Wolfe speakers or microphones.");
    ImGui::Spacing();
    ImGui::TextColored(strong, "2.  Link them");
    ImGui::TextUnformatted("Drag from a box's \"out\" to another box's \"in\": the sound goes that way. Right-click a "
                           "link or a box for more; the loudspeaker in a box's title mutes it.");
    ImGui::Spacing();
    ImGui::TextColored(strong, "3.  Give the devices to applications");
    ImGui::TextUnformatted("In Windows, Settings > System > Sound > Volume mixer, give an application a Wolfe speaker: "
                           "what it plays comes into Wolfe. In a chat program, choose a Wolfe microphone: it hears "
                           "what you link into it.");
    ImGui::Spacing();
    ImGui::TextDisabled("Not sure what a box does? Hover the \"?\" in its title.");
    ImGui::Separator();
    ImGui::TextUnformatted("The example plays what goes to \"Wolfe Music\" (give it to your music player) on your "
                           "headphones and, quieter, into \"Wolfe Chat\" with your microphone cleaned of noise "
                           "(choose it as the microphone in your chat program).");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (ImGui::Button("Build the example")) {
        const std::string problems = controller_.buildExample();
        if (!problems.empty()) flash(problems);
        showAll();
        welcome_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(controller_.nodes().empty() ? "Start empty" : "Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        welcome_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Adding things
// ---------------------------------------------------------------------------

std::vector<Editor::Choice> Editor::choices() const {
    std::vector<Choice> list;
    for (const EffectType& type : effectTypes()) {
        if (type.offered) list.push_back({Choice::Effect, type.name, type.hint, toLower(type.words), type.type});
    }
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
        case Choice::Effect: placeNew(controller_.addEffect(choice.target, 0, 0), screenPosition); break;
        case Choice::Microphone:
        case Choice::Speaker:
            placeNew(controller_.addDevice(choice.kind == Choice::Microphone, choice.target, 0, 0), screenPosition);
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
// search, the effects, Wolfe's own devices and the computer's.
void Editor::addItems(ImVec2 screenPosition) {
    if (ImGui::MenuItem("Search...", "Ctrl+A")) openSearch(screenPosition);
    ImGui::Separator();
    const std::vector<Choice> all = choices();
    for (const Choice& choice : all) {
        if (choice.kind != Choice::Effect) continue;
        // Routing, level, tone, space and time, character.
        if (choice.target == "gain" || choice.target == "highpass" || choice.target == "delay" || choice.target == "distortion") {
            ImGui::Separator();
        }
        if (ImGui::MenuItem(choice.title.c_str())) choose(choice, screenPosition);
    }
    ImGui::Separator();
    for (const Choice& choice : all) {
        if (choice.kind != Choice::WolfeSpeaker && choice.kind != Choice::WolfeMic) continue;
        if (ImGui::MenuItem((choice.title + "...").c_str())) choose(choice, screenPosition);
    }
    ImGui::Separator();
    // The computer's own, those not on the canvas yet.
    for (const Choice::Kind kind : {Choice::Microphone, Choice::Speaker}) {
        if (!ImGui::BeginMenu(kind == Choice::Microphone ? "Microphones" : "Speakers and headphones")) continue;
        bool any = false;
        for (const Choice& choice : all) {
            if (choice.kind != kind) continue;
            any = true;
            if (ImGui::MenuItem(choice.title.substr(choice.title.find(": ") + 2).c_str())) choose(choice, screenPosition);
        }
        if (!any) ImGui::TextDisabled("All of them are on the canvas already.");
        ImGui::EndMenu();
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
        "##query", "Add: gain, reverb, microphone, Wolfe speaker...", query_, sizeof(query_),
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
            covered |= std::fabs(there.x - position.x) < kNodeWidth * view() && std::fabs(there.y - position.y) < 70.0f * view();
        }
        if (!covered) break;
        position.y += 90.0f * view();
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
    const float width = kNodeWidth * view();
    const bool trouble = node.active && !node.healthy;

    ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColour(node, 0.0f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, titleColour(node, 0.08f));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColour(node, 0.14f));
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, trouble ? IM_COL32(235, 150, 60, 255) : IM_COL32(18, 19, 23, 255));
    ImNodes::BeginNode(node.uid);

    std::string title = node.label, second = node.detail;
    if (node.isDevice()) splitDeviceName(node.label, title, second);

    // The title, and at its right a "?" that says what the box is for and the
    // mute switch, red while muted.
    ImNodes::BeginNodeTitleBar();
    const float titleX = ImGui::GetCursorPosX();
    const float button = ImGui::GetFontSize(), gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::TextUnformatted(fit(title, width - 2.0f * button - 2.0f * gap).c_str());
    ImGui::PushID(node.uid);
    // (SameLine with a position would count it from elsewhere inside the node's group.)
    ImGui::SameLine();
    ImGui::SetCursorPosX(titleX + width - 2.0f * button - gap * 0.5f);
    ImGui::InvisibleButton("##help", ImVec2(button, button));
    const bool helpHovered = ImGui::IsItemHovered();
    drawHelpIcon(ImGui::GetItemRectMin(), button, helpHovered);
    if (helpHovered) helpTip_ = Controller::helpFor(node);
    ImGui::SameLine();
    ImGui::SetCursorPosX(titleX + width - button);
    if (ImGui::InvisibleButton("##mute", ImVec2(button, button))) controller_.setMuted(node.uid, !node.muted);
    drawSpeakerIcon(ImGui::GetItemRectMin(), button, node.muted, ImGui::IsItemHovered());
    ImGui::PopID();
    ImNodes::EndNodeTitleBar();
    if (!second.empty()) ImGui::TextUnformatted(fit(second, width).c_str());

    // The applications that have audio flowing on its device right now (the
    // tooltip lists the others too). The line is there even when there are
    // none, so that the box does not change size as they come and go.
    if (controller_.showApps() && node.kind != NodeKind::Effect) {
        std::vector<std::string> names;
        for (const AppUse& app : node.apps) {
            if (app.active) names.push_back(app.name);
        }
        const bool recording = node.kind == NodeKind::Input || node.kind == NodeKind::WolfeMic;
        if (names.empty()) ImGui::TextDisabled("%s", recording ? "no app recording" : "no app playing");
        else ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.88f, 1.0f), "%s", namesThatFit(names, width).c_str());
    }

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (node.kind == NodeKind::Effect) {
        // One field per setting, dragged left or right or typed after a double
        // click; the first between the pins:  in [value] out.
        const EffectType* type = findEffectType(node.type);
        const int count = type ? (int)std::min(type->parameters.size(), node.parameters.size()) : 0;
        const float in = ImGui::CalcTextSize("in").x, out = ImGui::CalcTextSize("out").x;
        const float fieldWidth = width - in - out - 2.0f * spacing;
        float fieldX = 0.0f;
        ImGui::PushID(node.uid);
        for (int i = 0; i < std::max(count, 1); i++) {
            if (i == 0) {
                ImNodes::BeginInputAttribute(inputPin(node.uid));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("in");
                ImNodes::EndInputAttribute();
                ImGui::SameLine();
                fieldX = ImGui::GetCursorPosX();
            } else {
                ImGui::SetCursorPosX(fieldX);
            }
            if (count == 0) {
                ImGui::Dummy(ImVec2(fieldWidth, ImGui::GetFrameHeight()));
            } else {
                float value = node.parameters[i];
                ImGui::PushID(i);
                if (drawSetting(type->parameters[i], value, fieldWidth)) controller_.setParameter(node.uid, i, value);
                ImGui::PopID();
            }
            if (i == 0) {
                ImGui::SameLine();
                ImNodes::BeginOutputAttribute(outputPin(node.uid));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("out");
                ImNodes::EndOutputAttribute();
            }
        }

        // A selector: the sources linked into it, one line each; a click on
        // one lets it through instead of the others.
        if (node.isSelector()) {
            const std::vector<int> inputs = controller_.inputsOf(node.uid);
            const int chosen = controller_.selectedInput(node.uid);
            if (inputs.empty()) ImGui::TextDisabled("%s", fit("link sources into \"in\"", width).c_str());
            for (int from : inputs) {
                const Node* source = controller_.node(from);
                if (!source) continue;
                std::string name = source->label, adapter;
                if (source->isDevice()) splitDeviceName(source->label, name, adapter);
                ImGui::PushID(from);
                const float radio = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
                if (ImGui::RadioButton(fit(name, width - radio).c_str(), from == chosen)) controller_.select(node.uid, from);
                ImGui::PopID();
            }
        }
        ImGui::PopID();

        // What goes in (with the level the effect acts at marked on it) or what comes out.
        if (type && metersItsInput(node.type) && !node.parameters.empty()) {
            drawMeter(node.inputLevel, width, 5.0f * view(), node.active, "in ", node.parameters[0]);
        } else {
            drawMeter(node.level, width, 5.0f * view(), node.active);
        }
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
            // (A Wolfe microphone passes on what goes into it: "out" at the right.)
            const float rowX = ImGui::GetCursorPosX();
            const float out = node.givesLinks() ? ImGui::CalcTextSize("out").x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
            ImNodes::BeginInputAttribute(inputPin(node.uid));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - out);
            ImGui::TextColored(colour, "%s", state.c_str());
            ImGui::PopTextWrapPos();
            ImNodes::EndInputAttribute();
            if (node.givesLinks()) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(rowX + width - ImGui::CalcTextSize("out").x);
                ImNodes::BeginOutputAttribute(outputPin(node.uid));
                ImGui::TextUnformatted("out");
                ImNodes::EndOutputAttribute();
            }
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
        drawMeter(node.level, width, 5.0f * view(), node.present);
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

    applyZoom();
    // Everything on the canvas in view: after an import or the example.
    if (showAllWanted_ && !controller_.nodes().empty()) {
        showAllWanted_ = false;
        float left = FLT_MAX, top = FLT_MAX;
        for (const Node& node : controller_.nodes()) {
            left = std::min(left, node.x);
            top = std::min(top, node.y);
        }
        const float margin = 40.0f * scale_;
        ImNodes::EditorContextResetPanning(ImVec2(margin - left * view(), margin - top * view()));
    }
    helpTip_.clear();
    ImNodes::BeginNodeEditor();
    gridOrigin_ = ImGui::GetCursorScreenPos();
    // Inside the boxes, text and spacing at the zoom's size.
    const ImGuiStyle& look = ImGui::GetStyle();
    ImGui::PushFont(nullptr, look.FontSizeBase * zoom_);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(look.ItemSpacing.x * zoom_, look.ItemSpacing.y * zoom_));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(look.ItemInnerSpacing.x * zoom_, look.ItemInnerSpacing.y * zoom_));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(look.FramePadding.x * zoom_, look.FramePadding.y * zoom_));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, look.FrameRounding * zoom_);
    for (const Node& node : controller_.nodes()) {
        if (!placed_.count(node.uid) && node.hasPosition) {
            ImNodes::SetNodeGridSpacePos(node.uid, ImVec2(node.x * view(), node.y * view()));
            placed_.insert(node.uid);
        }
        drawNode(node);
    }
    // (A link that was pulled off its pin is in the user's hand, not here.)
    // Grey when no sound goes through it, because of a mute here or anywhere
    // before it, or a selector; a link muted itself is a dull red.
    const std::set<int> silent = controller_.silentLinks();
    for (const Connection& link : controller_.links()) {
        if (link.uid == heldLink_) continue;
        const bool quiet = link.muted || silent.count(link.uid);
        if (quiet) ImNodes::PushColorStyle(ImNodesCol_Link, link.muted ? IM_COL32(150, 82, 88, 255) : IM_COL32(95, 100, 112, 255));
        ImNodes::Link(link.uid, outputPin(link.from), inputPin(link.to));
        if (quiet) ImNodes::PopColorStyle();
    }
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    // (imnodes' IsEditorHovered() asks this window, not the canvas' own one inside it.)
    overCanvas_ = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
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
        controller_.moveNode(node.uid, position.x / view(), position.y / view());
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
        const std::string text = !helpTip_.empty() ? helpTip_
                                 : overNode       ? controller_.describeNode(hoveredNode)
                                 : overLink       ? controller_.describeLink(hoveredLink)
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
            const EffectType* type = node->kind == NodeKind::Effect ? findEffectType(node->type) : nullptr;
            bool changed = false;
            for (size_t i = 0; type && i < type->parameters.size() && i < node->parameters.size(); i++) {
                changed |= node->parameters[i] != type->parameters[i].initial;
            }
            const bool connected = controller_.linked(uid);
            const bool muted = node->muted;
            ImGui::TextDisabled("%s", node->label.c_str());
            ImGui::Separator();
            // Each of these may change the list of nodes: `node` is not used after them.
            if (ImGui::MenuItem("Mute", nullptr, muted)) {
                controller_.setMuted(uid, !muted);
            } else if (type && !type->parameters.empty() && ImGui::MenuItem("Reset values", nullptr, false, changed)) {
                controller_.resetParameters(uid);
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
            const bool muted = link->muted;
            ImGui::TextDisabled("%s  ->  %s", controller_.node(link->from)->label.c_str(),
                                controller_.node(link->to)->label.c_str());
            ImGui::Separator();
            // Each of these may change the list of links: `link` is not used after them.
            if (ImGui::MenuItem("Mute this link", nullptr, muted)) controller_.setLinkMuted(uid, !muted);
            else if (ImGui::MenuItem("Remove this link", "Ctrl+click")) controller_.disconnect(uid);
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleVar();
}

}  // namespace wolfe
