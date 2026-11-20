// A sound card made of software: a USB audio device served over USB/IP, which
// usbip-win2 attaches to Windows. Windows then drives it with its own USB
// audio driver and shows a speaker and a microphone. This is the prototype
// that decides whether Wolfe can make its own devices this way; it is not
// needed to use Wolfe.
//
//   wolfe-uacproto [uac1|uac2] [port] [delay_ms]     defaults: uac2, 3241, 1
//
// uac1: USB Audio Class 1, full speed, one packet per millisecond; Windows
//       drives it with usbaudio.sys.
// uac2: USB Audio Class 2, high speed, one packet per 125 microseconds, with a
//       feedback endpoint that tells Windows how fast the speaker consumes;
//       Windows drives it with usbaudio2.sys.
//
// What is played on the speaker comes out of the microphone delay_ms later,
// like a cable, so that it can be measured with wolfe-sigtool. Attach it while
// this runs (no administrator needed):
//
//   usbip --tcp-port 3241 attach -r 127.0.0.1 -b 1-1
//
// Timing: every endpoint has a timeline on the performance counter. A request
// of n packets is completed when its n packet periods have passed on that
// timeline, as a real device would consume or produce them. Samples are kept
// by the frame of the server's clock they belong to, so the microphone reads
// exactly what the speaker was given delay_ms earlier.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// --- output ------------------------------------------------------------------

// Writing to a console or a file can take milliseconds, which a thread that
// keeps a device's time cannot spend: lines are queued here and written by a
// thread of their own.
class Output {
public:
    Output() : thread_([this] { run(); }) {}

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
            wake_.wait(lock, [this] { return !lines_.empty(); });
            std::deque<std::string> batch;
            batch.swap(lines_);
            lock.unlock();
            for (const std::string& line : batch) fputs(line.c_str(), stdout);
            fflush(stdout);
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> lines_;
    std::thread thread_;  // runs until the process ends
};

void say(const char* format, ...) {
    static Output* output = new Output;  // never destroyed: the process ends with its thread running
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    output->add(text);
}

// --- the device --------------------------------------------------------------

constexpr int kRate = 48000;
constexpr int kFrameBytes = 4;        // 16-bit stereo
constexpr uint16_t kVendor = 0x1209;  // pid.codes, for open source hardware

const char* const kBusId = "1-1";
constexpr uint32_t kBusNum = 1, kDevNum = 2;

enum class Role { None, Speaker, Feedback, Microphone };

// Everything that differs between the two kinds of device.
struct DeviceKind {
    bool uac2;
    // Never change product, release or serial of a device people use: Windows
    // gives its endpoints new ids (losing what apps were assigned to them)
    // when these change. The descriptors themselves can change without that.
    uint16_t product, release;
    const char* name;
    const char* serial;
    uint32_t speed;         // USB/IP: 2 full, 3 high
    double dataPeriod;      // seconds per packet on the audio endpoints
    double feedbackPeriod;  // seconds per packet on the feedback endpoint
    int framesPerPacket() const { return (int)std::lround(kRate * dataPeriod); }
    int maxPacket() const { return (framesPerPacket() + 1) * kFrameBytes; }
};

const DeviceKind kUac1 = {false, 0x0001, 0x0001, "Wolfe Proto", "WOLFE0001", 2, 0.001, 0.0};
const DeviceKind kUac2 = {true, 0x0002, 0x0001, "Wolfe Proto 2", "WOLFE0002", 3, 0.000125, 0.001};
const DeviceKind* g_kind = &kUac2;

void add(std::vector<uint8_t>& out, std::initializer_list<int> bytes) {
    for (int b : bytes) out.push_back((uint8_t)b);
}
void add16(std::vector<uint8_t>& out, int value) { add(out, {value & 0xFF, (value >> 8) & 0xFF}); }
void setLength16(std::vector<uint8_t>& out, size_t at, size_t value) {
    out[at] = (uint8_t)(value & 0xFF);
    out[at + 1] = (uint8_t)(value >> 8);
}

std::vector<uint8_t> deviceDescriptor() {
    std::vector<uint8_t> d;
    if (g_kind->uac2) add(d, {18, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 64});  // USB 2.0, interface association
    else add(d, {18, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 64});               // USB 1.1, class per interface
    add16(d, kVendor);
    add16(d, g_kind->product);
    add16(d, g_kind->release);
    add(d, {1, 2, 3, 1});
    return d;
}

std::vector<uint8_t> qualifierDescriptor() { return {10, 0x06, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0}; }

