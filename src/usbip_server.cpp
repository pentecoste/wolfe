#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "usbip_server.h"

#include "com.h"
#include "common.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace wolfe {

namespace {

// --- the device, as USB describes it ------------------------------------------
//
// Identity. Windows names a device's endpoints after vendor id, product id,
// release and serial number, and gives them new ids (losing which applications
// were assigned to them) if any of these change. They must stay as they are
// once people use them; the descriptors below may change.
//
// 1209 is pid.codes, the vendor id shared by open source hardware; 0003 and
// 0004 are placeholders until Wolfe has product ids of its own there.
constexpr uint16_t kVendor = 0x1209;
constexpr uint16_t kPlaybackProduct = 0x0003;
constexpr uint16_t kRecordingProduct = 0x0004;
constexpr uint16_t kRelease = 0x0100;

// High speed: one packet every 125 microseconds, 6 frames of 32-bit stereo PCM.
constexpr double kPacketSeconds = 0.000125;
constexpr double kFeedbackSeconds = 0.001;  // one feedback value per millisecond
constexpr int kFramesPerPacket = 6;
constexpr int kFrameBytes = 8;
constexpr int kMaxPacket = (kFramesPerPacket + 1) * kFrameBytes;
constexpr int kClockId = 0x10;

constexpr uint32_t kSpeedHigh = 3;  // in USB/IP's numbering
constexpr int32_t kEPIPE = -32, kECONNRESET = -104;

void add(std::vector<uint8_t>& out, std::initializer_list<int> bytes) {
    for (int b : bytes) out.push_back((uint8_t)b);
}
void setLength16(std::vector<uint8_t>& out, size_t at, size_t value) {
    out[at] = (uint8_t)(value & 0xFF);
    out[at + 1] = (uint8_t)(value >> 8);
}

uint16_t productOf(const UsbIpDevice& device) {
    return device.direction == UsbIpDevice::Direction::Playback ? kPlaybackProduct : kRecordingProduct;
}

std::vector<uint8_t> deviceDescriptor(const UsbIpDevice& device) {
    const int product = productOf(device);
    std::vector<uint8_t> d = {18, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 64};  // USB 2.0, interface association
    add(d, {kVendor & 0xFF, kVendor >> 8, product & 0xFF, product >> 8, kRelease & 0xFF, kRelease >> 8, 1, 2, 3, 1});
    return d;
}

std::vector<uint8_t> qualifierDescriptor() { return {10, 0x06, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0}; }

// Audio control: a clock source and two terminals. Playback: USB streaming
// (1) feeds a speaker (3); recording: a microphone (4) feeds USB streaming (6).
// Then one streaming interface, 32-bit stereo PCM at 48 kHz.
std::vector<uint8_t> configDescriptor(const UsbIpDevice& device) {
    const bool playback = device.direction == UsbIpDevice::Direction::Playback;
    std::vector<uint8_t> c = {9, 0x02, 0, 0, 2, 1, 0, 0x80, 50};
    add(c, {8, 0x0B, 0, 2, 0x01, 0x00, 0x20, 0});       // interface association: 0 and 1 are one audio function
    add(c, {9, 0x04, 0, 0, 0, 0x01, 0x01, 0x20, 0});    // audio control, UAC2
    const size_t header = c.size();
    add(c, {9, 0x24, 0x01, 0x00, 0x02, 0x08, 0, 0, 0});  // header: ADC 2.0, category I/O box
    add(c, {8, 0x24, 0x0A, kClockId, 0x01, 0x05, 0, 0});  // clock source: internal, fixed; rate and validity readable
    if (playback) {
        add(c, {17, 0x24, 0x02, 1, 0x01, 0x01, 0, kClockId, 2, 0x03, 0, 0, 0, 0, 0, 0, 0});  // IT 1: USB streaming
        add(c, {12, 0x24, 0x03, 3, 0x01, 0x03, 0, 1, kClockId, 0, 0, 0});                    // OT 3: speaker
    } else {
        add(c, {17, 0x24, 0x02, 4, 0x01, 0x02, 0, kClockId, 2, 0x03, 0, 0, 0, 0, 0, 0, 0});  // IT 4: microphone
        add(c, {12, 0x24, 0x03, 6, 0x01, 0x01, 0, 4, kClockId, 0, 0, 0});                    // OT 6: USB streaming
    }
    setLength16(c, header + 6, c.size() - header);

    add(c, {9, 0x04, 1, 0, 0, 0x01, 0x02, 0x20, 0});
    add(c, {9, 0x04, 1, 1, playback ? 2 : 1, 0x01, 0x02, 0x20, 0});
    add(c, {16, 0x24, 0x01, playback ? 1 : 6, 0, 0x01, 0x01, 0, 0, 0, 2, 0x03, 0, 0, 0, 0});  // AS general: PCM, 2 ch
    add(c, {6, 0x24, 0x02, 0x01, 4, 32});                                                       // type I: 32 bits in 4 bytes
    // Asynchronous both ways: the device keeps the time. The speaker tells the
    // host how many frames to send through a feedback endpoint (1 IN).
    add(c, {7, 0x05, playback ? 0x01 : 0x81, 0x05, kMaxPacket & 0xFF, kMaxPacket >> 8, 1});
    add(c, {8, 0x25, 0x01, 0, 0, 0, 0, 0});
    if (playback) add(c, {7, 0x05, 0x81, 0x11, 4, 0, 4});  // feedback, every 8 microframes
    setLength16(c, 2, c.size());
    return c;
}

std::vector<uint8_t> stringDescriptor(const UsbIpDevice& device, int index) {
    if (index == 0) return {4, 0x03, 0x09, 0x04};  // US English
    const std::string text = index == 1 ? "Wolfe" : index == 2 ? device.name : index == 3 ? device.serial : "";
    if (text.empty()) return {};
    const std::wstring wide = toWide(text);
    std::vector<uint8_t> result = {0, 0x03};
    for (wchar_t c : wide) add(result, {c & 0xFF, (c >> 8) & 0xFF});
    result[0] = (uint8_t)std::min<size_t>(result.size(), 254);
    result.resize(result[0]);
    return result;
}

// --- bytes on the wire (USB/IP is big-endian) ------------------------------------

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((uint8_t)(value >> shift));
}
void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}
void putText(std::vector<uint8_t>& out, const std::string& text, size_t size) {
    const size_t length = std::min(text.size(), size - 1);
    out.insert(out.end(), text.begin(), text.begin() + (ptrdiff_t)length);
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
void putDevice(std::vector<uint8_t>& out, const UsbIpDevice& device) {
    putText(out, "/sys/devices/platform/wolfe/" + device.busId, 256);
    putText(out, device.busId, 32);
    const size_t dash = device.busId.find('-');
    put32(out, 1);
    put32(out, dash == std::string::npos ? 2u : (uint32_t)atoi(device.busId.c_str() + dash + 1) + 1);
    put32(out, kSpeedHigh);
    put16(out, kVendor);
    put16(out, productOf(device));
    put16(out, kRelease);
    out.insert(out.end(), {0xEF, 0x02, 0x01, 1, 1, 2});  // class, subclass, protocol, configuration, configurations, interfaces
}

void raise(std::atomic<long>& value, long candidate) {
    long seen = value.load();
    while (candidate > seen && !value.compare_exchange_weak(seen, candidate)) {}
}

// --- one attached device ----------------------------------------------------------

enum class Role { None, Data, Feedback };

struct IsoPacket {
    uint32_t offset, length;
};

struct Urb {
    uint32_t seqnum;
    Role role;
    std::vector<IsoPacket> packets;
    int64_t firstFrame;   // recording: the graph frame of its first sample
    uint32_t startFrame;  // what the reply says: the millisecond it started in
    double due;           // when it is complete
};

// The timeline of one endpoint.
struct Stream {
    double end = 0;      // when what is scheduled ends; 0: not running
    int64_t cursor = 0;  // the graph frame where the next packet's samples go
};

}  // namespace

