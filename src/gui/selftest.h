#pragma once

#include "app/controller.h"

#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

namespace wolfe {

// Checks the editor's mouse and keyboard gestures by performing them: pretend
// input is fed to the user interface frame by frame, and after each gesture
// the graph is looked at. Started with "wolfe.exe --selftest <report file>",
// on a graph file made for the purpose.
//
// The links that Windows makes are only tried when the environment names a
// running application to try them on, WOLFE_SELFTEST_APP, and a playback
// device to move it to, WOLFE_SELFTEST_DEVICE (parts of their names): that
// application really is moved, taken off its device and put back. Likewise
// "Listen to this device": with WOLFE_SELFTEST_LISTEN naming a recording
// device that Windows is playing somewhere and WOLFE_SELFTEST_LISTEN_TO a
// playback device, that link is moved there and then removed.
class SelfTest {
public:
    SelfTest(Controller& controller, std::string reportPath);

    // Call once per frame, before the frame begins. False once it has finished.
    bool step();

private:
    using Place = std::function<ImVec2()>;  // a screen position, worked out when it is needed

    void wait(int frames);
    void waitSeconds(double seconds);
    void run(std::function<void()> action);
    void moveTo(Place where);
    void press(int button, bool down);
    void click(Place where, int button = 0);
    void ctrlClick(Place where);
    void drag(Place from, Place to);
    void tapKey(ImGuiKey key);
    void check(const char* what, std::function<bool()> condition);
    bool linked(int fromUid, int toUid) const;   // Wolfe carries a link between them
    bool hasLink(int fromUid, int toUid) const;  // ...and it is the only link there is

    Controller& controller_;
    std::string reportPath_;
    std::string report_;
    int failures_ = 0;

    // Each step is called once per frame until it returns true.
    std::vector<std::function<bool()>> steps_;
    size_t current_ = 0;
    int first_ = 0, second_ = 0, third_ = 0;  // the effect nodes the gestures are tried on
    int app_ = 0, from_ = 0, to_ = 0;  // an application, the device it plays on, the one to move it to
    int heard_ = 0, heardOn_ = 0, heardThen_ = 0;  // a recording device, where Windows plays it, where to move that
};

}  // namespace wolfe