// Entities of the audio function. Terminal 1 (USB streaming) feeds terminal 3
// (speaker); terminal 4 (microphone) feeds terminal 6 (USB streaming). In UAC2
// everything runs on clock source 0x10.
constexpr int kClockId = 0x10;

std::vector<uint8_t> uac1Config() {
    const int maxPacket = g_kind->maxPacket();
    std::vector<uint8_t> c = {9, 0x02, 0, 0, 3, 1, 0, 0x80, 50};
    add(c, {9, 0x04, 0, 0, 0, 0x01, 0x01, 0, 0});
    const size_t header = c.size();
    add(c, {10, 0x24, 0x01, 0x00, 0x01, 0, 0, 2, 1, 2});
    add(c, {12, 0x24, 0x02, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0});
    add(c, {9, 0x24, 0x03, 3, 0x01, 0x03, 0, 1, 0});
    add(c, {12, 0x24, 0x02, 4, 0x01, 0x02, 0, 2, 0x03, 0x00, 0, 0});
    add(c, {9, 0x24, 0x03, 6, 0x01, 0x01, 0, 4, 0});
    setLength16(c, header + 5, c.size() - header);
    // The speaker's endpoint adapts to the host's pace; the microphone's runs on its own clock.
    const int streams[2][4] = {{1, 1, 0x01, 0x09}, {2, 6, 0x82, 0x05}};
    for (const auto& s : streams) {
        add(c, {9, 0x04, s[0], 0, 0, 0x01, 0x02, 0, 0});
        add(c, {9, 0x04, s[0], 1, 1, 0x01, 0x02, 0, 0});
        add(c, {7, 0x24, 0x01, s[1], 1, 0x01, 0x00});
        add(c, {11, 0x24, 0x02, 0x01, 2, 2, 16, 1, 0x80, 0xBB, 0x00});
        add(c, {9, 0x05, s[2], s[3], maxPacket & 0xFF, maxPacket >> 8, 1, 0, 0});
        add(c, {7, 0x25, 0x01, 0x00, 0, 0, 0});
    }
    setLength16(c, 2, c.size());
    return c;
}

std::vector<uint8_t> uac2Config() {
    const int maxPacket = g_kind->maxPacket();
    std::vector<uint8_t> c = {9, 0x02, 0, 0, 3, 1, 0, 0x80, 50};
    add(c, {8, 0x0B, 0, 3, 0x01, 0x00, 0x20, 0});         // interface association: 0..2 are one audio function
    add(c, {9, 0x04, 0, 0, 0, 0x01, 0x01, 0x20, 0});      // audio control, UAC2
    const size_t header = c.size();
    add(c, {9, 0x24, 0x01, 0x00, 0x02, 0x08, 0, 0, 0});   // header: ADC 2.0, category I/O box
    add(c, {8, 0x24, 0x0A, kClockId, 0x01, 0x05, 0, 0});  // clock source: internal, fixed; frequency and validity readable
    add(c, {17, 0x24, 0x02, 1, 0x01, 0x01, 0, kClockId, 2, 0x03, 0, 0, 0, 0, 0, 0, 0});  // IT 1: USB streaming
    add(c, {12, 0x24, 0x03, 3, 0x01, 0x03, 0, 1, kClockId, 0, 0, 0});                    // OT 3: speaker
    add(c, {17, 0x24, 0x02, 4, 0x01, 0x02, 0, kClockId, 2, 0x03, 0, 0, 0, 0, 0, 0, 0});  // IT 4: microphone
    add(c, {12, 0x24, 0x03, 6, 0x01, 0x01, 0, 4, kClockId, 0, 0, 0});                    // OT 6: USB streaming
    setLength16(c, header + 6, c.size() - header);

    // Speaker: asynchronous, with an explicit feedback endpoint (1 IN) that
    // tells the host how many frames to send per packet.
    add(c, {9, 0x04, 1, 0, 0, 0x01, 0x02, 0x20, 0});
    add(c, {9, 0x04, 1, 1, 2, 0x01, 0x02, 0x20, 0});
    add(c, {16, 0x24, 0x01, 1, 0, 0x01, 0x01, 0, 0, 0, 2, 0x03, 0, 0, 0, 0});  // AS general: PCM, 2 ch
    add(c, {6, 0x24, 0x02, 0x01, 2, 16});                                       // type I: 16 bits in 2 bytes
    add(c, {7, 0x05, 0x01, 0x05, maxPacket & 0xFF, maxPacket >> 8, 1});         // data OUT, async, every microframe
    add(c, {8, 0x25, 0x01, 0, 0, 0, 0, 0});
    add(c, {7, 0x05, 0x81, 0x11, 4, 0, 4});                                     // feedback IN, every 8 microframes
    // Microphone: asynchronous; the host follows the amount it receives.
    add(c, {9, 0x04, 2, 0, 0, 0x01, 0x02, 0x20, 0});
    add(c, {9, 0x04, 2, 1, 1, 0x01, 0x02, 0x20, 0});
    add(c, {16, 0x24, 0x01, 6, 0, 0x01, 0x01, 0, 0, 0, 2, 0x03, 0, 0, 0, 0});
    add(c, {6, 0x24, 0x02, 0x01, 2, 16});
    add(c, {7, 0x05, 0x82, 0x05, maxPacket & 0xFF, maxPacket >> 8, 1});
    add(c, {8, 0x25, 0x01, 0, 0, 0, 0, 0});
    setLength16(c, 2, c.size());
    return c;
}