class Session {
public:
    Session(SOCKET socket, std::shared_ptr<UsbIpDevice> device) : socket_(socket), device_(std::move(device)) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~Session() { CloseHandle(timer_); }

    // Until the connection ends.
    void run() {
        device_->connected = true;
        std::thread clock([this] { clockMain(); });
        receiveMain();
        stop_ = true;
        clock.join();
        device_->connected = false;
        device_->streaming = false;
    }

private:
    bool playback() const { return device_->direction == UsbIpDevice::Direction::Playback; }

    Role roleOf(uint32_t endpoint, bool in) const {
        if (endpoint != 1) return Role::None;
        if (playback()) return in ? Role::Feedback : Role::Data;
        return in ? Role::Data : Role::None;
    }

    void receiveMain() {
        for (;;) {
            uint8_t header[48];
            if (!receiveAll(socket_, header, sizeof(header))) return;
            const uint32_t command = get32(header);
            const uint32_t seqnum = get32(header + 4);
            if (command == 2) {  // CMD_UNLINK
                unlink(seqnum, get32(header + 20));
                continue;
            }
            if (command != 1) {
                logLine("Wolfe device \"%s\": unknown USB/IP command %08x, closing", device_->name.c_str(), command);
                return;
            }
            const bool in = get32(header + 12) == 1;
            const uint32_t endpoint = get32(header + 16);
            const uint32_t length = get32(header + 24);
            const uint32_t packetCount = get32(header + 32);
            const bool iso = packetCount != 0xFFFFFFFF && packetCount > 0 && endpoint != 0;
            if (length > (16u << 20) || (iso && packetCount > 4096)) {
                logLine("Wolfe device \"%s\": implausible request, closing", device_->name.c_str());
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
                isochronous(seqnum, role, std::move(packets), data);
            } else {
                reply(seqnum, kEPIPE, {}, 0, packetCount, 0, nullptr, nullptr);
            }
        }
    }

