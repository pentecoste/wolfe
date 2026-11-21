#pragma once

#include "app/controller.h"

#include "imgui.h"
#include "imnodes.h"

#include <set>
#include <string>
#include <vector>

namespace wolfe {

// Draws the window: the menu bar and the canvas with its boxes and links.
// Everything it changes goes through the Controller.
class Editor {
public:
    // `scale` is 1 on a standard screen, more on a high-density one.
    Editor(Controller& controller, float scale);

    // One frame. Call between ImGui::NewFrame() and ImGui::Render().
    void draw(double now);

    bool exitRequested = false;  // the user chose Exit
    // The user chose Import or Export: the program asks for the file between
    // two frames, then sets this back to None.
    enum class FileRequest { None, Import, Export };
    FileRequest fileRequest = FileRequest::None;

    // The window that explains the basics and offers the example.
    void showWelcome();
    // Moves the view so that every box is in it.
    void showAll() { showAllWanted_ = true; }
    // Says something next to the mouse for a few seconds (a problem, mostly).
    void flash(const std::string& message);

private:
    // Something that can be added to the canvas, as the search lists it.
    struct Choice {
        enum Kind { Effect, WolfeSpeaker, WolfeMic, Microphone, Speaker } kind;
        std::string title;
        std::string hint;    // shown greyed after the title
        std::string words;   // what a search matches, lower case
        std::string target;  // Effect: its type; Microphone and Speaker: the device's id
    };

    void drawMenuBar();
    void drawCanvas();
    void drawNode(const Node& node);
    void drawMenus();
    void drawSearch();
    void drawDialogs();
    void drawFlash();
    void addItems(ImVec2 screenPosition);
    std::vector<Choice> choices() const;
    void choose(const Choice& choice, ImVec2 screenPosition);
    void openSearch(ImVec2 screenPosition);
    void askToRemove(int nodeUid);
    void placeNew(int uid, ImVec2 screenPosition);
    void drawWelcome();
    // Zoom: steps of 10%, 0 being the actual size.
    void zoomBy(int steps, ImVec2 pivot);
    void zoomTo(int step, ImVec2 pivot);
    void applyZoom();
    float view() const { return scale_ * zoom_; }  // canvas pixels per unit of a saved position
    static int searchKeys(ImGuiInputTextCallbackData* data);

    Controller& controller_;
    float scale_;
    double now_ = 0;

    // The canvas is drawn at `zoom_` times its size: fonts, boxes, spacing
    // and positions alike, so that text stays sharp. A change is asked for
    // (wheel, keys, menu) and made at the start of the next canvas.
    float zoom_ = 1.0f;
    int zoomStep_ = 0;
    int zoomWanted_ = 0;
    float wheel_ = 0.0f;   // wheel turned and not yet made into steps (a touchpad turns it a little at a time)
    ImVec2 zoomPivot_;     // the point of the screen that stays put
    ImVec2 gridOrigin_;    // where the canvas started on the screen, last frame
    ImNodesStyle style_;   // the canvas' style at the actual size

    std::set<int> placed_;  // nodes the canvas has already been told the position of
    ImVec2 canvasCentre_;
    bool overCanvas_ = false;  // the mouse was over the canvas at the end of the last frame

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

    // The search for something to add (Ctrl+A).
    bool searchWanted_ = false;  // to be opened at the next chance
    ImVec2 searchAt_;            // where what is chosen goes
    char query_[128] = "";
    int selected_ = 0;
    int shownChoices_ = 0;
    bool scrollToSelected_ = false;

    // A new device of Wolfe's being named: 1 a speaker, 2 a microphone (0: none).
    int naming_ = 0;
    bool namingOpened_ = false;
    char name_[64] = "";
    std::string nameProblem_;
    ImVec2 namingAt_;
    int removing_ = 0;  // a device of Wolfe's the user asked to remove, to be confirmed
    bool removingOpened_ = false;

    std::string flash_;
    double flashUntil_ = 0;

    std::string helpTip_;        // the "?" under the mouse in this frame says this
    bool showAllWanted_ = false;
    bool welcome_ = false, welcomeOpened_ = false;
};

}  // namespace wolfe
