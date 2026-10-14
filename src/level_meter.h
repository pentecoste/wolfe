#pragma once

#include <windows.h>

// Windows measures the level of every device and of every stream an
// application has open; this is the interface it is read through. The MinGW
// headers only announce it (in endpointvolume.h), they do not define it.
struct IAudioMeterInformation : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;  // 0..1, since the last period
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

inline const GUID kAudioMeterInformationId = {
    0xc02216f6, 0x8c67, 0x4b5b, {0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64}};
