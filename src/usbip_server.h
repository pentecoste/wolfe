#pragma once

#include "frame_timeline.h"

#include <atomic>
#include <memory>
#include <string>

namespace wolfe {

// One of Wolfe's own sound devices, as the USB/IP server offers it.
//
// It is a USB Audio Class 2 device with a single stream: a speaker that
// applications play into (Playback: the audio is written to `timeline`) or a
// microphone they record from (Recording: the audio is read from `timeline`).
// Windows drives it with its own USB audio driver, as any sound card.
struct UsbIpDevice {
    enum class Direction { Playback, Recording };

    // Set before the device is offered, never changed afterwards.
    std::string busId;  // "1-3": what usbip attaches
    Direction direction = Direction::Playback;
    std::string name;    // what Windows calls the device
    std::string serial;  // its identity for Windows (see usbip_server.cpp)
    std::shared_ptr<FrameTimeline> timeline;
    // Recording: how far behind the present the microphone reads the timeline,
    // which gives the graph time to have written it.
    int64_t readBehindFrames = 0;

    // Kept up to date by the server.
    std::atomic<bool> connected{false};  // usbip-win2 has it, Windows sees it
    std::atomic<bool> streaming{false};  // an application is playing or recording on it
    std::atomic<long> restarts{0};       // times the stream stopped and started again
    std::atomic<long> missingFrames{0};  // recording: frames the graph had not written in time
    std::atomic<long> lateMaxUs{0};      // worst lateness of a request's completion since the last look
};

// Serves Wolfe's devices over USB/IP on 127.0.0.1, for usbip-win2 to attach
// to Windows. Each attached device has a connection of its own; its timing is
// kept on the performance counter, the graph's clock: a request for n packets
// is completed when their time has passed, as real hardware would.
class UsbIpServer {
public:
    UsbIpServer();
    ~UsbIpServer();

    // Returns false, with the reason in `error`, if the port cannot be had.
    bool start(int port, std::string* error);
    void stop();

    void add(std::shared_ptr<UsbIpDevice> device);
    // Stops offering the device and ends its connection, if it has one.
    void remove(const std::string& busId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wolfe
