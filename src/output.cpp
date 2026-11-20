#include "output.h"

#include "devices.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace wolfe {

namespace {

// Audio kept queued in the device beyond the one period it plays before asking
// again. It covers this thread waking up a little late.
constexpr int kSafetyFrames = kSampleRate * 5 / 1000;
// Some devices ask irregularly right after starting; they are fed silence for
// this long before the graph is connected.
constexpr double kSettleSeconds = 0.5;
// A queue that grew after an underrun shrinks again after this long without
// ever coming close to empty.
constexpr double kShrinkSeconds = 60.0;

constexpr double kQuantumSeconds = (double)kQuantum / kSampleRate;
constexpr size_t kQuantumSamples = (size_t)kQuantum * kChannels;

}  // namespace

Output::Output(std::string device, std::shared_ptr<SourceBuffer> feed, double marginSeconds)
    : device_(std::move(device)), reader_(std::make_shared<SourceReader>(std::move(feed), marginSeconds)) {}

Output::~Output() { stop(); }

void Output::start() {
    if (thread_.joinable()) return;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    dataEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { threadMain(); });
}

void Output::stop() {
    if (!thread_.joinable()) return;
    SetEvent(stopEvent_);
    thread_.join();
    CloseHandle(stopEvent_);
    CloseHandle(dataEvent_);
    stopEvent_ = dataEvent_ = nullptr;
}

std::string Output::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void Output::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (text == status_) return;
    status_ = text;
    logLine("output \"%s\": %s", device_.c_str(), text.c_str());
}

void Output::threadMain() {
    ComApartment com;

    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        try {
            open();
            run();
        } catch (const std::exception& error) {
            setStatus(error.what());
        }
        close();
        if (WaitForSingleObject(stopEvent_, 1000) == WAIT_OBJECT_0) break;
    }

    setStatus("stopped");
}

void Output::open() {
    ComPtr<IMMDevice> device = findDevice(eRender, device_);
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid()), "activate device");

    // Stereo float at 48 kHz whatever the device runs at: Windows converts.
    WAVEFORMATEXTENSIBLE format = floatFormat(kChannels);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    // 100 ms of room; how much of it is actually kept filled is targetFill_.
    check(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format.Format, nullptr),
          "initialize playback");
    UINT32 bufferFrames = 0;
    check(client_->GetBufferSize(&bufferFrames), "get buffer size");
    check(client_->SetEventHandle(dataEvent_), "set playback event");
    check(client_->GetService(__uuidof(IAudioRenderClient), render_.putVoid()), "get playback service");

    bufferFrames_ = (int)bufferFrames;
    targetFill_ = std::min(devicePeriodFrames(client_.get()) + kSafetyFrames, bufferFrames_);
    statTargetFill_.store(targetFill_, std::memory_order_relaxed);
    clock_.reset();
    submitted_ = 0;
    madeUp_ = 0;
    setStatus("playing on " + deviceName(device.get()));
}

