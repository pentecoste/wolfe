#include "app_devices.h"

#include "common.h"

namespace wolfe {

namespace {

// The class is Windows.Media.Internal.AudioPolicyConfig; this is the id of its
// interface on Windows 10 21H2 and later. No header describes it, so the
// methods are called by their position in the interface:
//
//    0 ..  2   IUnknown
//    3 ..  5   IInspectable
//    6 .. 24   nineteen methods Wolfe has no use for
//   25         SetPersistedDefaultAudioEndpoint(pid, flow, role, device)
//   26         GetPersistedDefaultAudioEndpoint(pid, flow, role, &device)
//   27         ClearAllPersistedApplicationDefaultEndpoints()
//
// A wrong position would call some other method with these arguments, and the
// one right after these two wipes every per-application choice the user has
// made: the numbers below are not to be changed on a guess.
const GUID kPolicyConfigId = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
const wchar_t* const kPolicyConfigClass = L"Windows.Media.Internal.AudioPolicyConfig";
enum { kSetSlot = 25, kGetSlot = 26 };

using SetFunction = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, int flow, int role, void* device);
using GetFunction = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, int flow, int role, void** device);

// The interface wants a device written the long way.
const wchar_t* const kDevicePrefix = L"\\\\?\\SWD#MMDEVAPI#";
const wchar_t* const kRenderSuffix = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
const wchar_t* const kCaptureSuffix = L"#{2eef81be-33fa-4800-9670-1cd474972c3f}";

// Windows Runtime strings, taken from combase.dll at run time: the toolchain
// has no import library for them.
struct RuntimeStrings {
    HRESULT(WINAPI* create)(LPCWSTR, UINT32, void**) = nullptr;
    HRESULT(WINAPI* destroy)(void*) = nullptr;
    LPCWSTR(WINAPI* raw)(void*, UINT32*) = nullptr;
    HRESULT(WINAPI* factory)(void*, REFIID, void**) = nullptr;

    RuntimeStrings() {
        HMODULE library = LoadLibraryW(L"combase.dll");
        if (!library) return;
        create = (HRESULT(WINAPI*)(LPCWSTR, UINT32, void**))(void*)GetProcAddress(library, "WindowsCreateString");
        destroy = (HRESULT(WINAPI*)(void*))(void*)GetProcAddress(library, "WindowsDeleteString");
        raw = (LPCWSTR(WINAPI*)(void*, UINT32*))(void*)GetProcAddress(library, "WindowsGetStringRawBuffer");
        factory = (HRESULT(WINAPI*)(void*, REFIID, void**))(void*)GetProcAddress(library, "RoGetActivationFactory");
    }
    bool usable() const { return create && destroy && raw && factory; }
};

const RuntimeStrings& runtime() {
    static const RuntimeStrings strings;
    return strings;
}

// The two roles an application's ordinary audio goes by. (Voice calls have a
// third, which Windows' own Settings leave alone as well.)
const ERole kRoles[] = {eConsole, eMultimedia};

}  // namespace

AppDevices::AppDevices() {
    if (!runtime().usable()) return;
    void* className = nullptr;
    if (FAILED(runtime().create(kPolicyConfigClass, (UINT32)wcslen(kPolicyConfigClass), &className))) return;
    void* factory = nullptr;
    const HRESULT result = runtime().factory(className, kPolicyConfigId, &factory);
    runtime().destroy(className);
    if (FAILED(result) || !factory) return;
    factory_ = factory;
}

AppDevices::~AppDevices() {
    if (factory_) ((IUnknown*)factory_)->Release();
}

bool AppDevices::set(DWORD pid, EDataFlow flow, const std::wstring& deviceId) {
    if (!factory_) return false;

    // Before changing anything for a process, reading must work for it: that
    // shows both that this is the interface described above and that Windows
    // knows the process as one that uses audio (it refuses the others).
    void* const untouched = (void*)(UINT_PTR)0x1;
    void* current = untouched;
    void** methods = *(void***)factory_;
    const HRESULT read = ((GetFunction)methods[kGetSlot])(factory_, pid, flow, eMultimedia, &current);
    if (FAILED(read) || current == untouched) return false;
    if (current) runtime().destroy(current);

    void* device = nullptr;  // a null string means "the default device"
    if (!deviceId.empty()) {
        const std::wstring full = kDevicePrefix + deviceId + (flow == eRender ? kRenderSuffix : kCaptureSuffix);
        if (FAILED(runtime().create(full.c_str(), (UINT32)full.size(), &device))) return false;
    }
    bool ok = true;
    for (ERole role : kRoles) {
        ok &= SUCCEEDED(((SetFunction)methods[kSetSlot])(factory_, pid, flow, role, device));
    }
    if (device) runtime().destroy(device);
    return ok;
}

std::wstring AppDevices::get(DWORD pid, EDataFlow flow) {
    if (!factory_) return {};
    void* device = nullptr;
    void** methods = *(void***)factory_;
    if (FAILED(((GetFunction)methods[kGetSlot])(factory_, pid, flow, eMultimedia, &device)) || !device) return {};
    UINT32 length = 0;
    const wchar_t* text = runtime().raw(device, &length);
    std::wstring id(text, length);
    runtime().destroy(device);

    // Back to the short form the rest of the program uses.
    const std::wstring prefix = kDevicePrefix;
    if (id.compare(0, prefix.size(), prefix) == 0) id.erase(0, prefix.size());
    const size_t suffix = id.rfind(L"#{");
    if (suffix != std::wstring::npos) id.erase(suffix);
    return id;
}

}  // namespace wolfe
