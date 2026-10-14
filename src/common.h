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

}  // namespace wolfe
