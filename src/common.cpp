#include "common.h"

#include "com.h"

#include <avrt.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace wolfe {

double nowSeconds() {
    static const double secondsPerTick = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1.0 / (double)frequency.QuadPart;
    }();
    static const LONGLONG start = [] {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)(counter.QuadPart - start) * secondsPerTick;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), size);
    return wide;
}

std::string toLower(std::string text) {
    // Through UTF-16 so that non-ASCII device names compare correctly.
    std::wstring wide = toWide(text);
    if (!wide.empty()) CharLowerBuffW(wide.data(), (DWORD)wide.size());
    return toUtf8(wide);
}

namespace {

// Writing to the console or to a file can block for a long time, and audio
// threads log too. Lines are queued here and written by a thread of their own.
class Logger {
public:
    Logger() : thread_([this] { run(); }) {}
    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }

    void add(std::string line) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(std::move(line));
        }
        wake_.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || !lines_.empty(); });
            if (lines_.empty()) return;
            std::deque<std::string> batch;
            batch.swap(lines_);
            lock.unlock();
            for (const std::string& line : batch) fputs(line.c_str(), stderr);
            fflush(stderr);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> lines_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace

void logLine(const char* format, ...) {
    static Logger logger;

    char stamp[32];
    snprintf(stamp, sizeof(stamp), "[%9.3f] ", nowSeconds());
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::string text(length > 0 ? (size_t)length : 0, '\0');
    if (length > 0) vsnprintf(text.data(), (size_t)length + 1, format, args);
    va_end(args);

    logger.add(stamp + text + "\n");
}

AudioThreadPriority::AudioThreadPriority() {
    DWORD taskIndex = 0;
    task_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

AudioThreadPriority::~AudioThreadPriority() {
    if (task_) AvRevertMmThreadCharacteristics(task_);
}

ComError::ComError(HRESULT hr, const char* what)
    : std::runtime_error([&] {
          char text[160];
          snprintf(text, sizeof(text), "%s failed (0x%08lx)", what, (unsigned long)hr);
          return std::string(text);
      }()),
      hr_(hr) {}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw ComError(hr, what);
}

ComApartment::ComApartment() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
ComApartment::~ComApartment() { CoUninitialize(); }

}  // namespace wolfe
