#pragma once

#include "com.h"
#include "level_meter.h"

#include <map>
#include <string>
#include <vector>

namespace wolfe {

// An application is described by an executable name ("chrome.exe"), a
// fragment of its full path ("\\Soundrio\\") or "pid:1234".
//
// It is often several processes: a launcher that starts a command prompt that
// starts the real program, a browser and its helpers. Its family is every
// process that fits the description plus everything those started.

// The processes running at one moment. Looking up several applications in the
// same snapshot costs little more than looking up one.
class ProcessSnapshot {
public:
    ProcessSnapshot();

    // The family of an application: each member with how many steps below the
    // top of the family it was started. Empty when it is not running.
    std::map<DWORD, int> family(const std::string& spec);

    // Full path of a process's executable; empty when it cannot be read.
    const std::string& path(DWORD pid);
    std::string name(DWORD pid) const;
    DWORD parent(DWORD pid) const;

private:
    struct Entry {
        DWORD parent;
        std::string name;
    };
    std::map<DWORD, Entry> entries_;
    std::map<DWORD, std::string> paths_;
};

struct FoundProcess {
    DWORD pid = 0;  // 0: nothing found
    std::string name;
};

// Picks the one process to capture for an application.
//
// Windows captures a process together with the processes it started directly,
// but not the ones those started in turn. So the whole family is looked at,
// and the process chosen is the one whose capture covers the most members
// that are actually playing audio.
//
// `current` is the process being captured now (0 if none); it is kept as long
// as nothing else would be better. Needs COM on the calling thread.
FoundProcess chooseProcess(const std::string& spec, DWORD current);

// A stream that a process has open on a device right now: this is how Windows
// itself has things connected.
struct AudioSession {
    DWORD pid = 0;
    std::wstring device;     // the device's id
    bool recording = false;  // false: the process plays on the device; true: it records from it
    bool active = false;     // audio is flowing, as opposed to a stream that is merely open
    ComPtr<IAudioMeterInformation> meter;  // the level of this stream alone
};

// Every such stream on every device. Needs COM on the calling thread.
std::vector<AudioSession> audioSessions();

// The processes that have a playback stream open on some device right now.
// Needs COM on the calling thread.
std::vector<DWORD> processesPlayingAudio();

}  // namespace wolfe
