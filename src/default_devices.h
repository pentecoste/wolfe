#pragma once

#include "com.h"

#include <mmdeviceapi.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace wolfe {

// Windows' default playback and recording devices, for each role (console,
// multimedia, communications).
//
// Setting one goes through the interface Windows' own sound settings use. It
// is not documented, but has stayed the same since Windows 7 (the tools that
// switch the default device all use it).
//
// Needs COM on the calling thread.

// The id of the default device for that direction and role; empty if none.
std::wstring defaultDevice(EDataFlow flow, ERole role);
// False if Windows refused or the interface is missing.
bool setDefaultDevice(const std::wstring& deviceId, ERole role);

// When Windows sees a device for the first time it makes it the default one.
// For a device of Wolfe's the user has just made, that is never wanted: this
// notices it and puts back the device that was the default before.
//
// It is told which devices are new (by their Wolfe serial number), and looks
// at the defaults each time the system is looked at. A new device's first
// minute is watched; a role whose default becomes that device in that time is
// given back the one it had, once. Devices the user sets as default later on,
// or that were known before, are left alone.
class DefaultKeeper {
public:
    // A device of Wolfe's that Windows has never seen.
    void expect(const std::string& serial);

    // `present`: the devices there are now, by id, with the Wolfe serial of
    // those that are Wolfe's (empty for the others). `now` in seconds.
    // Returns what was done, one line each, for the log.
    std::vector<std::string> look(EDataFlow flow, const std::map<std::wstring, std::string>& present, double now);

    // For tests: the defaults are given instead of read, and what would be
    // set is returned instead of done.
    struct Change {
        ERole role;
        std::wstring device;
    };
    std::vector<Change> decide(EDataFlow flow, const std::map<std::wstring, std::string>& present,
                               const std::map<ERole, std::wstring>& defaults, double now);

private:
    struct Fresh {
        double seenAt = -1.0;           // when Windows first had it; -1 not yet
        std::set<int> restored;         // flow * 8 + role: done once each
    };
    std::map<std::string, Fresh> fresh_;                // by serial
    std::map<int, std::wstring> before_;                // flow * 8 + role -> the last default that was not a new device
};

}  // namespace wolfe
