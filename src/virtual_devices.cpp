#include "virtual_devices.h"

#include "usbip_server.h"

#include "com.h"
#include "common.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace wolfe {

namespace {

// After asking usbip-win2 to attach a device, how long it may take to connect.
constexpr double kConnectSeconds = 3.0;
constexpr double kRetrySeconds = 5.0;

struct ProgramResult {
    bool ran = false;
    DWORD exitCode = 0;
    std::string output;  // what it printed, both streams
};

// Runs a console program without a window and collects what it prints.
ProgramResult runProgram(const std::wstring& program, const std::wstring& arguments) {
    ProgramResult result;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return result;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process = {};
    std::wstring commandLine = L"\"" + program + L"\" " + arguments;
    const BOOL started = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        return result;
    }
    char buffer[512];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) result.output.append(buffer, got);
    WaitForSingleObject(process.hProcess, 10000);
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readEnd);
    result.ran = true;
    return result;
}

// usbip-win2's command line program, or empty if it is not installed.
std::wstring findUsbip() {
    wchar_t folder[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", folder, MAX_PATH)) {
        std::wstring path = std::wstring(folder) + L"\\USBip\\usbip.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return {};
}

std::string firstLine(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    const size_t end = text.find_first_of("\r\n");
    return end == std::string::npos ? text : text.substr(0, end);
}

class Device : public VirtualDevice {
public:
    Device(const VirtualDeviceSpec& spec, std::string busId, std::shared_ptr<const GraphClock> clock)
        : spec_(spec), usb_(std::make_shared<UsbIpDevice>()) {
        usb_->busId = std::move(busId);
        usb_->clock = std::move(clock);
        usb_->direction = spec.recording ? UsbIpDevice::Direction::Recording : UsbIpDevice::Direction::Playback;
        usb_->name = spec.name;
        usb_->serial = spec.serial;
        usb_->timeline = std::make_shared<FrameTimeline>();
    }

    const VirtualDeviceSpec& spec() const override { return spec_; }
    const std::shared_ptr<FrameTimeline>& timeline() const override { return usb_->timeline; }
    bool streaming() const override { return usb_->connected && usb_->streaming; }

    std::string takeTiming() override {
        char text[160];
        snprintf(text, sizeof(text), "restarts %ld  jumps %ld  missing %ld  late max %.1f ms", usb_->restarts.exchange(0),
                 usb_->jumps.exchange(0), usb_->missingFrames.exchange(0), usb_->lateMaxUs.exchange(0) / 1000.0);
        return text;
    }

    std::string status() const override {
        if (usb_->connected) {
            if (!usb_->streaming) return "ready: Windows has it, nothing is using it";
            return spec_.recording ? "an application is recording from it" : "an application is playing on it";
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return problem_.empty() ? "being added to Windows" : problem_;
    }

    void setProblem(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text != problem_ && !text.empty()) logLine("Wolfe device \"%s\": %s", spec_.name.c_str(), text.c_str());
        problem_ = text;
    }

    const std::shared_ptr<UsbIpDevice>& usb() const { return usb_; }

    // Keeper thread only.
    int port = 0;              // where usbip-win2 has it plugged in; 0: not attached
    double attachedAt = 0;     // when it was attached, to give it time to connect
    double nextAttempt = 0;
    std::atomic<bool> removed{false};

private:
    VirtualDeviceSpec spec_;
    std::shared_ptr<UsbIpDevice> usb_;
    mutable std::mutex mutex_;
    std::string problem_;
};

}  // namespace

struct VirtualDevices::Impl {
    std::shared_ptr<const GraphClock> clock;
    int port;
    UsbIpServer server;
    std::string serverProblem;  // why the server could not start; empty when running
    std::wstring usbip;

    std::mutex mutex;  // devices, stop
    std::condition_variable wake;
    std::vector<std::shared_ptr<Device>> devices;
    bool stop = false;
    std::thread keeper;

