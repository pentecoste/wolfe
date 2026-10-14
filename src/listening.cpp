#include "listening.h"

#include "common.h"

#include <mmdeviceapi.h>
#include <propidl.h>
#include <propsys.h>

namespace wolfe {

// The interface of the audio service through which a device's properties are
// changed (see below). It is declared out here, and not among the private
// things of this file, on purpose: of a type that only this file can see the
// compiler concludes that nothing implements it, and turns every call through
// it into a call of nothing at all.
struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR device, INT effectsStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR device, INT effectsStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

namespace {

// The setting lives among the properties of the recording device: one says
// whether it is on, one names the playback device (nothing: the default one).
const GUID kListenProperties = {0x24dbb0fc, 0x9311, 0x4b3d, {0x9c, 0xf0, 0x18, 0xff, 0x15, 0x56, 0x39, 0xd4}};
const PROPERTYKEY kListenTarget = {kListenProperties, 0};
const PROPERTYKEY kListenOn = {kListenProperties, 1};

// Any program may read a device's properties; changing them is done by asking
// the audio service, through the interface the Sound control panel uses. No
// header describes that interface. This is its shape on Windows 7 and later,
// confirmed on Windows 11 by reading a known setting through it; and before
// anything is ever written through it, setListening() reads the same value
// both ways and goes no further if they differ.
const GUID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const GUID kPolicyConfigId = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

ComPtr<IMMDeviceEnumerator> createEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.putVoid());
    return enumerator;
}

// One of a device's properties, read the documented way. False when it cannot be read at all.
bool readProperty(IMMDevice* device, const PROPERTYKEY& key, PROPVARIANT* value) {
    PropVariantInit(value);
    ComPtr<IPropertyStore> store;
    return SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put())) && SUCCEEDED(store->GetValue(key, value));
}

}  // namespace

std::vector<Listening> listListening() {
    std::vector<Listening> result;
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    ComPtr<IMMDeviceCollection> devices;
    if (!enumerator || FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, devices.put()))) return result;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put()))) continue;
        PROPVARIANT on, target;
        const bool listening = readProperty(device.get(), kListenOn, &on) && on.vt == VT_BOOL && on.boolVal != VARIANT_FALSE;
        PropVariantClear(&on);
        if (!listening) continue;

        Listening entry;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            entry.input = id;
            CoTaskMemFree(id);
        }
        if (readProperty(device.get(), kListenTarget, &target) && target.vt == VT_LPWSTR && target.pwszVal) {
            entry.output = target.pwszVal;
        }
        PropVariantClear(&target);
        if (!entry.input.empty()) result.push_back(entry);
    }
    return result;
}

bool setListening(const std::wstring& inputId, bool on, const std::wstring& outputId) {
    if (inputId.empty() || (on && outputId.empty())) return false;
    ComPtr<IPolicyConfig> config;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, kPolicyConfigId, config.putVoid())) || !config) {
        return false;
    }

    // The interface must read what the documented way reads.
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    ComPtr<IMMDevice> device;
    if (!enumerator || FAILED(enumerator->GetDevice(inputId.c_str(), device.put()))) return false;
    PROPVARIANT documented, asked;
    PropVariantInit(&asked);
    const bool readable = readProperty(device.get(), kListenOn, &documented) &&
                          SUCCEEDED(config->GetPropertyValue(inputId.c_str(), 0, kListenOn, &asked));
    const bool agree = readable && documented.vt == asked.vt &&
                       (documented.vt == VT_EMPTY || (documented.vt == VT_BOOL && documented.boolVal == asked.boolVal));
    PropVariantClear(&documented);
    PropVariantClear(&asked);
    if (!agree) {
        logLine("\"Listen to this device\": Windows does not answer as expected, leaving it alone");
        return false;
    }

    if (on) {
        PROPVARIANT target;
        PropVariantInit(&target);
        target.vt = VT_LPWSTR;
        target.pwszVal = const_cast<wchar_t*>(outputId.c_str());  // only read by the call
        if (FAILED(config->SetPropertyValue(inputId.c_str(), 0, kListenTarget, &target))) return false;
    }
    PROPVARIANT state;
    PropVariantInit(&state);
    state.vt = VT_BOOL;
    state.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
    return SUCCEEDED(config->SetPropertyValue(inputId.c_str(), 0, kListenOn, &state));
}

}  // namespace wolfe
