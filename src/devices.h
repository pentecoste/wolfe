#pragma once

#include "com.h"
#include "level_meter.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace wolfe {

struct DeviceInfo {
    std::string name;
    std::wstring id;
    bool isDefault = false;
    std::string wolfeSerial;  // set if this is one of Wolfe's own devices (see wolfeSerialOf)
};

// eRender = outputs, eCapture = inputs. Only devices that are currently usable.
std::vector<DeviceInfo> listDevices(EDataFlow flow);

std::string deviceName(IMMDevice* device);

// If the endpoint is one of Wolfe's own devices (see VirtualDevices), the
// serial number it was given; empty for any other device.
std::string wolfeSerialOf(IMMDevice* device);
// The endpoint of Wolfe's own device with this serial number, or null while
// Windows does not have it.
ComPtr<IMMDevice> findWolfeDevice(EDataFlow flow, const std::string& serial);

// `spec` is "default" (or empty), an exact endpoint id, or a case-insensitive
// fragment of the device name. Throws std::runtime_error if nothing matches.
ComPtr<IMMDevice> findDevice(EDataFlow flow, const std::string& spec);

// The level meter of every usable device, playback and recording, by device
// id. A recording device only shows a level while something is recording from it.
struct DeviceMeter {
    std::wstring id;
    ComPtr<IAudioMeterInformation> meter;
};
std::vector<DeviceMeter> deviceMeters();

// 48 kHz 32-bit float with the given channel count. A zero mask picks the usual one.
WAVEFORMATEXTENSIBLE floatFormat(int channels, DWORD channelMask = 0);

// 10 ms unless the device says otherwise, expressed in frames at 48 kHz.
int devicePeriodFrames(IAudioClient* client);

}  // namespace wolfe
