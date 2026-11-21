#pragma once

#include "com.h"

#include <mmdeviceapi.h>

#include <set>
#include <string>
#include <vector>

namespace wolfe {

// Windows can give one application its own playback or recording device in
// place of the default one (Settings > System > Sound > Volume mixer). This is
// that same switch.
//
// Windows remembers the choice per program, by the path of its executable,
// across restarts, and also while the device is missing. A stream that is
// already open moves at once when the choice is applied, if the application
// follows the default device; an application that names its device itself
// ignores all this.
//
// Wolfe uses it for one thing only: when one of its devices comes back (Wolfe
// was closed, or started after the applications), an application that was
// given that device but is playing elsewhere because the device was missing
// has its own choice applied again, which moves it back. One that the user
// has put back on the default device has no choice of its own, and is left
// alone.
//
// The interface behind it is the one Windows' own Settings use. It is not
// documented, and exists from Windows 10 21H2 on: on anything older
// available() is false and nothing is changed.
//
// Needs COM on the calling thread.
class AppDevices {
public:
    AppDevices();
    ~AppDevices();
    AppDevices(const AppDevices&) = delete;
    AppDevices& operator=(const AppDevices&) = delete;

    bool available() const { return factory_ != nullptr; }

    // `deviceId` is a device's id as devices.h gives it; empty hands the
    // application back to the default device. False if Windows refused, which
    // it does for a process that has never opened any audio.
    bool set(DWORD pid, EDataFlow flow, const std::wstring& deviceId);

    // The device chosen for the application; empty when it follows the default one.
    std::wstring get(DWORD pid, EDataFlow flow);

private:
    void* factory_ = nullptr;
};

// A stream an application has open, with the device Windows has chosen for
// that application (empty: it follows the default one).
struct AppStream {
    DWORD pid = 0;
    std::wstring device;  // where it plays or records now
    bool recording = false;
    bool active = false;  // audio is flowing
    std::wstring chosen;
};

// The streams that are somewhere other than where their application was put,
// because that device was missing: audio flowing on one device while the
// application's own choice is another, which is one of Wolfe's and is there
// now (`wolfeDevices`). One per process and direction.
std::vector<AppStream> strayStreams(const std::vector<AppStream>& streams, const std::set<std::wstring>& wolfeDevices);

}  // namespace wolfe
