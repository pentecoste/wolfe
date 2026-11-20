#include "devices.h"

#include "common.h"
#include "virtual_devices.h"

#include <cfgmgr32.h>

#include <cmath>
#include <cstdio>

namespace wolfe {

namespace {

const PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// The device instance of the hardware an endpoint belongs to, as
// "{1}.USB\VID_1209&PID_0003&MI_00\3&3703FCA5&0&0000".
const PROPERTYKEY kHardwareInstanceKey = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

ComPtr<IMMDeviceEnumerator> createEnumerator() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                           enumerator.putVoid()),
          "create device enumerator");
    return enumerator;
}

std::wstring deviceId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return {};
    std::wstring id = raw;
    CoTaskMemFree(raw);
    return id;
}

}  // namespace

std::string deviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) return "?";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name = "?";
    if (SUCCEEDED(properties->GetValue(kFriendlyNameKey, &value)) && value.vt == VT_LPWSTR) {
        name = toUtf8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

std::string wolfeSerialOf(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring instance;
    if (SUCCEEDED(properties->GetValue(kHardwareInstanceKey, &value)) && value.vt == VT_LPWSTR) instance = value.pwszVal;
    PropVariantClear(&value);

    // The endpoint belongs to the audio function of the USB device, whose
    // parent is the device itself: "USB\VID_1209&PID_0003\<serial>".
    const size_t prefix = instance.find(L"}.");
    if (prefix != std::wstring::npos) instance.erase(0, prefix + 2);
    wchar_t wanted[64];
    swprintf(wanted, 64, L"USB\\VID_%04X&PID_", kWolfeVendor);
    if (instance.compare(0, wcslen(wanted), wanted) != 0) return {};

    DEVINST function = 0, parent = 0;
    if (CM_Locate_DevNodeW(&function, instance.data(), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) return {};
    if (CM_Get_Parent(&parent, function, 0) != CR_SUCCESS) return {};
    wchar_t parentId[MAX_DEVICE_ID_LEN + 1] = {};
    if (CM_Get_Device_IDW(parent, parentId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return {};

    std::wstring id = parentId;
    bool ours = false;
    for (uint16_t product : {kWolfeSpeakerProduct, kWolfeMicrophoneProduct}) {
        wchar_t expected[64];
        swprintf(expected, 64, L"USB\\VID_%04X&PID_%04X\\", kWolfeVendor, product);
        if (_wcsnicmp(id.c_str(), expected, wcslen(expected)) == 0) ours = true;
    }
    if (!ours) return {};
    return toUtf8(id.substr(id.find_last_of(L'\\') + 1));
}

ComPtr<IMMDevice> findWolfeDevice(EDataFlow flow, const std::string& serial) {
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) return {};
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;
        if (_stricmp(wolfeSerialOf(device.get()).c_str(), serial.c_str()) == 0 && !serial.empty()) return device;
    }
    return {};
}

std::vector<DeviceInfo> listDevices(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, defaultDevice.put()))) {
        defaultId = deviceId(defaultDevice.get());
    }

    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()), "enumerate devices");
    UINT count = 0;
    check(collection->GetCount(&count), "count devices");

    std::vector<DeviceInfo> devices;
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;
        DeviceInfo info;
        info.name = deviceName(device.get());
        info.id = deviceId(device.get());
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        info.wolfeSerial = wolfeSerialOf(device.get());
        devices.push_back(info);
    }
    return devices;
}

ComPtr<IMMDevice> findDevice(EDataFlow flow, const std::string& spec) {
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    const char* kind = flow == eRender ? "output" : "input";

    if (spec.empty() || toLower(spec) == "default") {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.put()))) {
            throw std::runtime_error(std::string("no default ") + kind + " device");
        }
        return device;
    }

    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()), "enumerate devices");
    UINT count = 0;
    check(collection->GetCount(&count), "count devices");

    const std::wstring wantedId = toWide(spec);
    const std::string wantedName = toLower(spec);
    ComPtr<IMMDevice> byName;
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;
        if (deviceId(device.get()) == wantedId) return device;
        if (!byName && toLower(deviceName(device.get())).find(wantedName) != std::string::npos) byName = device;
    }
    if (!byName) throw std::runtime_error(std::string(kind) + " device \"" + spec + "\" not found");
    return byName;
}

std::vector<DeviceMeter> deviceMeters() {
    std::vector<DeviceMeter> meters;
    ComPtr<IMMDeviceEnumerator> enumerator = createEnumerator();
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) continue;
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put()))) continue;
            DeviceMeter entry;
            entry.id = deviceId(device.get());
            if (SUCCEEDED(device->Activate(kAudioMeterInformationId, CLSCTX_ALL, nullptr, entry.meter.putVoid()))) {
                meters.push_back(entry);
            }
        }
    }
    return meters;
}

WAVEFORMATEXTENSIBLE floatFormat(int channels, DWORD channelMask) {
    if (channelMask == 0) {
        if (channels == 1) channelMask = SPEAKER_FRONT_CENTER;
        else if (channels == 2) channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    WAVEFORMATEXTENSIBLE format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = (WORD)channels;
    format.Format.nSamplesPerSec = kSampleRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = (WORD)(channels * 4);
    format.Format.nAvgBytesPerSec = kSampleRate * channels * 4;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channelMask;
    format.SubFormat = kSubtypeIeeeFloat;
    return format;
}

int devicePeriodFrames(IAudioClient* client) {
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    if (FAILED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod)) || defaultPeriod <= 0) return kQuantum;
    // REFERENCE_TIME counts 100 ns units.
    return (int)std::lround((double)defaultPeriod * kSampleRate / 1e7);
}

}  // namespace wolfe
