#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace wolfe {

// Where the editor keeps its graph: %APPDATA%\Wolfe\graph.json
std::string defaultDocumentPath();
std::string logFilePath();

// "Start with Windows": an entry under the current user's Run key that starts
// this program hidden in the tray.
bool startsWithWindows();
void setStartsWithWindows(bool enabled);

// Windows' Open or Save As window, for a graph file (.json). The chosen path,
// or empty if the user cancelled.
std::string chooseGraphFile(HWND owner, bool saving);

// The program's icon, drawn in code at the size asked for: as an icon, or as
// rows of pixels from the top, 0xAARRGGBB (alpha not premultiplied).
HICON makeIcon(int size);
std::vector<uint32_t> iconPixels(int size);

}  // namespace wolfe
