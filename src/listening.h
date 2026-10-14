#pragma once

#include "com.h"

#include <string>
#include <vector>

namespace wolfe {

// Windows can play a recording device on a playback device by itself: "Listen
// to this device", in the properties of the recording device. It is a
// connection like any other, made by Windows: it works whether or not Wolfe
// is running, a recording device has at most one, and it has no gain.
struct Listening {
    std::wstring input;   // the recording device's id
    std::wstring output;  // the playback device's id; empty: whichever is the default one
};

// The ones that are switched on. Needs COM on the calling thread.
std::vector<Listening> listListening();

// Switches it on for `inputId` towards `outputId`, moving it if it was on
// towards another device, or switches it off (`outputId` is then ignored).
// Windows acts on it at once. False if Windows refused, or does not answer
// the way this was written for. Needs COM on the calling thread.
bool setListening(const std::wstring& inputId, bool on, const std::wstring& outputId = {});

}  // namespace wolfe
