// The graphical front end: a window with the patch canvas, an icon in the
// notification area, and the engine running underneath.
//
//   wolfe.exe                 open the window
//   wolfe.exe --tray          start hidden in the notification area
//   wolfe.exe --import <path> replace the graph with the one in this file
//   wolfe.exe --file <path>   keep the graph in this file instead of the usual one
//   wolfe.exe --selftest <report>   try the editor's gestures by itself (see selftest.h)
#include "app/controller.h"
#include "common.h"
#include "editor.h"
#include "selftest.h"
#include "shell.h"

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"
#include "imnodes.h"

#include <d3d11.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

using namespace wolfe;

namespace {

const wchar_t* const kWindowClass = L"WolfeMainWindow";
const wchar_t* const kInstanceMutex = L"Local\\WolfeSingleInstance";
const UINT kTrayMessage = WM_APP + 1;   // something happened on the notification-area icon
const UINT kShowMessage = WM_APP + 2;   // a second copy was started: come to the front
const UINT kQuitMessage = WM_APP + 3;   // asked from outside to save and exit
enum { kMenuOpen = 1, kMenuExit = 2 };

// --- Direct3D 11 -----------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;

void createTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        g_device->CreateRenderTargetView(backBuffer, nullptr, &g_target);
        backBuffer->Release();
    }
}

void releaseTarget() {
    if (g_target) {
        g_target->Release();
        g_target = nullptr;
    }
}

bool createGraphics(HWND window) {
    DXGI_SWAP_CHAIN_DESC description = {};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL wanted[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2, D3D11_SDK_VERSION,
                                                   &description, &g_swapChain, &g_device, &got, &g_context);
    if (FAILED(result)) {
        // No usable graphics card driver: draw in software.
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, wanted, 2, D3D11_SDK_VERSION,
                                               &description, &g_swapChain, &g_device, &got, &g_context);
    }
    if (FAILED(result)) return false;
    createTarget();
    return true;
}

void destroyGraphics() {
    releaseTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

// --- window and notification area ------------------------------------------

Controller* g_controller = nullptr;
NOTIFYICONDATAW g_trayIcon = {};
UINT g_taskbarCreated = 0;
bool g_selfTesting = false;  // the self-test provides the input: the real mouse and keyboard are ignored

bool isUserInput(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           message == WM_MOUSELEAVE || message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE ||
           message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_INPUT;
}

void showWindow(HWND window) {
    ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window);
}

void showTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Wolfe");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(window);  // otherwise the menu does not close when clicking elsewhere
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (chosen == kMenuOpen) showWindow(window);
    if (chosen == kMenuExit) DestroyWindow(window);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const bool ignored = g_selfTesting && isUserInput(message);
    if (!ignored && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return 1;

    if (message == g_taskbarCreated && g_taskbarCreated != 0) {
        Shell_NotifyIconW(NIM_ADD, &g_trayIcon);  // Explorer restarted and lost the icon
        return 0;
    }
    switch (message) {
        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED) {
                releaseTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                createTarget();
            }
            return 0;
        case WM_CLOSE:
            // The audio must keep flowing: closing only hides, unless the user turned that off.
            if (g_controller && g_controller->closeToTray()) ShowWindow(window, SW_HIDE);
            else DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_ENDSESSION:
            // Windows is shutting down and may not leave time for the ordinary
            // way out. Applications kept off every device stay that way only
            // if Wolfe will be there again at the next start to carry them.
            if (wParam && g_controller) {
                g_controller->save();
                if (!startsWithWindows()) g_controller->giveBackParked();
            }
            return 0;
        case kShowMessage:
            showWindow(window);
            return 0;
        case kQuitMessage:
            DestroyWindow(window);
            return 0;
        case kTrayMessage:
            if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) showWindow(window);
            else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) showTrayMenu(window);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    bool startHidden = false;
    std::string document = defaultDocumentPath();
    std::string importPath, selfTestReport;
    const std::vector<std::string> arguments = commandLine();
    for (size_t i = 0; i < arguments.size(); i++) {
        const std::string& argument = arguments[i];
        const bool hasValue = i + 1 < arguments.size();
        if (argument == "--tray") {
            startHidden = true;
        } else if (argument == "--file" && hasValue) {
            document = arguments[++i];
        } else if (argument == "--import" && hasValue) {
            importPath = arguments[++i];
        } else if (argument == "--selftest" && hasValue) {
            selfTestReport = arguments[++i];
        } else if (argument.size() > 5 && toLower(argument.substr(argument.size() - 5)) == ".json") {
            // A graph file given just like that, or the way the console
            // program takes it ("run file.json"), is one to import.
            importPath = argument;
        }
    }
    g_selfTesting = !selfTestReport.empty();

    // Two copies on the same graph would both feed the same devices. A second
    // one only brings the first to the front. (A copy started on a graph file
    // of its own, with --file, is a deliberate exception.)
    HANDLE mutex = nullptr;
    if (document == defaultDocumentPath()) {
        mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, kShowMessage, 0, 0);
            return 0;
        }
    }

    // There is no console: what the engine reports goes to a file.
    _wfreopen(toWide(logFilePath()).c_str(), L"w", stderr);

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = makeIcon(GetSystemMetrics(SM_CXICON));
    windowClass.hIconSm = makeIcon(GetSystemMetrics(SM_CXSMICON));
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    // Wide enough for the five columns boxes are first laid out in, if the screen allows.
    RECT workArea = {0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = std::min((int)(1480 * scale), (int)((workArea.right - workArea.left) * 0.92));
    const int height = std::min((int)(860 * scale), (int)((workArea.bottom - workArea.top) * 0.92));
    HWND window = CreateWindowExW(0, kWindowClass, L"Wolfe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width,
                                  height, nullptr, nullptr, instance, nullptr);
    if (!window || !createGraphics(window)) {
        MessageBoxW(nullptr, L"Wolfe could not open its window (Direct3D 11 is not available).", L"Wolfe", MB_ICONERROR);
        return 1;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = window;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = windowClass.hIconSm;
    lstrcpynW(g_trayIcon.szTip, L"Wolfe", ARRAYSIZE(g_trayIcon.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImNodes::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing of the layout is worth a file of its own
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // arrow keys in menus, Escape closes them
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 16.0f;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf")) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);

    int exitCode = 0;
    {
        ComApartment com;
        Controller controller(document);
        g_controller = &controller;
        Editor editor(controller, scale);
        editor.window = window;
        std::string problems = controller.load();
        if (!importPath.empty()) problems = controller.importFrom(importPath);
        if (!problems.empty()) editor.notify(problems);

        std::unique_ptr<SelfTest> selfTest;
        if (g_selfTesting) {
            selfTest = std::make_unique<SelfTest>(controller, selfTestReport);
            ShowWindow(window, SW_SHOWNOACTIVATE);  // without taking the keyboard away from the user
        } else if (!startHidden) {
            ShowWindow(window, SW_SHOWDEFAULT);
            UpdateWindow(window);
        }

        bool running = true;
        while (running) {
            // Nothing to draw while hidden. While shown and left alone, 20
            // frames a second are enough for the level meters; any input
            // wakes this up at once.
            const bool shown = IsWindowVisible(window) && !IsIconic(window);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, selfTest ? 1 : shown ? 50 : 500, QS_ALLINPUT);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    running = false;
                    exitCode = (int)message.wParam;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running) break;

            const double now = nowSeconds();
            controller.setWatched(IsWindowVisible(window) && !IsIconic(window));
            controller.update(now);
            if (!IsWindowVisible(window) || IsIconic(window) || !g_target) continue;

            if (selfTest && !selfTest->step()) {
                DestroyWindow(window);
                continue;
            }
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            editor.draw(now);
            ImGui::Render();
            const float background[4] = {0.09f, 0.10f, 0.12f, 1.0f};
            g_context->OMSetRenderTargets(1, &g_target, nullptr);
            g_context->ClearRenderTargetView(g_target, background);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_swapChain->Present(1, 0);

            if (editor.exitRequested) DestroyWindow(window);
        }
        g_controller = nullptr;
    }  // the controller saves the graph and stops the engine here

    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImNodes::DestroyContext();
    ImGui::DestroyContext();
    destroyGraphics();
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
