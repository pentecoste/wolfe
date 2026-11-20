// Plays test signals and records devices to WAV files, to measure what the
// engine does to the audio. Not needed to use Wolfe.
//
//   wolfe-sigtool play <output> <seconds> sine <Hz> <dB>
//   wolfe-sigtool play <output> <seconds> clicks <dB>      one click every 500 ms
//   wolfe-sigtool play <output> <seconds> noise <dB>       white noise
//   wolfe-sigtool rec <input> <seconds> <file.wav>
//   wolfe-sigtool rec2 <inputA> <inputB> <seconds> <a.wav> <b.wav>
//   wolfe-sigtool pace <output | in:input> <seconds>       timing of a device
//   wolfe-sigtool sessions [seconds]                       who is playing/recording where
//   wolfe-sigtool volumes                                  every device's own volume and mute
//
// "play follow ..." plays on the default output and moves with it when the
// default changes, as most applications do; "play default ..." stays on the
// device that was the default when it started.
//
// Devices are named like in a Wolfe configuration; an input written as
// "loopback:<output>" records what is being played on that output. Recordings
// are 48 kHz stereo float. rec2 records two inputs at once and prints when
// each one started, so that events in the two files can be compared in time.
#include "com.h"
#include "common.h"
#include "devices.h"
#include "stream_clock.h"

#include <audiopolicy.h>
#include <endpointvolume.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

using namespace wolfe;

namespace {

const DWORD kConvertFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

// Tells when the default output device changes.
struct DefaultOutputWatch : IMMNotificationClient {
    std::atomic<bool> changed{false};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) changed = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

int play(const std::string& deviceSpec, double seconds, const std::string& signal, double frequency, double levelDb) {
    const bool follow = deviceSpec == "follow";
    DefaultOutputWatch watch;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (follow) {
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                               enumerator.putVoid()),
              "enumerator");
        enumerator->RegisterEndpointNotificationCallback(&watch);
    }

    const float amplitude = (float)std::pow(10.0, levelDb / 20.0);
    const double phaseStep = 2.0 * 3.14159265358979323846 * frequency / kSampleRate;
    const bool clicks = signal == "clicks";
    const bool noise = signal == "noise";
    uint32_t noiseState = 0x12345678;
    double phase = 0.0;
    int64_t frame = 0;
    const double end = nowSeconds() + seconds;

    // Once per device: again from the top when the default output changes.
    while (nowSeconds() < end) {
        watch.changed = false;
        ComPtr<IMMDevice> device = findDevice(eRender, follow ? "default" : deviceSpec);
        ComPtr<IAudioClient> client;
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()), "activate");
        WAVEFORMATEXTENSIBLE format = floatFormat(kChannels);
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, kConvertFlags, 1000000, 0, &format.Format, nullptr), "initialize");
        UINT32 bufferFrames = 0;
        check(client->GetBufferSize(&bufferFrames), "buffer size");
        ComPtr<IAudioRenderClient> render;
        check(client->GetService(__uuidof(IAudioRenderClient), render.putVoid()), "render service");
        check(client->Start(), "start");
        fprintf(stderr, "playing %s on %s\n", signal.c_str(), deviceName(device.get()).c_str());
        fflush(stderr);

        while (nowSeconds() < end && !watch.changed) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;  // the device went away
            UINT32 count = bufferFrames - padding;
            BYTE* data = nullptr;
            if (count && SUCCEEDED(render->GetBuffer(count, &data))) {
                float* out = (float*)data;
                for (UINT32 i = 0; i < count; i++, frame++) {
                    float sample;
                    if (clicks) {
                        sample = frame % (kSampleRate / 2) == 0 ? amplitude : 0.0f;
                    } else if (noise) {
                        noiseState = noiseState * 1664525u + 1013904223u;
                        sample = amplitude * ((float)(noiseState >> 8) / 8388608.0f - 1.0f);
                    } else {
                        sample = amplitude * (float)std::sin(phase);
                        phase += phaseStep;
                    }
                    out[i * 2] = out[i * 2 + 1] = sample;
                }
                render->ReleaseBuffer(count, 0);
            }
            Sleep(5);
        }
        client->Stop();
        if (!follow) break;
    }
    if (follow) enumerator->UnregisterEndpointNotificationCallback(&watch);
    return 0;
}