std::vector<uint8_t> stringDescriptor(int index) {
    if (index == 0) return {4, 0x03, 0x09, 0x04};  // US English
    const char* text = index == 1 ? "Wolfe" : index == 2 ? g_kind->name : index == 3 ? g_kind->serial : nullptr;
    if (!text) return {};
    std::vector<uint8_t> result = {0, 0x03};
    for (const char* c = text; *c; c++) add(result, {*c, 0});
    result[0] = (uint8_t)result.size();
    return result;
}

Role roleOf(uint32_t endpoint, bool in) {
    if (endpoint == 1 && !in) return Role::Speaker;
    if (endpoint == 1 && in && g_kind->uac2) return Role::Feedback;
    if (endpoint == 2 && in) return Role::Microphone;
    return Role::None;
}

const char* nameOf(Role role) {
    return role == Role::Speaker    ? "speaker"
           : role == Role::Feedback ? "feedback"
           : role == Role::Microphone ? "microphone"
                                      : "?";
}

// --- time ----------------------------------------------------------------------

double nowSeconds() {
    static const double period = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1.0 / (double)frequency.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * period;
}

// --- bytes on the wire (USB/IP is big-endian) ------------------------------------

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((uint8_t)(value >> shift));
}
void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}
void putText(std::vector<uint8_t>& out, const char* text, size_t size) {
    const size_t length = std::min(strlen(text), size - 1);
    out.insert(out.end(), text, text + length);
    out.insert(out.end(), size - length, 0);
}
uint32_t get32(const uint8_t* in) { return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3]; }
uint16_t get16(const uint8_t* in) { return (uint16_t)(in[0] << 8 | in[1]); }

bool receiveAll(SOCKET socket, void* buffer, size_t size) {
    char* at = (char*)buffer;
    while (size > 0) {
        const int got = recv(socket, at, (int)std::min<size_t>(size, 1 << 20), 0);
        if (got <= 0) return false;
        at += got;
        size -= (size_t)got;
    }
    return true;
}

bool sendAll(SOCKET socket, const std::vector<uint8_t>& data) {
    const char* at = (const char*)data.data();
    size_t size = data.size();
    while (size > 0) {
        const int sent = send(socket, at, (int)size, 0);
        if (sent <= 0) return false;
        at += sent;
        size -= (size_t)sent;
    }
    return true;
}

// The device as OP_REP_DEVLIST and OP_REP_IMPORT describe it.
void putDevice(std::vector<uint8_t>& out) {
    putText(out, "/sys/devices/platform/wolfe/1-1", 256);
    putText(out, kBusId, 32);
    put32(out, kBusNum);
    put32(out, kDevNum);
    put32(out, g_kind->speed);
    put16(out, kVendor);
    put16(out, g_kind->product);
    put16(out, g_kind->release);
    if (g_kind->uac2) out.insert(out.end(), {0xEF, 0x02, 0x01, 1, 1, 3});
    else out.insert(out.end(), {0, 0, 0, 1, 1, 3});
}

// --- the cable -----------------------------------------------------------------

// What the speaker was given, by the frame of the server's clock it belongs
// to. A frame not written for that position reads as silence.
class Cable {
public:
    Cable() : samples_(kFrames * 2, 0), held_(kFrames, -1) {}

