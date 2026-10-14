#include "com.h"
#include "common.h"
#include "config.h"
#include "devices.h"
#include "graph.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wolfe;

namespace {

HANDLE g_quitEvent = nullptr;

BOOL WINAPI onConsoleSignal(DWORD) {
    SetEvent(g_quitEvent);
    return TRUE;
}

void printUsage() {
    printf("Wolfe - audio patchbay, command line\n"
           "\n"
           "  wolfe-cli devices           list the audio devices\n"
           "  wolfe-cli run <file.json>   run a graph until Ctrl+C\n"
           "      --report N            print its state every N seconds (default 5, 0 = never)\n"
           "      --seconds N           stop by itself after N seconds\n");
}

void printDevices() {
    struct {
        EDataFlow flow;
        const char* title;
    } groups[] = {{eCapture, "Inputs (recording devices)"}, {eRender, "Outputs (playback devices)"}};
    for (const auto& group : groups) {
        printf("%s:\n", group.title);
        for (const DeviceInfo& device : listDevices(group.flow)) {
            printf("  %s%s\n", device.name.c_str(), device.isDefault ? "   [default]" : "");
        }
        printf("\n");
    }
}

int runGraph(const std::string& configPath, double reportSeconds, double runSeconds) {
    Graph graph;
    buildGraph(loadDescription(configPath), graph);

    g_quitEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onConsoleSignal, TRUE);

    graph.start();
    logLine("running, press Ctrl+C to stop");
    const double end = runSeconds > 0 ? nowSeconds() + runSeconds : 1e18;
    double nextReport = reportSeconds > 0 ? nowSeconds() + reportSeconds : 1e18;
    for (;;) {
        const double now = nowSeconds();
        if (now >= end) break;
        if (now >= nextReport) {
            logLine("state:\n%s", graph.report().c_str());
            nextReport += reportSeconds;
            continue;
        }
        // Rounded up, so that the wait never ends a moment before the time it is for.
        const double wait = std::min(std::min(end, nextReport) - now, 3600.0);
        if (WaitForSingleObject(g_quitEvent, (DWORD)(wait * 1000.0) + 1) != WAIT_TIMEOUT) break;
    }
    logLine("stopping");
    graph.stop();
    return 0;
}

std::vector<std::string> commandLine() {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> arguments;
    for (int i = 1; i < count; i++) arguments.push_back(toUtf8(wide[i]));
    LocalFree(wide);
    return arguments;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    ComApartment com;
    const std::vector<std::string> arguments = commandLine();

    try {
        if (arguments.size() == 1 && arguments[0] == "devices") {
            printDevices();
            return 0;
        }
        if (arguments.size() >= 2 && arguments[0] == "run") {
            double reportSeconds = 5.0, runSeconds = 0.0;
            for (size_t i = 2; i + 1 < arguments.size(); i++) {
                if (arguments[i] == "--report") reportSeconds = atof(arguments[i + 1].c_str());
                if (arguments[i] == "--seconds") runSeconds = atof(arguments[i + 1].c_str());
            }
            return runGraph(arguments[1], reportSeconds, runSeconds);
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }

    printUsage();
    return arguments.empty() ? 0 : 2;
}
