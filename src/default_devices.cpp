#include "default_devices.h"

#include "common.h"

namespace wolfe {

namespace {

// CPolicyConfigClient and its IPolicyConfig interface. No header describes
// them, so the method is called by its position in the interface:
//
//    0 ..  2   IUnknown
//    3 .. 12   ten methods about formats, periods and properties
//   13         SetDefaultEndpoint(device, role)
//   14         SetEndpointVisibility(device, visible)
//
// This is the layout every default-switching tool relies on, unchanged from
// Windows 7 to 11.
const CLSID kPolicyConfigClass = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const IID kPolicyConfigId = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
enum { kSetDefaultSlot = 13 };
using SetDefaultFunction = HRESULT(STDMETHODCALLTYPE*)(void* self, PCWSTR device, ERole role);

const ERole kRoles[] = {eConsole, eMultimedia, eCommunications};
int keyOf(EDataFlow flow, ERole role) { return (int)flow * 8 + (int)role; }

const char* roleName(ERole role) {
    return role == eConsole ? "default" : role == eMultimedia ? "multimedia" : "communications";
}

}  // namespace

std::wstring defaultDevice(EDataFlow flow, ERole role) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                enumerator.putVoid()))) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, device.put()))) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
}

bool setDefaultDevice(const std::wstring& deviceId, ERole role) {
    void* config = nullptr;
    if (FAILED(CoCreateInstance(kPolicyConfigClass, nullptr, CLSCTX_ALL, kPolicyConfigId, &config)) || !config) return false;
    void** table = *(void***)config;
    const HRESULT hr = ((SetDefaultFunction)table[kSetDefaultSlot])(config, deviceId.c_str(), role);
    ((IUnknown*)config)->Release();
    return SUCCEEDED(hr);
}

void DefaultKeeper::expect(const std::string& serial) {
    if (!serial.empty()) fresh_[toLower(serial)] = Fresh();
}

std::vector<DefaultKeeper::Change> DefaultKeeper::decide(EDataFlow flow, const std::map<std::wstring, std::string>& present,
                                                         const std::map<ERole, std::wstring>& defaults, double now) {
    // A new device is watched for a minute from when Windows first has it.
    for (const auto& [id, serial] : present) {
        auto found = fresh_.find(toLower(serial));
        if (!serial.empty() && found != fresh_.end() && found->second.seenAt < 0) found->second.seenAt = now;
    }
    for (auto it = fresh_.begin(); it != fresh_.end();) {
        if (it->second.seenAt >= 0 && now - it->second.seenAt > 60.0) it = fresh_.erase(it);
        else ++it;
    }

    std::vector<Change> changes;
    for (ERole role : kRoles) {
        auto current = defaults.find(role);
        if (current == defaults.end() || current->second.empty()) continue;
        const int key = keyOf(flow, role);
        auto device = present.find(current->second);
        auto fresh = device == present.end() || device->second.empty() ? fresh_.end() : fresh_.find(toLower(device->second));
        if (fresh == fresh_.end() || fresh->second.seenAt < 0) {
            before_[key] = current->second;  // an ordinary default: remembered, in case a new device takes its place
            continue;
        }
        if (!fresh->second.restored.insert(key).second) continue;  // put back once; afterwards it is the user's choice
        auto before = before_.find(key);
        if (before != before_.end() && before->second != current->second && present.count(before->second)) {
            changes.push_back({role, before->second});
        }
    }
    return changes;
}

std::vector<std::string> DefaultKeeper::look(EDataFlow flow, const std::map<std::wstring, std::string>& present, double now) {
    std::map<ERole, std::wstring> defaults;
    for (ERole role : kRoles) defaults[role] = defaultDevice(flow, role);
    std::vector<std::string> done;
    for (const Change& change : decide(flow, present, defaults, now)) {
        const bool ok = setDefaultDevice(change.device, change.role);
        done.push_back(std::string(ok ? "put back " : "could not put back ") + "the " + (flow == eRender ? "playback" : "recording") +
                       " device that was the " + roleName(change.role) + " one");
    }
    return done;
}

}  // namespace wolfe
