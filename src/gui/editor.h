#pragma once

#include "app/controller.h"

#include "imgui.h"

#include <set>
#include <string>

namespace wolfe {

// Draws the window: the canvas with its boxes and links, the menus, the line
// of text at the bottom. Everything it changes goes through the Controller.
class Editor {
public:
    // `scale` is 1 on a standard screen, more on a high-density one.
    Editor(Controller& controller, float scale);

    // One frame. Call between ImGui::NewFrame() and ImGui::Render().
    void draw(double now);

    // Shown at the bottom of the window for a few seconds.
    void notify(const std::string& message);

    HWND window = nullptr;       // owner of the "open file" window
    bool exitRequested = false;  // the user chose Exit

private:
    void drawMenuBar();
    void drawCanvas();
    void drawNode(const Node& node);
    void drawMenus();
    void drawStatusLine();
    void addEffectAt(const std::string& type, ImVec2 screenPosition);
    void addItems(ImVec2 screenPosition);
    void askToRemove(int nodeUid);
    void placeNew(int uid, ImVec2 screenPosition);
    void drawDialogs();

    Controller& controller_;
    float scale_;
    double now_ = 0;

    std::set<int> placed_;  // nodes the canvas has already been told the position of
    ImVec2 canvasCentre_;
    bool anythingShown_ = false;  // the last frame drew at least one box

    // A link the user has taken hold of and not yet let go: where, and
    // whether with Ctrl (which makes letting go anywhere a removal).
    int heldLink_ = 0;
    ImVec2 heldAt_;
    bool heldWithCtrl_ = false;
    int selectNext_ = 0;      // a link that was only clicked: selected once it is back on the canvas
    bool grabLinks_ = true;   // read by the canvas at each click: does a click on a link take hold of it?
    bool overPin_ = false;    // the mouse was over a pin at the end of the last frame
    int menuNode_ = 0, menuLink_ = 0;
    ImVec2 menuPosition_;

    // A new device of Wolfe's being named: 1 a speaker, 2 a microphone (0: none).
    int naming_ = 0;
    bool namingOpened_ = false;
    char name_[64] = "";
    std::string nameProblem_;
    ImVec2 namingAt_;
    int removing_ = 0;  // a device of Wolfe's the user asked to remove, to be confirmed
    bool removingOpened_ = false;

    std::string notice_;
    double noticeUntil_ = 0;
};

}  // namespace wolfe