// Opens a device the way the engine does (event driven, 48 kHz float) and
// reports how regularly it asks for or delivers audio, and how far its clock
// is from the computer's. Plays silence / discards what it records.
int pace(std::string spec, double seconds) {
    const bool input = spec.rfind("in:", 0) == 0;
    if (input) spec = spec.substr(3);
    ComPtr<IMMDevice> device = findDevice(input ? eCapture : eRender, spec);
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()), "activate");

    WAVEFORMATEX* mix = nullptr;
    check(client->GetMixFormat(&mix), "mix format");
    const int channels = input ? mix->nChannels : kChannels;
    const DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask : 0;
    printf("%s: device format %lu Hz, %u channels, %u bits\n", deviceName(device.get()).c_str(), mix->nSamplesPerSec,
           mix->nChannels, mix->wBitsPerSample);
    CoTaskMemFree(mix);

    WAVEFORMATEXTENSIBLE format = floatFormat(channels, input ? mask : 0);
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, kConvertFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0,
                             &format.Format, nullptr),
          "initialize");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(client->SetEventHandle(event), "event");
    UINT32 bufferFrames = 0;
    check(client->GetBufferSize(&bufferFrames), "buffer size");
    REFERENCE_TIME latency = 0;
    client->GetStreamLatency(&latency);
    printf("period %d frames, buffer %u frames, stream latency %.1f ms (as the driver declares it)\n",
           devicePeriodFrames(client.get()), bufferFrames, latency / 10000.0);

    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioCaptureClient> capture;
    if (input) {
        check(client->GetService(__uuidof(IAudioCaptureClient), capture.putVoid()), "capture service");
    } else {
        check(client->GetService(__uuidof(IAudioRenderClient), render.putVoid()), "render service");
        BYTE* data = nullptr;
        check(render->GetBuffer(bufferFrames, &data), "get buffer");
        check(render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT), "release buffer");
    }
    check(client->Start(), "start");

    // The first second is left out of the statistics.
    const double start = nowSeconds();
    double measureFrom = -1, last = -1, lastTime = 0;
    int64_t total = 0, totalAtMeasure = 0;
    double minGap = 1e9, maxGap = 0, nextLine = 0;
    int events = 0, minFrames = 1 << 30, maxFrames = 0, emptyEvents = 0;
    while (nowSeconds() - start < seconds + 1.0) {
        if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) continue;
        const double now = nowSeconds();
        int frames = 0;
        if (input) {
            for (;;) {
                UINT32 packet = 0;
                if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;
                BYTE* data = nullptr;
                UINT32 count = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) break;
                capture->ReleaseBuffer(count);
                frames += (int)count;
            }
        } else {
            UINT32 padding = 0;
            check(client->GetCurrentPadding(&padding), "padding");
            frames = (int)(bufferFrames - padding);
            BYTE* data = nullptr;
            if (frames > 0 && SUCCEEDED(render->GetBuffer(frames, &data))) {
                render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        total += frames;
        if (now - start < 1.0) {
            last = now;
            continue;
        }
        if (measureFrom < 0) {
            measureFrom = now;
            totalAtMeasure = total;
        } else {
            minGap = std::min(minGap, now - last);
            maxGap = std::max(maxGap, now - last);
            minFrames = std::min(minFrames, frames);
            maxFrames = std::max(maxFrames, frames);
            if (frames == 0) emptyEvents++;
            events++;
            // Every 5 s: how far the device is from where a perfect 48 kHz clock would be.
            if (now - measureFrom >= nextLine) {
                printf("  after %5.1f s: %+7.1f frames from nominal\n", now - measureFrom,
                       (double)(total - totalAtMeasure) - (now - measureFrom) * kSampleRate);
                nextLine += 5.0;
            }
        }
        last = now;
        lastTime = now;
    }
    client->Stop();
    const double elapsed = lastTime - measureFrom;
    const double rate = (double)(total - totalAtMeasure) / elapsed;
    printf("%d events in %.2f s: every %.2f ms on average (shortest %.2f, longest %.2f)\n", events, elapsed,
           elapsed / events * 1000.0, minGap * 1000.0, maxGap * 1000.0);
    printf("frames per event: %d to %d, events with none: %d\n", minFrames, maxFrames, emptyEvents);
    printf("rate %.1f frames/s, %+.0f ppm from 48000\n", rate, (rate / kSampleRate - 1.0) * 1e6);
    return 0;
}

