// One connected session with the tablet: capture, encode and send on this thread,
// and receive pen input on a second, higher-priority one.
#pragma once
#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "adb.h"
#include "display.h"
#include "encoder.h"
#include "protocol.h"

struct StreamSettings {
    int maxRefreshHz = 240;
    int streamFps = 0;  // Frames per second to send. 0 means the same as the refresh rate.
    int bitrate = 80000000;
    EncoderKind encoder = EncoderKind::Auto;
    CodecChoice codec = CodecChoice::Auto;
    int preset = 1;
    int minQp = 22;
    int maxFramesInFlight = 0;  // 0 chooses from the refresh rate.
    int penCurvePercent = 50;   // Pressure curve exponent, in hundredths. 100 is a straight line.
    int penMinimumPercent = 6;  // What the lightest pen contact registers as, in percent.
};

enum class StreamEnd {
    ConnectionLost,  // The cable, adb or the app went away unexpectedly.
    AppHidden,       // The user left the app on the tablet or turned its screen off.
    Stopped,         // The laptop program is quitting.
    NotReady,        // The tablet app was closing or starting as we connected. Just try again.
    SetupFailed,     // Could not start; the log says why.
};

// Runs until the session ends. Takes ownership of the socket.
// onStreaming is called with a status line once frames are flowing, and when it changes.
StreamEnd runStream(SOCKET socket, Adb& adb, const std::string& serial, VirtualDisplay& display, const StreamSettings& settings, std::atomic<bool>& stop,
                    const std::function<void(const std::wstring&)>& onStreaming, std::wstring* problem);