    std::string attach(Device& device) {
        if (usbip.empty()) usbip = findUsbip();
        if (usbip.empty()) return "usbip-win2 is not installed: Wolfe's own devices need it";
        const std::wstring arguments = L"--tcp-port " + std::to_wstring(port) + L" attach -r 127.0.0.1 -b " +
                                       toWide(device.usb()->busId) + L" --once --terse";
        const ProgramResult result = runProgram(usbip, arguments);
        if (!result.ran) return "usbip-win2 could not be started";
        const int pluggedIn = atoi(result.output.c_str());
        if (result.exitCode != 0 || pluggedIn <= 0) return "usbip-win2 could not attach it: " + firstLine(result.output);
        device.port = pluggedIn;
        device.attachedAt = nowSeconds();
        return {};
    }

    void detach(Device& device) {
        if (device.port <= 0 || usbip.empty()) return;
        runProgram(usbip, L"detach -p " + std::to_wstring(device.port));
        device.port = 0;
    }

    void keeperMain() {
        ComApartment com;
        std::unique_lock<std::mutex> lock(mutex);
        while (!stop) {
            std::vector<std::shared_ptr<Device>> current = devices;
            lock.unlock();

            for (const auto& device : current) {
                if (device->removed) continue;
                const double now = nowSeconds();
                if (!serverProblem.empty()) {
                    device->setProblem("Wolfe's devices cannot be offered: " + serverProblem);
                    continue;
                }
                if (device->usb()->connected) {
                    device->setProblem({});
                    continue;
                }
                if (device->port > 0) {
                    if (now - device->attachedAt < kConnectSeconds) continue;
                    // Attached once, but no longer connected: start over.
                    detach(*device);
                    device->nextAttempt = now;
                }
                if (now < device->nextAttempt) continue;
                const std::string problem = attach(*device);
                device->setProblem(problem);
                if (!problem.empty()) device->nextAttempt = now + kRetrySeconds;
            }

            lock.lock();
            // Devices taken away are detached here, out of everyone's way.
            for (auto it = devices.begin(); it != devices.end();) {
                if (!(*it)->removed) {
                    ++it;
                    continue;
                }
                std::shared_ptr<Device> device = *it;
                it = devices.erase(it);
                lock.unlock();
                detach(*device);
                server.remove(device->usb()->busId);
                lock.lock();
            }
            wake.wait_for(lock, std::chrono::milliseconds(500));
        }
        // Wolfe is going away: so are its devices.
        std::vector<std::shared_ptr<Device>> remaining;
        remaining.swap(devices);
        lock.unlock();
        for (const auto& device : remaining) {
            detach(*device);
            server.remove(device->usb()->busId);
        }
    }

    std::string freeBusId() const {
        std::set<std::string> used;
        for (const auto& device : devices) used.insert(device->usb()->busId);
        for (int n = 1;; n++) {
            const std::string candidate = "1-" + std::to_string(n);
            if (!used.count(candidate)) return candidate;
        }
    }
};

VirtualDevices::VirtualDevices(std::shared_ptr<const GraphClock> clock, int port) : impl_(std::make_unique<Impl>()) {
    impl_->clock = std::move(clock);
    impl_->port = port;
    if (!impl_->server.start(port, &impl_->serverProblem)) logLine("Wolfe's devices: %s", impl_->serverProblem.c_str());
    impl_->keeper = std::thread([this] { impl_->keeperMain(); });
}

VirtualDevices::~VirtualDevices() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stop = true;
    }
    impl_->wake.notify_all();
    impl_->keeper.join();
    impl_->server.stop();
}

std::shared_ptr<VirtualDevice> VirtualDevices::add(const VirtualDeviceSpec& spec) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto device = std::make_shared<Device>(spec, impl_->freeBusId(), impl_->clock);
    impl_->server.add(device->usb());
    impl_->devices.push_back(device);
    impl_->wake.notify_all();
    return device;
}

void VirtualDevices::remove(const std::shared_ptr<VirtualDevice>& device) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& candidate : impl_->devices) {
        if (candidate == device) candidate->removed = true;
    }
    impl_->wake.notify_all();
}

std::string serialFor(const std::string& text) {
    uint32_t hash = 2166136261u;  // FNV-1a
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    char serial[16];
    snprintf(serial, sizeof(serial), "W%08X", hash);
    return serial;
}

}  // namespace wolfe
