#include "shell.h"

#include "common.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace wolfe {

namespace {

const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValue = L"Wolfe";

std::wstring dataFolder() {
    wchar_t appData[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::filesystem::path folder = std::filesystem::path(appData) / L"Wolfe";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    return folder.wstring();
}

std::wstring startCommand() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\" --tray";
}

}  // namespace

std::string defaultDocumentPath() { return toUtf8(dataFolder() + L"\\graph.json"); }
std::string logFilePath() { return toUtf8(dataFolder() + L"\\wolfe.log"); }

bool startsWithWindows() {
    wchar_t value[1024];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
        return false;
    }
    // An entry left behind by a copy of the program somewhere else does not count.
    return startCommand() == value;
}

void setStartsWithWindows(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        const std::wstring command = startCommand();
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)command.c_str(),
                       (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// A dark rounded square with a zigzag of connected dots: a "W" made of a patch.
HICON makeIcon(int size) {
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;  // top row first
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&header, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    struct Point {
        float x, y;
    };
    const Point points[5] = {{0.20f, 0.30f}, {0.35f, 0.72f}, {0.50f, 0.42f}, {0.65f, 0.72f}, {0.80f, 0.30f}};
    // How much of a pixel a shape covers, from its distance to the shape's edge (in pixels).
    auto coverage = [](float distanceInside) { return std::max(0.0f, std::min(1.0f, distanceInside + 0.5f)); };
    auto distanceToSegment = [](float px, float py, Point a, Point b) {
        float dx = b.x - a.x, dy = b.y - a.y;
        float t = ((px - a.x) * dx + (py - a.y) * dy) / (dx * dx + dy * dy);
        t = std::max(0.0f, std::min(1.0f, t));
        return std::hypot(px - (a.x + t * dx), py - (a.y + t * dy));
    };

    uint32_t* pixels = (uint32_t*)bits;
    for (int row = 0; row < size; row++) {
        for (int column = 0; column < size; column++) {
            const float x = (column + 0.5f) / size, y = (row + 0.5f) / size;

            // Background: a rounded square.
            const float half = 0.47f, radius = 0.20f;
            const float qx = std::fabs(x - 0.5f) - (half - radius), qy = std::fabs(y - 0.5f) - (half - radius);
            const float outside = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
            float alpha = coverage(-outside * size);
            float red = 0x20, green = 0x24, blue = 0x2E;

            // The zigzag, then a dot on each corner of it.
            float nearestLine = 1e9f, nearestDot = 1e9f;
            for (int i = 0; i < 4; i++) nearestLine = std::min(nearestLine, distanceToSegment(x, y, points[i], points[i + 1]));
            for (const Point& point : points) nearestDot = std::min(nearestDot, std::hypot(x - point.x, y - point.y));
            const float line = coverage((0.035f - nearestLine) * size);
            red += (0x6C - red) * line;
            green += (0xB6 - green) * line;
            blue += (0xFF - blue) * line;
            const float dot = coverage((0.085f - nearestDot) * size);
            red += (0xF2 - red) * dot;
            green += (0xF5 - green) * dot;
            blue += (0xFA - blue) * dot;

            pixels[row * size + column] = ((uint32_t)(alpha * 255.0f + 0.5f) << 24) | ((uint32_t)red << 16) |
                                          ((uint32_t)green << 8) | (uint32_t)blue;
        }
    }

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace wolfe
