#pragma once

#include <windows.h>

#include <string>

namespace wolfe {

// Where the editor keeps its graph: %APPDATA%\Wolfe\graph.json
std::string defaultDocumentPath();
std::string logFilePath();

// "Start with Windows": an entry under the current user's Run key that starts
// this program hidden in the tray.
bool startsWithWindows();
void setStartsWithWindows(bool enabled);

// Windows' settings page where each application is given its devices.
void openVolumeMixer();

// The standard "open file" window; empty when the user cancels.
std::string askForGraphFile(HWND owner);

// The program's icon, drawn in code at the size asked for.
HICON makeIcon(int size);

}  // namespace wolfe
