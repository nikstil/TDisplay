// Turns two fingers on the tablet into two fingers on a Windows touchscreen over the virtual display. Only exactly two
// fingers make gestures: one finger, a palm, or any finger while the pen is near does nothing, and three or more only count as
// a quick tap.
//
// Like the Mac version, it first works out what the fingers are doing: moving together (pan), apart or together
// (zoom) or twisting (rotate). The first of those past its threshold owns the gesture until the fingers lift, and
// Windows is sent a clean version of only that movement. A twist arrives with the fingers exactly as far apart as they
// started, so programs like Photoshop that favour zooming have nothing to zoom by. Until a gesture is decided nothing
// reaches Windows, which also lets a quick two-finger tap become undo instead of a tap. A quick three-finger tap is redo.
#pragma once
#include <windows.h>

#include <cstdint>

#include "protocol.h"

// The tray menu's switch. On unless turned off, remembered for this Windows user.
bool touchGesturesEnabled();
void setTouchGesturesEnabled(bool enabled);

class TouchInjector {
public:
    TouchInjector();
    ~TouchInjector();
    TouchInjector(const TouchInjector&) = delete;
    TouchInjector& operator=(const TouchInjector&) = delete;

    // A touch message's payload. rect is where the tablet's screen sits, as for the pen. allowed is false while the
    // pen is near or gestures are switched off; fingers already down are then let go as canceled, not as a tap.
    void apply(const uint8_t* payload, uint32_t size, const RECT& rect, bool allowed, uint64_t nowUs);

    // Call often. Windows cancels touches that get no input for a while, and Android sends nothing for fingers
    // that are down but still, so this repeats where they are.
    void tick(uint64_t nowUs);
    int msUntilTick(uint64_t nowUs) const;

    // Lifts any fingers, as canceled. Used when the connection ends.
    void release();

    uint64_t gestures() const { return gestures_; }
    uint64_t taps() const { return taps_; }

private:
    enum class Mode { Idle, Undecided, Pan, Zoom, Rotate };
    struct Point {
        double x, y;
    };

    bool inject(UINT32 flags);
    void lift(bool canceled);
    void endTouch();
    // Called as fingers lift: sends undo if two fingers just tapped.
    void checkTwoFingerTap(uint32_t count, uint64_t nowUs);
    void shape();  // Sets points_ from where the fingers are, keeping only the decided movement.

    void* device_ = nullptr;
    Mode mode_ = Mode::Idle;
    bool down_ = false;  // Windows has the fingers.
    uint32_t ids_[2] = {};
    Point start_[2] = {};
    Point now_[2] = {};
    bool tapSpoiled_ = false;  // The fingers moved too far, or stayed too long, to be a tap.
    uint64_t startUs_ = 0;
    // The whole touch, from the first finger down to the last up, for telling a three-finger tap.
    bool touching_ = false;
    uint64_t touchStartUs_ = 0;
    uint32_t mostFingers_ = 0;
    bool fingersMoved_ = false;
    bool threeTapChecked_ = false;
    uint32_t seenIds_[proto::kTouchMaxFingers] = {};
    Point seenAt_[proto::kTouchMaxFingers] = {};
    uint32_t seen_ = 0;
    RECT rect_{};
    POINT points_[2] = {};
    uint64_t lastInjectUs_ = 0;
    uint64_t gestures_ = 0;
    uint64_t taps_ = 0;
    bool loggedFailure_ = false;
};