void Output::run() {
    auto writeSilence = [this](int frames) {
        BYTE* data = nullptr;
        check(render_->GetBuffer(frames, &data), "get playback buffer");
        check(render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT), "release playback buffer");
        submitted_ += frames;
    };

    AudioThreadPriority priority;
    writeSilence(targetFill_);
    check(client_->Start(), "start playback");

    const int baseFill = targetFill_;
    const int largestFill = bufferFrames_ - kQuantum;
    const double startTime = nowSeconds();
    bool settled = false;
    double lastWake = startTime;
    double shrinkCheck = startTime;  // start of the current "never close to empty" observation
    int lowestSinceCheck = 1 << 30;
    float cycle[kQuantumSamples];
    HANDLE handles[2] = {stopEvent_, dataEvent_};

    for (;;) {
        // The device signals each time it has taken audio from the queue.
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (result == WAIT_OBJECT_0) return;
        const double now = nowSeconds();

        UINT32 queued = 0;
        check(client_->GetCurrentPadding(&queued), "get playback position");
        int padding = (int)queued;

        if (!settled) {
            if (now - startTime < kSettleSeconds) {
                if (padding < targetFill_) writeSilence(targetFill_ - padding);
                continue;
            }
            settled = true;
            clock_.reset();
            lastWake = shrinkCheck = now;
        }

        if (now - lastWake > statLongestWait_.load(std::memory_order_relaxed)) {
            statLongestWait_.store(now - lastWake, std::memory_order_relaxed);
        }
        lastWake = now;
        if (padding < statLowest_.load(std::memory_order_relaxed)) statLowest_.store(padding, std::memory_order_relaxed);
        lowestSinceCheck = std::min(lowestSinceCheck, padding);

        if (padding == 0) {
            // The device ran dry: either this thread was late or the device took
            // more at once than was queued.
            underruns_.fetch_add(1, std::memory_order_relaxed);
            if (clock_.valid()) {
                // Keep counting device time through the silence it played on its own.
                double missing = clock_.frameAt(now) - (double)(submitted_ + madeUp_);
                if (missing > kSampleRate / 1000) madeUp_ += std::llround(missing);
            }
            if (targetFill_ + kQuantum <= largestFill) {
                // Queue one quantum more from now on. The extra quantum is silence:
                // the graph only produces as much audio as time has passed.
                targetFill_ += kQuantum;
                writeSilence(kQuantum);
                padding += kQuantum;
                statTargetFill_.store(targetFill_, std::memory_order_relaxed);
                logLine("output \"%s\": ran out of audio, now keeping %d ms queued", device_.c_str(),
                        targetFill_ * 1000 / kSampleRate);
            }
            shrinkCheck = now;
            lowestSinceCheck = 1 << 30;
        } else if (now - shrinkCheck > kShrinkSeconds) {
            if (targetFill_ > baseFill && lowestSinceCheck >= kQuantum + kSafetyFrames) {
                // Not refilling once lets the queue drop by one quantum.
                targetFill_ -= kQuantum;
                statTargetFill_.store(targetFill_, std::memory_order_relaxed);
                logLine("output \"%s\": steady, now keeping %d ms queued", device_.c_str(),
                        targetFill_ * 1000 / kSampleRate);
            }
            shrinkCheck = now;
            lowestSinceCheck = 1 << 30;
        }

        // Frames the device has played so far, on its own clock.
        const int64_t played = submitted_ - padding + madeUp_;
        clock_.update(now, played);

        int quanta = 0;
        while (padding + quanta * kQuantum < targetFill_ && padding + (quanta + 1) * kQuantum <= bufferFrames_) quanta++;
        if (quanta == 0) continue;

        // One cycle per quantum; when several are due, the earlier ones stand
        // for moments that have already passed.
        const double smoothNow = clock_.timeOf((double)played);
        for (int i = 0; i < quanta; i++) {
            runCycle(smoothNow - (quanta - 1 - i) * kQuantumSeconds, cycle);
            BYTE* data = nullptr;
            check(render_->GetBuffer(kQuantum, &data), "get playback buffer");
            std::memcpy(data, cycle, sizeof(cycle));
            check(render_->ReleaseBuffer(kQuantum, 0), "release playback buffer");
            submitted_ += kQuantum;
        }
    }
}

void Output::close() {
    if (client_) client_->Stop();
    render_.reset();
    client_.reset();
}

Output::Stats Output::takeStats() {
    const double msPerFrame = 1000.0 / kSampleRate;
    Stats stats;
    stats.underruns = underruns_.load(std::memory_order_relaxed);
    stats.queuedMs = statTargetFill_.load(std::memory_order_relaxed) * msPerFrame;
    const int lowest = statLowest_.exchange(1 << 30, std::memory_order_relaxed);
    stats.lowestMs = lowest == (1 << 30) ? 0.0 : lowest * msPerFrame;
    stats.longestWaitMs = statLongestWait_.exchange(0.0, std::memory_order_relaxed) * 1000.0;
    return stats;
}

void Output::runCycle(double cycleTime, float* stereo) {
    reader_->read(cycleTime, nowSeconds(), stereo);

    float peak = 0.0f;
    for (size_t i = 0; i < kQuantumSamples; i++) peak = std::max(peak, std::fabs(stereo[i]));
    if (peak > peak_.load(std::memory_order_relaxed)) peak_.store(peak, std::memory_order_relaxed);
}

}  // namespace wolfe
