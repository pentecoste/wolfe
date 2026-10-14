#include "input.h"

#include "devices.h"
#include "process_finder.h"

#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace wolfe {

namespace {

// ---------------------------------------------------------------------------
// Per-application capture ("process loopback")
// ---------------------------------------------------------------------------

// These come from audioclientactivationparams.h, which MinGW does not ship.
// "Process tree" is Windows' name; what is included is the process and the
// processes it started directly, not the ones those started (measured).
enum ProcessLoopbackMode { kIncludeProcessTree = 0, kExcludeProcessTree = 1 };
enum ActivationType { kActivationDefault = 0, kActivationProcessLoopback = 1 };
struct ProcessLoopbackParams {
    DWORD targetProcessId;
    ProcessLoopbackMode mode;
};
struct ActivationParams {
    ActivationType type;
    ProcessLoopbackParams loopback;
};
const wchar_t* const kProcessLoopbackDevice = L"VAD\\Process_Loopback";

const GUID kAgileObjectId = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// Windows activates this kind of stream asynchronously and reports back here.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    HANDLE doneEvent() const { return done_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        // IAgileObject has no methods of its own: answering to it only tells
        // COM that this object may be called from any thread.
        if (id == __uuidof(IUnknown) || id == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            id == kAgileObjectId) {
            *object = this;
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    ~ActivationHandler() { CloseHandle(done_); }  // destroyed by the last Release()

    std::atomic<ULONG> references_{1};
    HANDLE done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
};

ComPtr<IAudioClient> activateProcessLoopback(DWORD pid) {
    ActivationParams params = {};
    params.type = kActivationProcessLoopback;
    params.loopback.targetProcessId = pid;
    params.loopback.mode = kIncludeProcessTree;

    PROPVARIANT variant;
    PropVariantInit(&variant);
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(params);
    variant.blob.pBlobData = (BYTE*)&params;

    ActivationHandler* handler = new ActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient), &variant, handler,
                                             operation.put());
    if (SUCCEEDED(hr) && WaitForSingleObject(handler->doneEvent(), 5000) != WAIT_OBJECT_0) {
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    handler->Release();
    check(hr, "activate application capture");

    HRESULT activation = E_FAIL;
    ComPtr<IUnknown> unknown;
    check(operation->GetActivateResult(&activation, unknown.put()), "get activation result");
    check(activation, "application capture activation");

    ComPtr<IAudioClient> client;
    check(unknown->QueryInterface(__uuidof(IAudioClient), client.putVoid()), "get audio client");
    return client;
}

}  // namespace

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Input::Input(InputSpec spec) : spec_(std::move(spec)), buffer_(std::make_shared<SourceBuffer>()) {
    block_.reserve((size_t)kSampleRate * kChannels);
}

Input::~Input() { stop(); }

void Input::start() {
    if (thread_.joinable()) return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    lookNowEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (spec_.kind == InputKind::Process) {
        wantedPid_ = chooseProcess(spec_.target, 0).pid;
        watcher_ = std::thread([this] { watcherMain(); });
    }
    thread_ = std::thread([this] { threadMain(); });
}

void Input::stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
    if (watcher_.joinable()) watcher_.join();
    CloseHandle(stopEvent_);
    CloseHandle(dataEvent_);
    CloseHandle(lookNowEvent_);
    stopEvent_ = dataEvent_ = lookNowEvent_ = nullptr;
}

std::string Input::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void Input::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (text == status_) return;
    status_ = text;
    logLine("input \"%s\": %s", spec_.target.c_str(), text.c_str());
}

// Keeps deciding which process of the application should be captured: the
// application may not be running yet, may be restarted, or its audio may move
// to another of its processes. Looking at processes is slow, ordinary work,
// kept away from the thread that moves audio.
void Input::watcherMain() {
    ComApartment com;
    HANDLE handles[2] = {stopEvent_, lookNowEvent_};
    double lastChange = nowSeconds();
    for (;;) {
        // Every second while waiting for the application or while it is still
        // starting up; rarely once things have settled, when all that can
        // change is which of its processes plays (its exit is noticed by the
        // capture thread, which asks for a look at once).
        const bool settled = wantedPid_ != 0 && nowSeconds() - lastChange > 15.0;
        if (WaitForMultipleObjects(2, handles, FALSE, settled ? 3000 : 1000) == WAIT_OBJECT_0) return;
        const DWORD chosen = chooseProcess(spec_.target, wantedPid_).pid;
        if (chosen != wantedPid_) lastChange = nowSeconds();
        wantedPid_ = chosen;
    }
}

void Input::threadMain() {
    ComApartment com;

    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        retryNow_ = false;
        try {
            open();
            run();
        } catch (const std::exception& error) {
            setStatus(error.what());
        }
        close();
        // Device unplugged, application not running yet...: try again shortly.
        if (WaitForSingleObject(stopEvent_, retryNow_ ? 0 : 1000) == WAIT_OBJECT_0) break;
    }

    setStatus("stopped");
}

