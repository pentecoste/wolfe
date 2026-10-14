#pragma once

#include "com.h"

#include <mmdeviceapi.h>

#include <string>

namespace wolfe {

// Windows can give one application its own playback or recording device in
// place of the default one (Settings > System > Sound > Volume mixer). This is
// that same switch.
//
// Windows remembers the choice per program, by the path of its executable:
// it holds for every copy of the program and across restarts. A stream that
// is already open moves at once only if the application follows the default
// device; the others pick the new device up the next time they open their
// audio, and an application that names its device itself ignores all this.
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

}  // namespace wolfe
