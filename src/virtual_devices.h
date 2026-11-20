#pragma once

#include "frame_timeline.h"
#include "graph_clock.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wolfe {

// How Wolfe's devices identify themselves over USB. Windows names a device's
// endpoints after vendor id, product id, release and serial number, and gives
// them new ids (losing which applications were assigned to them) if any of
// these change: they must stay as they are once people use them.
//
// 1209 is pid.codes, the vendor id shared by open source hardware; 0003 and
// 0004 are placeholders until Wolfe has product ids of its own there.
constexpr uint16_t kWolfeVendor = 0x1209;
constexpr uint16_t kWolfeSpeakerProduct = 0x0003;
constexpr uint16_t kWolfeMicrophoneProduct = 0x0004;

struct VirtualDeviceSpec {
    // false: a speaker that applications play into (its audio comes into the
    // graph); true: a microphone they record from (the graph's audio goes out).
    bool recording = false;
    std::string name;    // what Windows calls it
    // Its identity for Windows, which remembers by it which applications use
    // the device: it must stay the same from one run to the next.
    std::string serial;
};

// One of Wolfe's own devices, as the graph sees it.
class VirtualDevice {
public:
    virtual ~VirtualDevice() = default;
    virtual const VirtualDeviceSpec& spec() const = 0;
    // The device's audio on the graph's clock.
    virtual const std::shared_ptr<FrameTimeline>& timeline() const = 0;
    virtual std::string status() const = 0;
    virtual bool streaming() const = 0;  // an application is playing or recording on it
    // How its timing went since the last call, in a few words, for reports.
    virtual std::string takeTiming() = 0;
};

// Wolfe's own sound devices. Windows has them through usbip-win2, which must
// be installed (no administrator rights are needed to use it): Wolfe serves
// them over USB/IP on 127.0.0.1 and asks usbip-win2 to attach them. They are
// detached again when removed, and when Wolfe exits.
//
// They keep the pace of `clock`, the graph's: a speaker is fed by Windows as
// fast as the graph consumes, a microphone delivers as fast as it produces.
class VirtualDevices {
public:
    explicit VirtualDevices(std::shared_ptr<const GraphClock> clock, int port = 3241);
    ~VirtualDevices();

    std::shared_ptr<VirtualDevice> add(const VirtualDeviceSpec& spec);
    void remove(const std::shared_ptr<VirtualDevice>& device);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A serial number for a device, stable for a given text (a node id, say).
std::string serialFor(const std::string& text);

}  // namespace wolfe