    // --- endpoint 0 ----------------------------------------------------------------

    void control(uint32_t seqnum, bool in, const uint8_t* setup, const std::vector<uint8_t>& data, uint32_t packetCount) {
        const uint8_t type = setup[0], request = setup[1];
        const uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
        const uint16_t index = (uint16_t)(setup[4] | setup[5] << 8);
        const uint16_t length = (uint16_t)(setup[6] | setup[7] << 8);

        bool ok = true;
        std::vector<uint8_t> answer;
        if (type == 0x80 && request == 6) {  // GET_DESCRIPTOR
            const int kind = value >> 8, number = value & 0xFF;
            if (kind == 1) answer = deviceDescriptor(*device_);
            else if (kind == 2) answer = configDescriptor(*device_);
            else if (kind == 3) answer = stringDescriptor(*device_, number);
            else if (kind == 6) answer = qualifierDescriptor();
            ok = !answer.empty();
        } else if (type == 0x00 && (request == 5 || request == 9)) {  // SET_ADDRESS, SET_CONFIGURATION
            if (request == 9) configuration_ = (uint8_t)value;
        } else if (type == 0x80 && request == 8) {  // GET_CONFIGURATION
            answer = {configuration_};
        } else if ((type & 0x9F) == 0x80 && request == 0) {  // GET_STATUS
            answer = {0, 0};
        } else if ((type & 0x7F) <= 0x02 && (request == 1 || request == 3)) {  // CLEAR_FEATURE, SET_FEATURE
        } else if (type == 0x01 && request == 11) {  // SET_INTERFACE
            if ((index & 0xFF) == 1) setStreaming(value != 0);
        } else if (type == 0x81 && request == 10) {  // GET_INTERFACE
            answer = {(uint8_t)((index & 0xFF) == 1 && alternate_ ? 1 : 0)};
        } else if ((type == 0xA1 || type == 0x21) && (index >> 8) == kClockId) {
            // The clock source: 48 kHz, always valid.
            const int selector = value >> 8;
            if (type == 0x21) ok = selector == 1 && request == 1;  // setting the one rate it has
            else if (selector == 1 && request == 1) answer = {0x80, 0xBB, 0x00, 0x00};
            else if (selector == 1 && request == 2) answer = {1, 0, 0x80, 0xBB, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0, 0};
            else if (selector == 2 && request == 1) answer = {1};
            else ok = false;
        } else {
            ok = false;
        }
        if (answer.size() > length) answer.resize(length);
        if (!ok) {
            reply(seqnum, kEPIPE, {}, 0, packetCount, 0, nullptr, nullptr);
            return;
        }
        reply(seqnum, 0, in ? answer : std::vector<uint8_t>(), in ? (uint32_t)answer.size() : (uint32_t)data.size(),
              packetCount, 0, nullptr, nullptr);
    }

    void setStreaming(bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        alternate_ = on;
        data_.end = feedback_.end = 0;
        device_->streaming = on;
    }

    // --- isochronous -------------------------------------------------------------------