// Lists which applications have audio streams open on which device, with the
// loudest level each one reached during `seconds`.
// The volume Windows applies to each device as a whole (the slider in its
// properties), playback and recording.
int volumes() {
    for (EDataFlow flow : {eRender, eCapture}) {
        for (const DeviceInfo& info : listDevices(flow)) {
            ComPtr<IMMDevice> device = findDevice(flow, toUtf8(info.id));
            ComPtr<IAudioEndpointVolume> volume;
            if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.putVoid()))) continue;
            float scalar = 0, db = 0;
            BOOL muted = FALSE;
            volume->GetMasterVolumeLevelScalar(&scalar);
            volume->GetMasterVolumeLevel(&db);
            volume->GetMute(&muted);
            DWORD hardware = 0;
            volume->QueryHardwareSupport(&hardware);
            printf("%-4s %-52s %3.0f%%  %+6.1f dB%s%s\n", flow == eRender ? "out" : "in", info.name.c_str(), scalar * 100.0f,
                   db, muted ? "  MUTED" : "", (hardware & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? "  (hardware)" : "");
        }
    }
    return 0;
}

int sessions(double seconds) {
    struct Row {
        std::string device, process;
        DWORD pid;
        AudioSessionState state;
        float volume;
        BOOL muted;
        ComPtr<IAudioMeterInformation> meter;
        float peak = 0.0f;
    };
    std::vector<Row> rows;

    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                           enumerator.putVoid()),
          "enumerator");
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> devices;
        check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, devices.put()), "enumerate");
        UINT deviceCount = 0;
        devices->GetCount(&deviceCount);
        for (UINT d = 0; d < deviceCount; d++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(d, device.put()))) continue;
            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid()))) continue;
            ComPtr<IAudioSessionEnumerator> list;
            if (FAILED(manager->GetSessionEnumerator(list.put()))) continue;
            int count = 0;
            list->GetCount(&count);
            for (int i = 0; i < count; i++) {
                ComPtr<IAudioSessionControl> control;
                if (FAILED(list->GetSession(i, control.put()))) continue;
                ComPtr<IAudioSessionControl2> control2;
                if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.putVoid()))) continue;
                Row row;
                row.device = std::string(flow == eRender ? "out " : "in  ") + deviceName(device.get());
                control2->GetProcessId(&row.pid);
                control->GetState(&row.state);
                ComPtr<ISimpleAudioVolume> volume;
                row.volume = 1.0f;
                row.muted = FALSE;
                if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), volume.putVoid()))) {
                    volume->GetMasterVolume(&row.volume);
                    volume->GetMute(&row.muted);
                }
                control->QueryInterface(kAudioMeterInformationId, row.meter.putVoid());

                row.process = row.pid == 0 ? "(system sounds)" : "?";
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.pid);
                if (process) {
                    wchar_t path[1024];
                    DWORD size = 1024;
                    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                        std::string full = toUtf8(std::wstring(path, size));
                        row.process = full.substr(full.find_last_of('\\') + 1);
                    }
                    CloseHandle(process);
                }
                rows.push_back(row);
            }
        }
    }

    const double end = nowSeconds() + seconds;
    do {
        for (Row& row : rows) {
            float peak = 0.0f;
            if (row.meter && SUCCEEDED(row.meter->GetPeakValue(&peak))) row.peak = std::max(row.peak, peak);
        }
        Sleep(20);
    } while (nowSeconds() < end);

    for (const Row& row : rows) {
        const char* state = row.state == AudioSessionStateActive ? "playing" : row.state == AudioSessionStateInactive ? "idle" : "closed";
        printf("%-52s %-22s pid %-6lu %-7s volume %3.0f%%%s  peak %6.1f dB\n", row.device.c_str(), row.process.c_str(),
               row.pid, state, row.volume * 100.0f, row.muted ? " MUTED" : "", row.peak > 0 ? 20.0 * std::log10(row.peak) : -120.0);
    }
    return 0;
}

struct Recorder {
    std::string name;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    int channels = 2;
    std::vector<float> stereo;
    StreamClock clock;       // arrival time vs. frames
    double firstStamp = -1;  // device timestamp of the first frame, in seconds
    int discontinuities = 0;