void Input::open() {
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    WAVEFORMATEXTENSIBLE format;
    std::string description;
    int packetFrames = kQuantum;

    if (spec_.kind == InputKind::Process) {
        capturedPid_ = wantedPid_;
        if (!capturedPid_) throw std::runtime_error("waiting for the application to start");
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, capturedPid_);
        client_ = activateProcessLoopback(capturedPid_);
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        channels_ = 2;
        format = floatFormat(channels_);

        wchar_t path[1024];
        DWORD size = 1024;
        std::string name = "process";
        if (process_ && QueryFullProcessImageNameW(process_, 0, path, &size)) {
            name = toUtf8(std::wstring(path, size));
            name = name.substr(name.find_last_of('\\') + 1);
        }
        description = name + " (pid " + std::to_string(capturedPid_) + ")";
    } else {
        const bool loopback = spec_.kind == InputKind::Loopback;
        ComPtr<IMMDevice> device = findDevice(loopback ? eRender : eCapture, spec_.target);
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid()), "activate device");

        // Keep the device's own channel layout and let Windows convert only the
        // sample rate; channels are folded to stereo in appendPacket().
        WAVEFORMATEX* mix = nullptr;
        check(client_->GetMixFormat(&mix), "get device format");
        channels_ = mix->nChannels;
        DWORD mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask : 0;
        CoTaskMemFree(mix);
        format = floatFormat(channels_, mask);

        flags |= AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        packetFrames = devicePeriodFrames(client_.get());
        description = deviceName(device.get());
    }

    // 200 ms of room in the capture buffer; it does not add any delay.
    check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &format.Format, nullptr),
          "initialize capture");
    check(client_->SetEventHandle(dataEvent_), "set capture event");
    check(client_->GetService(__uuidof(IAudioCaptureClient), capture_.putVoid()), "get capture service");

    recentBlock_ = packetFrames;
    publishedBlock_ = packetFrames;
    buffer_->setPacketFrames(packetFrames);
    setStatus("capturing " + description);
}

void Input::run() {
    // Only here: looking for the device or the application is ordinary work.
    AudioThreadPriority priority;
    check(client_->Start(), "start capture");
    HANDLE handles[2] = {stopEvent_, dataEvent_};
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, 200);
        if (result == WAIT_OBJECT_0) return;
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            wantedPid_ = 0;
            SetEvent(lookNowEvent_);
            throw std::runtime_error("the application exited");
        }
        if (spec_.kind == InputKind::Process && wantedPid_ != capturedPid_) {
            retryNow_ = true;
            throw std::runtime_error("its audio is in another process, switching");
        }
        drain();
    }
}

void Input::close() {
    if (client_) client_->Stop();
    capture_.reset();
    client_.reset();
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

// Everything that is ready goes to the buffer as one block with one arrival
// time. If this thread was held up and several packets piled up, the buffer
// then sees one large block that is on time, not a pause followed by a burst.
void Input::drain() {
    block_.clear();
    for (;;) {
        UINT32 packetFrames = 0;
        check(capture_->GetNextPacketSize(&packetFrames), "get packet size");
        if (packetFrames == 0) break;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        check(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "get capture buffer");
        appendPacket(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
        check(capture_->ReleaseBuffer(frames), "release capture buffer");
    }
    if (block_.empty()) return;
    const int frames = (int)(block_.size() / kChannels);
    buffer_->push(nowSeconds(), block_.data(), frames);

    // Readers must stay about one block behind, so tell them how large the
    // blocks usually are: some sources deliver several periods at once. This
    // follows the typical size, in 5 ms steps; one odd block (this thread
    // woke up late and found two packets) does not move it.
    recentBlock_ += 0.05 * (std::min(frames, kSampleRate / 10) - recentBlock_);
    const int step = kSampleRate * 5 / 1000;
    if (std::fabs(recentBlock_ - publishedBlock_) > 0.75 * step) {
        publishedBlock_ = std::max(step, (int)std::lround(recentBlock_ / step) * step);
        buffer_->setPacketFrames(publishedBlock_);
    }
}

void Input::appendPacket(const BYTE* data, UINT32 frames, bool silent) {
    size_t start = block_.size();
    block_.resize(start + (size_t)frames * kChannels, 0.0f);
    if (silent || !data) return;

    float* out = block_.data() + start;
    const float* in = (const float*)data;
    // Mono is copied to both sides; of anything wider only front left/right are kept.
    const int rightChannel = channels_ > 1 ? 1 : 0;
    for (UINT32 i = 0; i < frames; i++) {
        out[i * 2] = in[i * channels_];
        out[i * 2 + 1] = in[i * channels_ + rightChannel];
    }
}

}  // namespace wolfe