    void isochronous(uint32_t seqnum, Role role, std::vector<IsoPacket> packets, const std::vector<uint8_t>& data) {
        Stream& s = role == Role::Feedback ? feedback_ : data_;
        const double period = role == Role::Feedback ? kFeedbackSeconds : kPacketSeconds;
        const double now = nowSeconds();

        auto urb = std::make_unique<Urb>();
        urb->seqnum = seqnum;
        urb->role = role;
        urb->packets = std::move(packets);
        const int count = (int)urb->packets.size();

        // Samples are converted outside the lock that the clock thread needs.
        std::vector<float> samples;
        if (role == Role::Data && playback()) {
            for (const IsoPacket& packet : urb->packets) {
                if ((uint64_t)packet.offset + packet.length > data.size()) continue;
                const uint8_t* at = data.data() + packet.offset;
                for (uint32_t i = 0; i + 4 <= packet.length; i += 4) {
                    const int32_t value = (int32_t)((uint32_t)at[i] | (uint32_t)at[i + 1] << 8 |
                                                    (uint32_t)at[i + 2] << 16 | (uint32_t)at[i + 3] << 24);
                    samples.push_back((float)value / 2147483648.0f);
                }
            }
        }

        int64_t cursor;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // After what is already scheduled on this endpoint; from now if the
            // stream had stopped or fallen behind.
            if (s.end < now) {
                if (s.end > 0) device_->restarts++;
                s.end = now;
                s.cursor = std::llround(now * kSampleRate);
            }
            const double start = s.end;
            s.end += count * period;
            urb->due = s.end;
            urb->startFrame = (uint32_t)std::llround(start * 1000.0);
            urb->firstFrame = s.cursor;
            cursor = s.cursor;
            if (role == Role::Data) {
                s.cursor += playback() ? (int64_t)(samples.size() / 2) : (int64_t)count * kFramesPerPacket;
            }
            pending_.push_back(std::move(urb));
        }
        if (!samples.empty()) device_->timeline->write(cursor, samples.data(), (int)(samples.size() / 2));
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
        // -ECONNRESET: taken back before it completed; 0: it had completed already.
        std::vector<uint8_t> out;
        put32(out, 4);
        put32(out, seqnum);
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
        put32(out, found ? (uint32_t)kECONNRESET : 0);
        out.insert(out.end(), 24, 0);
        send(out);
    }

    // Completes requests when their time has come. Nothing in here may wait
    // for anything slow: a device that is late is heard.
    void clockMain() {
        AudioThreadPriority priority;
        while (!stop_) {
            std::unique_ptr<Urb> due;
            double wait = 0.0005;
            {
                std::lock_guard<std::mutex> lock(mutex_);
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
                complete(*due);
                continue;
            }
            LARGE_INTEGER relative;
            relative.QuadPart = -std::max<LONGLONG>(1, (LONGLONG)(wait * 1e7));
            SetWaitableTimer(timer_, &relative, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer_, 100);
        }
    }

    void complete(const Urb& urb) {
        raise(device_->lateMaxUs, (long)((nowSeconds() - urb.due) * 1e6));

        std::vector<uint8_t> data;
        std::vector<uint32_t> actual(urb.packets.size());
        if (urb.role == Role::Data && !playback()) {
            float frames[kFramesPerPacket * 2];
            for (size_t i = 0; i < urb.packets.size(); i++) {
                const int64_t first = urb.firstFrame + (int64_t)i * kFramesPerPacket - device_->readBehindFrames;
                device_->missingFrames += device_->timeline->read(first, kFramesPerPacket, frames);
                actual[i] = std::min<uint32_t>(urb.packets[i].length, kFramesPerPacket * kFrameBytes);
                for (uint32_t b = 0; b + 4 <= actual[i]; b += 4) {
                    const float sample = std::clamp(frames[b / 4], -1.0f, 1.0f);
                    const int32_t value = (int32_t)std::lround((double)sample * 2147483647.0);
                    add(data, {value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF});
                }
            }
        } else if (urb.role == Role::Feedback) {
            // Frames per packet period, 16.16 fixed point: exactly 6, the
            // device's clock being the graph's.
            const uint32_t value = (uint32_t)kFramesPerPacket << 16;
            for (size_t i = 0; i < urb.packets.size(); i++) {
                actual[i] = std::min<uint32_t>(urb.packets[i].length, 4);
                const uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
                data.insert(data.end(), bytes, bytes + actual[i]);  // packed: no gaps between packets
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
        sendAll(socket_, data);
    }

    SOCKET socket_;
    std::shared_ptr<UsbIpDevice> device_;
    HANDLE timer_ = nullptr;
    std::atomic<bool> stop_{false};

    std::mutex mutex_;  // the pending requests and the streams
    std::vector<std::unique_ptr<Urb>> pending_;
    Stream data_, feedback_;
    bool alternate_ = false;
    uint8_t configuration_ = 0;

    std::mutex sendMutex_;
};

// --- the server -------------------------------------------------------------------------

struct UsbIpServer::Impl {
    SOCKET listener = INVALID_SOCKET;
    std::thread acceptThread;
    bool winsock = false;

    std::mutex mutex;  // what follows
    std::map<std::string, std::shared_ptr<UsbIpDevice>> devices;
    struct Connection {
        SOCKET socket;
        std::string busId;  // empty until a device is imported
        std::thread thread;
        std::atomic<bool> done{false};
    };
    std::vector<std::unique_ptr<Connection>> connections;

    // Joins the threads of connections that have ended.
    void reap() {
        std::vector<std::unique_ptr<Connection>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = connections.begin(); it != connections.end();) {
                if ((*it)->done) {
                    finished.push_back(std::move(*it));
                    it = connections.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& connection : finished) connection->thread.join();
    }

    void acceptMain() {
        for (;;) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            BOOL noDelay = TRUE;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
            reap();
            auto connection = std::make_unique<Connection>();
            connection->socket = client;
            Connection* raw = connection.get();
            {
                std::lock_guard<std::mutex> lock(mutex);
                connections.push_back(std::move(connection));
            }
            raw->thread = std::thread([this, raw] {
                serve(*raw);
                closesocket(raw->socket);
                raw->done = true;
            });
        }
    }

    void serve(Connection& connection) {
        uint8_t header[8];
        if (!receiveAll(connection.socket, header, sizeof(header))) return;
        const uint16_t version = get16(header), code = get16(header + 2);

        if (code == 0x8005) {  // OP_REQ_DEVLIST
            std::vector<std::shared_ptr<UsbIpDevice>> offered;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [busId, device] : devices) offered.push_back(device);
            }
            std::vector<uint8_t> out;
            put16(out, version);
            put16(out, 0x0005);
            put32(out, 0);
            put32(out, (uint32_t)offered.size());
            for (const auto& device : offered) {
                putDevice(out, *device);
                out.insert(out.end(), {0x01, 0x01, 0x20, 0x00, 0x01, 0x02, 0x20, 0x00});
            }
            sendAll(connection.socket, out);
            return;
        }
        if (code != 0x8003) return;  // OP_REQ_IMPORT is all that is left

        char busId[32];
        if (!receiveAll(connection.socket, busId, sizeof(busId))) return;
        busId[31] = 0;
        std::shared_ptr<UsbIpDevice> device;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = devices.find(busId);
            if (found != devices.end()) device = found->second;
            connection.busId = busId;
        }
        std::vector<uint8_t> out;
        put16(out, version);
        put16(out, 0x0003);
        put32(out, device ? 0 : 1);
        if (device) putDevice(out, *device);
        sendAll(connection.socket, out);
        if (device) Session(connection.socket, device).run();
    }
};

