// Finds the virtual monitor created by the display driver and sets its mode.
#pragma once
#include <windows.h>

#include <string>

class VirtualDisplay {
public:
    // Looks the driver's display up. False if the driver isn't installed or is disabled.
    bool find();

    // Adds the display to the desktop if needed and sets its mode as close to the request as the
    // driver allows. Returns the mode that took effect.
    bool activate(int width, int height, int hz, int* actualWidth, int* actualHeight, int* actualHz);

    // True if the driver offers exactly this size and refresh rate.
    bool offersMode(int width, int height, int hz) const;

    // Tablets come in many sizes, so this adds one the driver doesn't offer yet to its settings and has it
    // reload them. The display goes away for a moment while that happens.
    bool addMode(int width, int height, int hz);

    // Takes the display off the desktop, which moves its windows back to the real screens.
    void deactivate();

    // True if the display is currently part of the desktop.
    bool isActive() const;

    const std::wstring& gdiName() const { return gdiName_; }

private:
    std::wstring gdiName_;
};