    void write(int64_t frame, const uint8_t* data, int frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < frames; i++, frame++) {
            const size_t at = (size_t)(frame & (kFrames - 1));
            memcpy(&samples_[at * 2], data + i * kFrameBytes, kFrameBytes);
            held_[at] = frame;
        }
    }

    // Returns how many of the frames were missing.
    int read(int64_t frame, uint8_t* out, int frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        int missing = 0;
        for (int i = 0; i < frames; i++, frame++) {
            const size_t at = (size_t)(frame & (kFrames - 1));
            if (held_[at] == frame) {
                memcpy(out + i * kFrameBytes, &samples_[at * 2], kFrameBytes);
            } else {
                memset(out + i * kFrameBytes, 0, kFrameBytes);
                missing++;
            }
        }
        return missing;
    }

private:
    static constexpr int64_t kFrames = 1 << 16;  // well over a second
    std::mutex mutex_;
    std::vector<int16_t> samples_;
    std::vector<int64_t> held_;
};

// --- what is measured ---------------------------------------------------------------

struct EndpointStats {
    std::atomic<long> urbs{0}, packets{0}, restarts{0}, oddPackets{0}, missing{0};
    std::atomic<long> leadMinUs{1 << 30}, leadMaxUs{0};  // how far ahead of its time a request arrived
    std::atomic<long> lateMaxUs{0};                      // how late a request was completed

    static void lower(std::atomic<long>& value, long candidate) {
        long seen = value.load();
        while (candidate < seen && !value.compare_exchange_weak(seen, candidate)) {}
    }
    static void raise(std::atomic<long>& value, long candidate) {
        long seen = value.load();
        while (candidate > seen && !value.compare_exchange_weak(seen, candidate)) {}
    }
    std::string take(const char* name) {
        const long count = urbs.exchange(0);
        const long leadMin = leadMinUs.exchange(1 << 30), leadMax = leadMaxUs.exchange(0);
        if (count == 0) return "";
        char line[256];
        snprintf(line, sizeof(line),
                 "  %-10s urbs %4ld  packets %5ld  lead %6.1f..%6.1f ms  late max %5.2f ms  restarts %ld  odd %ld  missing %ld\n",
                 name, count, packets.exchange(0), leadMin / 1000.0, leadMax / 1000.0, lateMaxUs.exchange(0) / 1000.0,
                 restarts.exchange(0), oddPackets.exchange(0), missing.exchange(0));
        return line;
    }
};

// Notices the test clicks of wolfe-sigtool, to time them through the server.
struct ClickFinder {
    double last = -1;
    bool found(const uint8_t* samples, int bytes, double now) {
        for (int i = 0; i + 1 < bytes; i += 2) {
            const int16_t value = (int16_t)(samples[i] | samples[i + 1] << 8);
            if (std::abs((int)value) > 1600 && now - last > 0.25) {
                last = now;
                return true;
            }
        }
        return false;
    }
};

// --- one attached device ----------------------------------------------------------

struct IsoPacket {
    uint32_t offset, length;
};

struct Urb {
    uint32_t seqnum;
    Role role;
    std::vector<IsoPacket> packets;
    int64_t firstFrame;   // microphone: the server's frame of its first sample
    uint32_t startFrame;  // what the reply says: the millisecond it started in
    double due;           // when it is complete
};

// The timeline and frame position of one endpoint.
struct Stream {
    double end = 0;      // when what is scheduled ends; 0: not running
    int64_t cursor = 0;  // the server's frame where the next packet's samples go
    int shown = 0;       // requests logged since the stream started
    EndpointStats stats;
    ClickFinder clicks;
};

class Session {
public:
    Session(SOCKET socket, int delayMs) : socket_(socket), delayFrames_(delayMs * kRate / 1000), origin_(nowSeconds()) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~Session() { CloseHandle(timer_); }

    // Until the client goes away.
    void run() {
        std::thread clock([this] { clockMain(); });
        receiveMain();
        stop_ = true;
        clock.join();
    }

private:
    Stream& stream(Role role) {
        return role == Role::Speaker ? speaker_ : role == Role::Feedback ? feedback_ : microphone_;
    }

    // --- receiving -----------------------------------------------------------------

