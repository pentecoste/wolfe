#include "process_finder.h"

#include "com.h"
#include "common.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <winver.h>

#include <algorithm>
#include <set>

namespace wolfe {

namespace {

// When the process started, or 0 if that cannot be asked.
ULONGLONG startTime(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return 0;
    FILETIME created, exited, kernel, user;
    ULONGLONG result = 0;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        result = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
    }
    CloseHandle(process);
    return result;
}

}  // namespace

ProcessSnapshot::ProcessSnapshot() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W item;
    item.dwSize = sizeof(item);
    for (BOOL more = Process32FirstW(snapshot, &item); more; more = Process32NextW(snapshot, &item)) {
        entries_[item.th32ProcessID] = {item.th32ParentProcessID, toUtf8(item.szExeFile)};
    }
    CloseHandle(snapshot);
}

const std::string& ProcessSnapshot::path(DWORD pid) {
    auto cached = paths_.find(pid);
    if (cached != paths_.end()) return cached->second;

    std::string result;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t text[1024];
        DWORD size = 1024;
        if (QueryFullProcessImageNameW(process, 0, text, &size)) result = toUtf8(std::wstring(text, size));
        CloseHandle(process);
    }
    return paths_[pid] = result;
}

std::string ProcessSnapshot::name(DWORD pid) const {
    auto found = entries_.find(pid);
    return found == entries_.end() ? std::string() : found->second.name;
}

DWORD ProcessSnapshot::parent(DWORD pid) const {
    auto found = entries_.find(pid);
    return found == entries_.end() ? 0 : found->second.parent;
}

std::map<DWORD, int> ProcessSnapshot::family(const std::string& spec) {
    std::string wanted = toLower(spec);
    std::replace(wanted.begin(), wanted.end(), '/', '\\');
    std::map<DWORD, int> depth;
    const DWORD self = GetCurrentProcessId();

    if (wanted.rfind("pid:", 0) == 0) {
        DWORD pid = (DWORD)atoi(wanted.c_str() + 4);
        if (entries_.count(pid)) depth[pid] = 0;
        return depth;
    }

    // 1. The processes that fit the description.
    const bool byPath = wanted.find('\\') != std::string::npos;
    std::set<DWORD> matching;
    for (const auto& [pid, entry] : entries_) {
        if (pid == self) continue;
        bool matches;
        if (byPath) {
            matches = toLower(path(pid)).find(wanted) != std::string::npos;
        } else {
            std::string name = toLower(entry.name);
            matches = name == wanted || name == wanted + ".exe";
        }
        if (matches) matching.insert(pid);
    }

    // 2. Those at the top (not started by another matching process), then
    //    everything started by a member, level by level.
    for (DWORD pid : matching) {
        if (!matching.count(entries_[pid].parent)) depth[pid] = 0;
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& [pid, entry] : entries_) {
            if (depth.count(pid) || !depth.count(entry.parent) || pid == entry.parent || pid == self) continue;
            // Process ids are reused: a "parent" younger than its "child" is
            // some unrelated process that got the dead parent's number.
            ULONGLONG parentStart = startTime(entry.parent), childStart = startTime(pid);
            if (parentStart && childStart && childStart < parentStart) continue;
            depth[pid] = depth[entry.parent] + 1;
            grew = true;
        }
    }
    return depth;
}

