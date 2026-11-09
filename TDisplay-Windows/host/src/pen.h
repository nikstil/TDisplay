// Turns pen samples from the tablet into Windows Ink pen input on the virtual display.
#pragma once
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "protocol.h"

class PenInjector {
public:
    PenInjector();
    ~PenInjector();
    PenInjector(const PenInjector&) = delete;
    PenInjector& operator=(const PenInjector&) = delete;

    bool ready() const { return device_ != nullptr; }

    // The display that pen coordinates map onto (\\.\DISPLAYn).
    void setTarget(const std::wstring& gdiName);

    // Picks up changes to where that display sits. This asks Windows about the display setup, which
    // can block, so call it from anywhere but the thread that's handling pen samples.
    void refreshRect();

    // How the pen's pressure maps onto what Windows gets: output = minimum + (1 - minimum) * input^exponent.
    // An exponent below 1 lifts light strokes, and the minimum is what the lightest contact registers as.
    void setPressureCurve(double exponent, double minimum);

    void apply(const proto::PenSample& sample, uint64_t nowUs);

    // Call often. Windows cancels a held pointer that gets no input for a while, so
    // this repeats the last state, and it finishes any pending pen-left-the-screen.
    void tick(uint64_t nowUs);

    // Time until tick() next has work, for the caller's poll timeout.
    int msUntilTick(uint64_t nowUs) const;

    // The pen is touching or hovering. Input thread only.
    bool penNear() const { return inRange_ || contact_; }

    // Where the display sits, measured as injected positions are.
    RECT rect() {
        std::lock_guard<std::mutex> lock(rectMutex_);
        return rect_;
    }

    // Lifts the pen. Used when the connection ends so nothing stays pressed.
    void release();

private:
    bool inject(UINT32 flags);

    void* device_ = nullptr;
    std::wstring target_;
    double pressureExponent_ = 0.5;
    double pressureMinimum_ = 0.06;
    std::mutex rectMutex_;
    RECT rect_{};

    bool inRange_ = false;
    bool contact_ = false;
    uint32_t tool_ = 0;  // kPenEraser while the pen is flipped to its eraser.
    uint32_t penFlags_ = 0;
    uint32_t pressure_ = 0;
    int tiltX_ = 0;
    int tiltY_ = 0;
    POINT point_{};
    uint64_t lastInjectUs_ = 0;
    uint64_t lastSampleUs_ = 0;
    uint64_t leaveAtUs_ = 0;  // Nonzero while a pen-left event is waiting to see if contact follows.
    bool loggedFailure_ = false;
};