    void receiveMain() {
        for (;;) {
            uint8_t header[48];
            if (!receiveAll(socket_, header, sizeof(header))) {
                say("client went away\n");
                return;
            }
            const uint32_t command = get32(header);
            const uint32_t seqnum = get32(header + 4);
            if (command == 2) {  // CMD_UNLINK
                unlink(seqnum, get32(header + 20));
                continue;
            }
            if (command != 1) {
                say("unknown command %08x, closing\n", command);
                return;
            }
            const bool in = get32(header + 12) == 1;
            const uint32_t endpoint = get32(header + 16);
            const uint32_t flags = get32(header + 20);
            const uint32_t length = get32(header + 24);
            const uint32_t packetCount = get32(header + 32);
            const bool iso = packetCount != 0xFFFFFFFF && packetCount > 0 && endpoint != 0;
            if (length > (16u << 20) || (iso && packetCount > 4096)) {
                say("implausible request (length %u, packets %u), closing\n", length, packetCount);
                return;
            }

            std::vector<uint8_t> data(in ? 0 : length);
            if (!data.empty() && !receiveAll(socket_, data.data(), data.size())) return;
            std::vector<IsoPacket> packets;
            if (iso) {
                std::vector<uint8_t> raw((size_t)packetCount * 16);
                if (!receiveAll(socket_, raw.data(), raw.size())) return;
                for (uint32_t i = 0; i < packetCount; i++) packets.push_back({get32(&raw[i * 16]), get32(&raw[i * 16 + 4])});
            }

            const Role role = roleOf(endpoint, in);
            if (endpoint == 0) {
                control(seqnum, in, header + 40, data, packetCount);
            } else if (iso && role != Role::None) {
                isochronous(seqnum, role, flags, std::move(packets), data);
            } else {
                say("request for endpoint %u (%s), stalled\n", endpoint, in ? "in" : "out");
                reply(seqnum, -32, {}, 0, packetCount, 0, nullptr, nullptr);
            }
        }
    }

    // --- endpoint 0 ----------------------------------------------------------------------

