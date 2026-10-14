#pragma once

#include "com.h"
#include "source_buffer.h"

#include <audioclient.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wolfe {

enum class InputKind {
    Device,    // a recording device: microphone, line in, the far end of a virtual cable
    Loopback,  // everything that is being played on an output device
    Process,   // everything one application (and the processes it starts) is playing
};

struct InputSpec {
    InputKind kind = InputKind::Device;
    // Device/Loopback: "default", a fragment of the device name, or an endpoint id.
    // Process: an executable name ("chrome.exe") or a fragment of its full path
    // ("\\Soundrio\\"), which is how to single out one Java program among many.
    std::string target;
};

// Captures audio from outside the graph into a SourceBuffer, on its own thread.
// If the device or the process is not there, or goes away, it keeps retrying.
class Input {
public:
    explicit Input(InputSpec spec);
    ~Input();

    void start();
    void stop();

    const std::shared_ptr<SourceBuffer>& buffer() const { return buffer_; }
    std::string status() const;

private:
    void threadMain();
    void watcherMain();
    void open();
    void run();
    void close();
    void drain();
    void appendPacket(const BYTE* data, UINT32 frames, bool silent);
    void setStatus(const std::string& text);

    InputSpec spec_;
    std::shared_ptr<SourceBuffer> buffer_;

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HANDLE dataEvent_ = nullptr;
    HANDLE lookNowEvent_ = nullptr;  // Process inputs: tells the watcher not to wait for its next turn

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    HANDLE process_ = nullptr;   // Process inputs: to notice when the application exits
    std::thread watcher_;                // Process inputs: picks the process to capture
    std::atomic<DWORD> wantedPid_{0};    // its current choice (0: the application is not running)
    DWORD capturedPid_ = 0;              // the process being captured
    bool retryNow_ = false;              // reopen at once, without the usual pause
    int channels_ = 2;           // channel count of the packets WASAPI hands over
    std::vector<float> block_;   // packets of one wake-up, already converted to stereo
    double recentBlock_ = 0;     // typical size of the blocks delivered lately, in frames
    int publishedBlock_ = 0;     // the same, as last told to the readers

    mutable std::mutex statusMutex_;
    std::string status_ = "stopped";
};

}  // namespace wolfe