std::vector<AudioSession> audioSessions() {
    std::vector<AudioSession> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                enumerator.putVoid()))) {
        return result;
    }
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> devices;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put()))) continue;
        UINT deviceCount = 0;
        devices->GetCount(&deviceCount);
        for (UINT d = 0; d < deviceCount; d++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(d, device.put()))) continue;
            LPWSTR rawId = nullptr;
            if (FAILED(device->GetId(&rawId)) || !rawId) continue;
            const std::wstring deviceId = rawId;
            CoTaskMemFree(rawId);

            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid()))) continue;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(manager->GetSessionEnumerator(sessions.put()))) continue;
            int sessionCount = 0;
            sessions->GetCount(&sessionCount);
            for (int i = 0; i < sessionCount; i++) {
                ComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(i, control.put()))) continue;
                AudioSessionState state = AudioSessionStateExpired;
                control->GetState(&state);
                if (state == AudioSessionStateExpired) continue;
                ComPtr<IAudioSessionControl2> control2;
                if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.putVoid()))) continue;
                DWORD pid = 0;
                control2->GetProcessId(&pid);
                if (pid == 0) continue;  // the system's own sounds
                AudioSession session;
                session.pid = pid;
                session.device = deviceId;
                session.recording = flow == eCapture;
                session.active = state == AudioSessionStateActive;
                control->QueryInterface(kAudioMeterInformationId, session.meter.putVoid());
                result.push_back(session);
            }
        }
    }
    return result;
}

std::string programName(const std::string& exePath) {
    const std::wstring path = toWide(exePath);
    std::string name;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size > 0) {
        std::vector<char> data(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
            // The description in the first language the file has.
            struct Translation {
                WORD language, codePage;
            };
            Translation* translations = nullptr;
            UINT length = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&translations, &length) &&
                length >= sizeof(Translation)) {
                wchar_t key[64];
                swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language,
                         translations[0].codePage);
                wchar_t* text = nullptr;
                UINT chars = 0;
                if (VerQueryValueW(data.data(), key, (void**)&text, &chars) && text && chars > 1) name = toUtf8(text);
            }
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (!name.empty()) return name;
    size_t slash = exePath.find_last_of('\\');
    name = slash == std::string::npos ? exePath : exePath.substr(slash + 1);
    if (name.size() > 4 && toLower(name.substr(name.size() - 4)) == ".exe") name.resize(name.size() - 4);
    return name;
}

std::vector<DWORD> processesPlayingAudio() {
    std::set<DWORD> pids;
    for (const AudioSession& session : audioSessions()) {
        if (!session.recording) pids.insert(session.pid);
    }
    return std::vector<DWORD>(pids.begin(), pids.end());
}

FoundProcess chooseProcess(const std::string& spec, DWORD current) {
    ProcessSnapshot snapshot;
    std::map<DWORD, int> depth = snapshot.family(spec);
    if (depth.empty()) return {};

    auto found = [&](DWORD pid) { return FoundProcess{pid, snapshot.name(pid)}; };
    auto topMost = [&] {
        DWORD best = depth.begin()->first;
        for (const auto& [pid, level] : depth) {
            if (level < depth[best]) best = pid;
        }
        return best;
    };

    // The members that are playing something.
    std::vector<DWORD> playing;
    for (DWORD pid : processesPlayingAudio()) {
        if (depth.count(pid)) playing.push_back(pid);
    }
    if (playing.empty()) return found(depth.count(current) ? current : topMost());

    // Capturing a member covers itself and the processes it started directly.
    auto covered = [&](DWORD pid) {
        int count = 0;
        for (DWORD player : playing) {
            if (player == pid || snapshot.parent(player) == pid) count++;
        }
        return count;
    };
    auto isPlaying = [&](DWORD pid) { return std::find(playing.begin(), playing.end(), pid) != playing.end(); };

    // Most covered first; then one that plays itself; then the one nearest the top.
    DWORD best = 0;
    for (const auto& [pid, level] : depth) {
        if (best == 0) {
            best = pid;
            continue;
        }
        const int mine = covered(pid), theirs = covered(best);
        if (mine != theirs) {
            if (mine > theirs) best = pid;
        } else if (isPlaying(pid) != isPlaying(best)) {
            if (isPlaying(pid)) best = pid;
        } else if (level < depth[best]) {
            best = pid;
        }
    }
    if (depth.count(current) && covered(current) == covered(best)) return found(current);
    return found(best);
}

}  // namespace wolfe