    void control(uint32_t seqnum, bool in, const uint8_t* setup, const std::vector<uint8_t>& data, uint32_t packetCount) {
        const uint8_t type = setup[0], request = setup[1];
        const uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
        const uint16_t index = (uint16_t)(setup[4] | setup[5] << 8);
        const uint16_t length = (uint16_t)(setup[6] | setup[7] << 8);

        bool ok = true;
        std::vector<uint8_t> answer;
        const char* what = "?";
        if (type == 0x80 && request == 6) {
            const int kind = value >> 8, number = value & 0xFF;
            what = "GET_DESCRIPTOR";
            if (kind == 1) answer = deviceDescriptor();
            else if (kind == 2) answer = g_kind->uac2 ? uac2Config() : uac1Config();
            else if (kind == 3) answer = stringDescriptor(number);
            else if (kind == 6 && g_kind->uac2) answer = qualifierDescriptor();
            ok = !answer.empty();
        } else if (type == 0x00 && request == 5) {
            what = "SET_ADDRESS";
        } else if (type == 0x00 && request == 9) {
            what = "SET_CONFIGURATION";
            configuration_ = (uint8_t)value;
        } else if (type == 0x80 && request == 8) {
            what = "GET_CONFIGURATION";
            answer = {configuration_};
        } else if ((type & 0x9F) == 0x80 && request == 0) {
            what = "GET_STATUS";
            answer = {0, 0};
        } else if ((type & 0x7F) <= 0x02 && (request == 1 || request == 3)) {
            what = request == 1 ? "CLEAR_FEATURE" : "SET_FEATURE";
        } else if (type == 0x01 && request == 11) {
            what = "SET_INTERFACE";
            setInterface(index & 0xFF, value);
        } else if (type == 0x81 && request == 10) {
            what = "GET_INTERFACE";
            const int iface = index & 0xFF;
            answer = {(uint8_t)(iface == 1 ? speakerAlt_ : iface == 2 ? micAlt_ : 0)};
        } else if (!g_kind->uac2 && type == 0x22 && request == 0x01) {
            what = "SET_CUR (endpoint)";  // the sampling rate: there is only one
        } else if (!g_kind->uac2 && type == 0xA2 && request >= 0x81 && request <= 0x84) {
            what = "GET (endpoint)";
            answer = {0x80, 0xBB, 0x00};
        } else if (g_kind->uac2 && (type == 0xA1 || type == 0x21) && (index >> 8) == kClockId) {
            // The clock source: 48 kHz, always valid.
            const int selector = value >> 8;
            what = type == 0x21 ? "SET clock" : request == 1 ? "GET clock CUR" : "GET clock RANGE";
            if (type == 0x21) {
                ok = selector == 1 && request == 1;  // setting the one rate it has
            } else if (selector == 1 && request == 1) {
                answer = {0x80, 0xBB, 0x00, 0x00};
            } else if (selector == 1 && request == 2) {
                answer = {1, 0, 0x80, 0xBB, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0, 0};
            } else if (selector == 2 && request == 1) {
                answer = {1};
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (answer.size() > length) answer.resize(length);

        say("[%8.3f] %-20s type %02x req %02x value %04x index %04x length %3u -> %s\n", nowSeconds() - origin_, what,
               type, request, value, index, length, ok ? "ok" : "stall");
        if (!ok) {
            reply(seqnum, -32, {}, 0, packetCount, 0, nullptr, nullptr);
            return;
        }
        reply(seqnum, 0, in ? answer : std::vector<uint8_t>(), in ? (uint32_t)answer.size() : (uint32_t)data.size(),
              packetCount, 0, nullptr, nullptr);
    }

    void setInterface(int iface, int alternate) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (iface == 1) {
            speakerAlt_ = alternate;
            speaker_.end = feedback_.end = 0;
            speaker_.shown = feedback_.shown = 0;
        } else if (iface == 2) {
            micAlt_ = alternate;
            microphone_.end = 0;
            microphone_.shown = 0;
        }
    }

    // --- isochronous -------------------------------------------------------------------

    void isochronous(uint32_t seqnum, Role role, uint32_t flags, std::vector<IsoPacket> packets,
                     const std::vector<uint8_t>& data) {
        Stream& s = stream(role);
        const double now = nowSeconds();
        const double period = role == Role::Feedback ? g_kind->feedbackPeriod : g_kind->dataPeriod;
        const int nominal = g_kind->framesPerPacket();

        auto urb = std::make_unique<Urb>();
        urb->seqnum = seqnum;
        urb->role = role;
        urb->packets = std::move(packets);
        const int count = (int)urb->packets.size();

        std::lock_guard<std::mutex> lock(mutex_);
        // After what is already scheduled on this endpoint; from now if the
        // stream had stopped or fallen behind.
        if (s.end < now) {
            if (s.end > 0) s.stats.restarts++;
            s.end = now;
            s.cursor = std::llround((now - origin_) * kRate);
        }
        const double start = s.end;
        s.end += count * period;
        urb->due = s.end;
        urb->startFrame = (uint32_t)std::llround((start - origin_) * 1000.0);
        urb->firstFrame = s.cursor;

        if (s.shown++ < 4) {
            uint32_t total = 0;
            for (const IsoPacket& p : urb->packets) total += p.length;
            say("[%8.3f] %s urb: flags %08x, %d packets, %u bytes, %.2f ms ahead\n", now - origin_, nameOf(role),
                   flags, count, total, (start - now) * 1000.0);
        }
        s.stats.urbs++;
        s.stats.packets += count;
        EndpointStats::lower(s.stats.leadMinUs, (long)((start - now) * 1e6));
        EndpointStats::raise(s.stats.leadMaxUs, (long)((start - now) * 1e6));

        if (role == Role::Speaker) {
            for (int i = 0; i < count; i++) {
                const IsoPacket& packet = urb->packets[i];
                if (packet.length != (uint32_t)(nominal * kFrameBytes)) s.stats.oddPackets++;
                if ((uint64_t)packet.offset + packet.length > data.size()) continue;
                const uint8_t* samples = data.data() + packet.offset;
                const int frames = (int)(packet.length / kFrameBytes);
                cable_.write(s.cursor, samples, frames);
                if (s.clicks.found(samples, (int)packet.length, now)) {
                    say("click received for the speaker at %.4f s (%.1f ms ahead)\n", now,
                           (start + i * period - now) * 1000.0);
                }
                s.cursor += frames;
            }
        } else if (role == Role::Microphone) {
            s.cursor += (int64_t)count * nominal;
        }
        pending_.push_back(std::move(urb));
    }

    void unlink(uint32_t seqnum, uint32_t target) {
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                if ((*it)->seqnum == target) {
                    pending_.erase(it);
                    found = true;
                    break;
                }
            }
        }
        unlinked_++;
        // -ECONNRESET: taken back before it completed; 0: it had completed already.
        std::vector<uint8_t> out;
        put32(out, 4);
        put32(out, seqnum);
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
        put32(out, found ? (uint32_t)-104 : 0);
        out.insert(out.end(), 24, 0);
        send(out);
    }

    // Completes requests when their time has come.
    void clockMain() {
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        double nextReport = nowSeconds() + 5.0;

        while (!stop_) {
            std::unique_ptr<Urb> due;
            double wait = 0.0005;
            {
                const double asked = nowSeconds();
                std::lock_guard<std::mutex> lock(mutex_);
                EndpointStats::raise(lockWaitMaxUs_, (long)((nowSeconds() - asked) * 1e6));
                auto earliest = std::min_element(pending_.begin(), pending_.end(),
                                                 [](const auto& a, const auto& b) { return a->due < b->due; });
                const double now = nowSeconds();
                if (earliest != pending_.end()) {
                    if ((*earliest)->due <= now) {
                        due = std::move(*earliest);
                        pending_.erase(earliest);
                    } else {
                        wait = std::min(wait, (*earliest)->due - now);
                    }
                }
            }
            if (due) {
                const double late = nowSeconds() - due->due;
                if (late > 0.005) {
                    say("[%8.3f] %s urb completed %.1f ms late (%zu packets); previous loop took %.1f ms\n",
                           nowSeconds() - origin_, nameOf(due->role), late * 1000.0, due->packets.size(),
                           lastLoopUs_ / 1000.0);
                }
                const double started = nowSeconds();
                complete(*due);
                lastLoopUs_ = (long)((nowSeconds() - started) * 1e6);
                continue;
            }
            lastLoopUs_ = 0;
            if (nowSeconds() >= nextReport) {
                nextReport += 5.0;
                const std::string lines = speaker_.stats.take("speaker") + feedback_.stats.take("feedback") +
                                          microphone_.stats.take("microphone");
                const long unlinked = unlinked_.exchange(0);
                if (!lines.empty() || unlinked) {
                    say("[%8.3f] last 5 s:\n%s  unlinked %ld  longest send %.2f ms  longest oversleep %.2f ms"
                           "  longest lock wait %.2f ms\n",
                           nowSeconds() - origin_, lines.c_str(), unlinked, sendMaxUs_.exchange(0) / 1000.0,
                           oversleepMaxUs_.exchange(0) / 1000.0, lockWaitMaxUs_.exchange(0) / 1000.0);
                }
            }
            LARGE_INTEGER relative;
            relative.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)(wait * 1e7));
            SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE);
            const double asleep = nowSeconds();
            WaitForSingleObject(timer_, 100);
            EndpointStats::raise(oversleepMaxUs_, (long)((nowSeconds() - asleep - wait) * 1e6));
        }
        if (task) AvRevertMmThreadCharacteristics(task);
    }

    void complete(const Urb& urb) {
        Stream& s = stream(urb.role);
        EndpointStats::raise(s.stats.lateMaxUs, (long)((nowSeconds() - urb.due) * 1e6));

        std::vector<uint8_t> data;
        std::vector<uint32_t> actual(urb.packets.size());
        if (urb.role == Role::Microphone) {
            const int nominal = g_kind->framesPerPacket();
            std::vector<uint8_t> block((size_t)nominal * kFrameBytes);
            for (size_t i = 0; i < urb.packets.size(); i++) {
                const int64_t frame = urb.firstFrame + (int64_t)i * nominal - delayFrames_;
                s.stats.missing += cable_.read(frame, block.data(), nominal);
                actual[i] = std::min<uint32_t>(urb.packets[i].length, (uint32_t)block.size());
                data.insert(data.end(), block.begin(), block.begin() + actual[i]);  // packed: no gaps between packets
                const double now = nowSeconds();
                if (s.clicks.found(block.data(), (int)actual[i], now)) {
                    say("click sent from the microphone at %.4f s\n", now);
                }
            }
        } else if (urb.role == Role::Feedback) {
            // Frames per packet period, 16.16 fixed point: exactly 6, the server's clock being the reference.
            const uint32_t value = (uint32_t)std::llround(kRate * g_kind->dataPeriod * 65536.0);
            const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
            for (size_t i = 0; i < urb.packets.size(); i++) {
                actual[i] = std::min<uint32_t>(urb.packets[i].length, 4);
                data.insert(data.end(), bytes, bytes + actual[i]);
            }
        } else {
            for (size_t i = 0; i < urb.packets.size(); i++) actual[i] = urb.packets[i].length;
        }
        uint32_t total = 0;
        for (uint32_t value : actual) total += value;
        reply(urb.seqnum, 0, data, total, (uint32_t)urb.packets.size(), urb.startFrame, &urb, &actual);
    }

    // RET_SUBMIT: the header, what was read (IN only), and the packets of an
    // isochronous request.
    void reply(uint32_t seqnum, int32_t status, const std::vector<uint8_t>& data, uint32_t actualLength,
               uint32_t packetCount, uint32_t startFrame, const Urb* urb, const std::vector<uint32_t>* actual) {
        std::vector<uint8_t> out;
        out.reserve(48 + data.size() + (urb ? urb->packets.size() * 16 : 0));
        put32(out, 3);
        put32(out, seqnum);
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
        put32(out, (uint32_t)status);
        put32(out, actualLength);
        put32(out, urb ? startFrame : 0);
        put32(out, packetCount);
        put32(out, 0);  // error count
        out.insert(out.end(), 8, 0);
        out.insert(out.end(), data.begin(), data.end());
        if (urb) {
            for (size_t i = 0; i < urb->packets.size(); i++) {
                put32(out, urb->packets[i].offset);
                put32(out, urb->packets[i].length);
                put32(out, (*actual)[i]);
                put32(out, 0);
            }
        }
        send(out);
    }

    void send(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        const double start = nowSeconds();
        sendAll(socket_, data);
        EndpointStats::raise(sendMaxUs_, (long)((nowSeconds() - start) * 1e6));
    }

    SOCKET socket_;
    const int delayFrames_;
    const double origin_;
    HANDLE timer_ = nullptr;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;  // the pending requests and the streams' timelines
    std::vector<std::unique_ptr<Urb>> pending_;
    Stream speaker_, feedback_, microphone_;
    int speakerAlt_ = 0, micAlt_ = 0;
    uint8_t configuration_ = 0;

    std::mutex sendMutex_;
    Cable cable_;
    std::atomic<long> unlinked_{0};
    std::atomic<long> sendMaxUs_{0}, oversleepMaxUs_{0}, lockWaitMaxUs_{0};
    long lastLoopUs_ = 0;  // clock thread only
};