    void open(std::string spec) {
        // "loopback:<output>" records what is being played on an output device.
        const bool loopback = spec.rfind("loopback:", 0) == 0;
        if (loopback) spec = spec.substr(9);
        ComPtr<IMMDevice> device = findDevice(loopback ? eRender : eCapture, spec);
        name = deviceName(device.get());
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()), "activate");
        WAVEFORMATEX* mix = nullptr;
        check(client->GetMixFormat(&mix), "mix format");
        channels = mix->nChannels;
        DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask : 0;
        fprintf(stderr, "%s: device format %lu Hz, %d channels\n", name.c_str(), mix->nSamplesPerSec, channels);
        CoTaskMemFree(mix);
        WAVEFORMATEXTENSIBLE format = floatFormat(channels, mask);
        const DWORD flags = kConvertFlags | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 5000000, 0, &format.Format, nullptr), "initialize");
        check(client->GetService(__uuidof(IAudioCaptureClient), capture.putVoid()), "capture service");
    }

    void poll() {
        for (;;) {
            UINT32 packet = 0;
            if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0, stamp = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devicePosition, &stamp))) break;
            if (firstStamp < 0) firstStamp = (double)stamp * 1e-7;
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && !stereo.empty()) discontinuities++;
            const float* in = (const float*)data;
            const int right = channels > 1 ? 1 : 0;
            for (UINT32 i = 0; i < frames; i++) {
                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                stereo.push_back(silent ? 0.0f : in[i * channels]);
                stereo.push_back(silent ? 0.0f : in[i * channels + right]);
            }
            capture->ReleaseBuffer(frames);
            clock.update(nowSeconds(), (int64_t)(stereo.size() / 2));
        }
    }

    void save(const std::string& path) const {
        FILE* file = _wfopen(toWide(path).c_str(), L"wb");
        if (!file) throw std::runtime_error("cannot write " + path);
        const uint32_t dataBytes = (uint32_t)(stereo.size() * sizeof(float));
        const uint32_t riffBytes = 36 + dataBytes, formatBytes = 16, rate = kSampleRate, byteRate = kSampleRate * 8;
        const uint16_t floatTag = 3, channelCount = 2, blockAlign = 8, bits = 32;
        fwrite("RIFF", 1, 4, file);
        fwrite(&riffBytes, 4, 1, file);
        fwrite("WAVEfmt ", 1, 8, file);
        fwrite(&formatBytes, 4, 1, file);
        fwrite(&floatTag, 2, 1, file);
        fwrite(&channelCount, 2, 1, file);
        fwrite(&rate, 4, 1, file);
        fwrite(&byteRate, 4, 1, file);
        fwrite(&blockAlign, 2, 1, file);
        fwrite(&bits, 2, 1, file);
        fwrite("data", 1, 4, file);
        fwrite(&dataBytes, 4, 1, file);
        fwrite(stereo.data(), sizeof(float), stereo.size(), file);
        fclose(file);
    }
};

int record(std::vector<Recorder>& recorders, double seconds, const std::vector<std::string>& paths) {
    for (Recorder& recorder : recorders) check(recorder.client->Start(), "start");
    const double end = nowSeconds() + seconds;
    while (nowSeconds() < end) {
        for (Recorder& recorder : recorders) recorder.poll();
        Sleep(2);
    }
    for (size_t i = 0; i < recorders.size(); i++) {
        Recorder& recorder = recorders[i];
        recorder.client->Stop();
        recorder.save(paths[i]);
        // "arrival" is when frame 0 reached this program, judged from all packets;
        // "stamp" is when the device says it captured frame 0.
        printf("%s frames=%zu arrival=%.6f stamp=%.6f discontinuities=%d\n", paths[i].c_str(),
               recorder.stereo.size() / 2, recorder.clock.offset(), recorder.firstStamp, recorder.discontinuities);
    }
    // Where "arrival" times start on the performance counter (the clock of
    // "stamp"), to compare them with other programs.
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    printf("arrival zero=%.6f\n", (double)counter.QuadPart / (double)frequency.QuadPart - nowSeconds());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    ComApartment com;
    try {
        std::string command = argc > 1 ? argv[1] : "";
        if (command == "play" && argc >= 6) {
            std::string signal = argv[4];
            if (signal == "sine" && argc >= 7) return play(argv[2], atof(argv[3]), signal, atof(argv[5]), atof(argv[6]));
            if (signal == "clicks" || signal == "noise") return play(argv[2], atof(argv[3]), signal, 0.0, atof(argv[5]));
        }
        if (command == "pace" && argc >= 4) return pace(argv[2], atof(argv[3]));
        if (command == "sessions") return sessions(argc >= 3 ? atof(argv[2]) : 0.0);
        if (command == "volumes") return volumes();
        if (command == "rec" && argc >= 5) {
            std::vector<Recorder> recorders(1);
            recorders[0].open(argv[2]);
            return record(recorders, atof(argv[3]), {argv[4]});
        }
        if (command == "rec2" && argc >= 7) {
            std::vector<Recorder> recorders(2);
            recorders[0].open(argv[2]);
            recorders[1].open(argv[3]);
            return record(recorders, atof(argv[4]), {argv[5], argv[6]});
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
    fprintf(stderr, "usage: see the comment at the top of tools/sigtool.cpp\n");
    return 2;
}