UsbIpServer::UsbIpServer() : impl_(std::make_unique<Impl>()) {}
UsbIpServer::~UsbIpServer() { stop(); }

bool UsbIpServer::start(int port, std::string* error) {
    if (impl_->listener != INVALID_SOCKET) return true;
    WSADATA wsa;
    if (!impl_->winsock) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            if (error) *error = "the network layer could not start";
            return false;
        }
        impl_->winsock = true;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 8) != 0) {
        if (error) *error = "port " + std::to_string(port) + " is taken (error " + std::to_string(WSAGetLastError()) + ")";
        if (listener != INVALID_SOCKET) closesocket(listener);
        return false;
    }
    impl_->listener = listener;
    impl_->acceptThread = std::thread([this] { impl_->acceptMain(); });
    return true;
}

void UsbIpServer::stop() {
    if (impl_->listener != INVALID_SOCKET) {
        closesocket(impl_->listener);  // ends accept()
        impl_->listener = INVALID_SOCKET;
        impl_->acceptThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& connection : impl_->connections) shutdown(connection->socket, SD_BOTH);  // ends recv()
    }
    for (;;) {
        impl_->reap();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->connections.empty()) break;
        Sleep(1);
    }
    if (impl_->winsock) {
        WSACleanup();
        impl_->winsock = false;
    }
}

void UsbIpServer::add(std::shared_ptr<UsbIpDevice> device) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->devices[device->busId] = std::move(device);
}

void UsbIpServer::remove(const std::string& busId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->devices.erase(busId);
    for (auto& connection : impl_->connections) {
        if (connection->busId == busId) shutdown(connection->socket, SD_BOTH);
    }
}

}  // namespace wolfe