// --- connections -----------------------------------------------------------------------

void serve(SOCKET client, int delayMs) {
    uint8_t header[8];
    if (!receiveAll(client, header, sizeof(header))) return;
    const uint16_t version = get16(header), code = get16(header + 2);
    const uint8_t protocol = g_kind->uac2 ? 0x20 : 0x00;

    if (code == 0x8005) {  // OP_REQ_DEVLIST
        std::vector<uint8_t> out;
        put16(out, version);
        put16(out, 0x0005);
        put32(out, 0);
        put32(out, 1);
        putDevice(out);
        out.insert(out.end(), {0x01, 0x01, protocol, 0x00, 0x01, 0x02, protocol, 0x00, 0x01, 0x02, protocol, 0x00});
        sendAll(client, out);
        say("listed the device\n");
        return;
    }
    if (code != 0x8003) {  // OP_REQ_IMPORT
        say("unknown operation %04x\n", code);
        return;
    }
    char busId[32];
    if (!receiveAll(client, busId, sizeof(busId))) return;
    busId[31] = 0;
    std::vector<uint8_t> out;
    put16(out, version);
    put16(out, 0x0003);
    const bool known = strcmp(busId, kBusId) == 0;
    put32(out, known ? 0 : 1);
    if (known) putDevice(out);
    sendAll(client, out);
    if (!known) {
        say("import of unknown bus id \"%s\" refused\n", busId);
        return;
    }
    say("attached (protocol version %04x)\n", version);
    Session(client, delayMs).run();
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);  // written only by the output thread, which flushes
    int next = 1;
    if (argc > next && (strcmp(argv[next], "uac1") == 0 || strcmp(argv[next], "uac2") == 0)) {
        g_kind = strcmp(argv[next], "uac1") == 0 ? &kUac1 : &kUac2;
        next++;
    }
    const int port = argc > next ? atoi(argv[next]) : 3241;
    const int delayMs = argc > next + 1 ? atoi(argv[next + 1]) : 1;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    timeBeginPeriod(1);
    // A process without a visible window is slowed down by Windows 11 (EcoQoS)
    // and its timer requests are ignored; a device has to keep time regardless.
    struct {
        ULONG version, controlMask, stateMask;
    } throttling = {1, 0x1 | 0x4, 0};  // execution speed, timer resolution: both off
    using SetInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    const auto setInformation =
        (SetInformation)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    if (!setInformation ||
        !setInformation(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &throttling, sizeof(throttling))) {
        say("could not opt out of power throttling (error %lu)\n", GetLastError());
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        fprintf(stderr, "cannot listen on 127.0.0.1:%d (error %d)\n", port, WSAGetLastError());
        return 1;
    }
    say("serving \"%s\" (%s) on 127.0.0.1:%d, bus id %s, microphone %d ms behind the speaker\n", g_kind->name,
           g_kind->uac2 ? "USB Audio 2.0, high speed" : "USB Audio 1.0, full speed", port, kBusId, delayMs);
    say("attach with:  usbip --tcp-port %d attach -r 127.0.0.1 -b %s\n", port, kBusId);

    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        serve(client, delayMs);
        closesocket(client);
    }
    closesocket(listener);
    WSACleanup();
    return 0;
}
