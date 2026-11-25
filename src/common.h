#pragma once

#include <cstdint>
#include <string>

namespace wolfe {

// Every stream inside the graph uses this one format: 48 kHz, stereo, 32-bit float.
// Devices that run at another format are converted by Windows at the edge.
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

// The graph is evaluated in fixed blocks of this many frames (10 ms).
// It is also the frame size RNNoise expects.
constexpr int kQuantum = 480;

// Monotonic time in seconds since the program started.
double nowSeconds();

std::string toUtf8(const std::wstring& wide);
std::wstring toWide(const std::string& utf8);
std::string toLower(std::string text);

// printf-style, thread-safe, timestamped, written to stderr.
void logLine(const char* format, ...);

// For a device that a thread keeps trying to open, once a second. Its status
// is logged when it changes, so a device failing the same way every time
// shows one line and then nothing, as if no one were trying any more. This
// says it is still failing, ever more rarely (after 10 s, 1 min, 6 min, then
// every hour), and when it works again after having been said to fail.
// Used by one thread only.
class RetryLog {
public:
    explicit RetryLog(std::string what) : what_(std::move(what)) {}
    void failed(const std::string& error);
    void succeeded();

private:
    std::string what_;     // how the log names the device: output "..."
    double since_ = 0;     // the first of the failures in a row; 0: working
    double nextReport_ = 0;
    long attempts_ = 0;
    bool reported_ = false;  // a "still failing" line went out for these failures
};

}  // namespace wolfe
