#include "display.h"

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <limits>
#include <sstream>

#include "util.h"

namespace {

bool isVirtualAdapter(const DISPLAY_DEVICEW& adapter) {
    return wcsstr(adapter.DeviceString, L"Virtual Display Driver") != nullptr || wcsstr(adapter.DeviceID, L"MttVDD") != nullptr;
}

// Where to put the display the first time: to the right of everything else.
POINTL defaultPosition() {
    POINTL position{0, 0};
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) || isVirtualAdapter(device)) continue;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
            position.x = std::max<LONG>(position.x, mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth));
        }
    }
    return position;
}

// The driver reads its modes from this fixed location, and any program may ask it to read them again.
constexpr wchar_t kDriverSettings[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
constexpr wchar_t kDriverPipe[] = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
constexpr int kDriverReloadWaitMs = 15000;

bool insertBefore(std::string& text, const std::string& closingTag, const std::string& addition) {
    const size_t at = text.find(closingTag);
    if (at == std::string::npos) return false;
    text.insert(at, addition);
    return true;
}

bool addModeToSettings(int width, int height, int hz) {
    std::string text;
    {
        std::ifstream in(kDriverSettings, std::ios::binary);
        std::stringstream contents;
        contents << in.rdbuf();
        text = contents.str();
    }
    const std::string rate = "<g_refresh_rate>" + std::to_string(hz) + "</g_refresh_rate>";
    if (text.find(rate) == std::string::npos && !insertBefore(text, "</global>", "    " + rate + "\n    ")) return false;
    const std::string size = "<width>" + std::to_string(width) + "</width>\n            <height>" + std::to_string(height) + "</height>";
    if (text.find(size) == std::string::npos) {
        const std::string mode = "    <resolution>\n            " + size + "\n            <refresh_rate>60</refresh_rate>\n        </resolution>\n    ";
        if (!insertBefore(text, "</resolutions>", mode)) return false;
    }
    std::ofstream out(kDriverSettings, std::ios::binary | std::ios::trunc);
    out << text;
    return out.good();
}

bool askDriverToReload() {
    HANDLE pipe = CreateFileW(kDriverPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    constexpr wchar_t command[] = L"RELOAD_DRIVER";
    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, command, sizeof(command) - sizeof(wchar_t), &written, nullptr);
    CloseHandle(pipe);
    return ok != FALSE;
}

const char* changeError(LONG code) {
    switch (code) {
        case DISP_CHANGE_SUCCESSFUL: return "ok";
        case DISP_CHANGE_RESTART: return "restart required";
        case DISP_CHANGE_FAILED: return "driver refused the mode";
        case DISP_CHANGE_BADMODE: return "mode not supported";
        case DISP_CHANGE_NOTUPDATED: return "could not save settings";
        case DISP_CHANGE_BADFLAGS: return "bad flags";
        case DISP_CHANGE_BADPARAM: return "bad parameter";
        case DISP_CHANGE_BADDUALVIEW: return "bad dualview";
        default: return "unknown error";
    }
}

}  // namespace

bool VirtualDisplay::find() {
    gdiName_.clear();
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    std::wstring fallback;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (!isVirtualAdapter(device)) continue;
        if (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            gdiName_ = device.DeviceName;
            return true;
        }
        if (fallback.empty()) fallback = device.DeviceName;
    }
    gdiName_ = fallback;
    return !gdiName_.empty();
}

bool VirtualDisplay::activate(int width, int height, int hz, int* actualWidth, int* actualHeight, int* actualHz) {
    if (gdiName_.empty()) return false;

    // Choose the closest mode the driver offers: the exact resolution if possible, then
    // the requested refresh rate or the nearest one below it.
    DEVMODEW best{};
    long bestScore = std::numeric_limits<long>::max();
    DEVMODEW candidate{};
    candidate.dmSize = sizeof(candidate);
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName_.c_str(), i, &candidate); ++i) {
        if (candidate.dmBitsPerPel != 32) continue;
        const long resolution = labs(static_cast<long>(candidate.dmPelsWidth) - width) + labs(static_cast<long>(candidate.dmPelsHeight) - height);
        const long hzGap = static_cast<long>(candidate.dmDisplayFrequency) - hz;
        const long score = resolution * 1000 + (hzGap > 0 ? hzGap * 2 + 1 : -hzGap * 2);
        if (score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    if (best.dmPelsWidth == 0) {
        logLine("display: %s offers no modes", narrow(gdiName_).c_str());
        return false;
    }

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    const bool attached = EnumDisplaySettingsExW(gdiName_.c_str(), ENUM_CURRENT_SETTINGS, &current, 0) && current.dmPelsWidth != 0;
    if (attached && current.dmPelsWidth == best.dmPelsWidth && current.dmPelsHeight == best.dmPelsHeight &&
        current.dmDisplayFrequency == best.dmDisplayFrequency) {
        *actualWidth = current.dmPelsWidth;
        *actualHeight = current.dmPelsHeight;
        *actualHz = current.dmDisplayFrequency;
        return true;
    }

    DEVMODEW change = best;
    change.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    if (!attached) {
        DEVMODEW saved{};
        saved.dmSize = sizeof(saved);
        // Prefer where the user last placed it; Windows keeps that across detaches.
        if (EnumDisplaySettingsExW(gdiName_.c_str(), ENUM_REGISTRY_SETTINGS, &saved, 0) && saved.dmPelsWidth != 0 &&
            (saved.dmFields & DM_POSITION)) {
            change.dmPosition = saved.dmPosition;
        } else {
            change.dmPosition = defaultPosition();
        }
        change.dmFields |= DM_POSITION;
    }
    LONG result = ChangeDisplaySettingsExW(gdiName_.c_str(), &change, nullptr, CDS_UPDATEREGISTRY | CDS_GLOBAL | CDS_NORESET, nullptr);
    if (result == DISP_CHANGE_SUCCESSFUL) result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        // HKLM-scoped settings need elevation; the per-user scope is enough for this to work.
        result = ChangeDisplaySettingsExW(gdiName_.c_str(), &change, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (result == DISP_CHANGE_SUCCESSFUL) result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    }
    if (result != DISP_CHANGE_SUCCESSFUL) {
        logLine("display: could not set %lux%lu@%lu: %s", best.dmPelsWidth, best.dmPelsHeight, best.dmDisplayFrequency, changeError(result));
        return false;
    }

    // Windows can renumber displays when one is attached.
    find();
    current = DEVMODEW{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsExW(gdiName_.c_str(), ENUM_CURRENT_SETTINGS, &current, 0)) return false;
    logLine("display: %s is now %lux%lu@%lu at (%ld,%ld)", narrow(gdiName_).c_str(), current.dmPelsWidth, current.dmPelsHeight,
            current.dmDisplayFrequency, current.dmPosition.x, current.dmPosition.y);
    *actualWidth = current.dmPelsWidth;
    *actualHeight = current.dmPelsHeight;
    *actualHz = current.dmDisplayFrequency;
    return true;
}

bool VirtualDisplay::offersMode(int width, int height, int hz) const {
    DEVMODEW candidate{};
    candidate.dmSize = sizeof(candidate);
    for (DWORD i = 0; EnumDisplaySettingsW(gdiName_.c_str(), i, &candidate); ++i) {
        if (candidate.dmBitsPerPel == 32 && static_cast<int>(candidate.dmPelsWidth) == width && static_cast<int>(candidate.dmPelsHeight) == height &&
            static_cast<int>(candidate.dmDisplayFrequency) == hz) {
            return true;
        }
    }
    return false;
}

bool VirtualDisplay::addMode(int width, int height, int hz) {
    if (!addModeToSettings(width, height, hz)) {
        logLine("display: could not update the driver's settings");
        return false;
    }
    if (!askDriverToReload()) {
        logLine("display: could not reach the driver to reload its settings (error %lu)", GetLastError());
        return false;
    }
    const uint64_t deadline = nowUs() + kDriverReloadWaitMs * 1000ull;
    while (nowUs() < deadline) {
        Sleep(100);
        if (find() && offersMode(width, height, hz)) {
            logLine("display: the driver now offers %dx%d@%d", width, height, hz);
            return true;
        }
    }
    find();
    logLine("display: the driver didn't offer %dx%d@%d after reloading", width, height, hz);
    return false;
}

bool VirtualDisplay::isActive() const {
    if (gdiName_.empty()) return false;
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    return EnumDisplaySettingsExW(gdiName_.c_str(), ENUM_CURRENT_SETTINGS, &current, 0) && current.dmPelsWidth != 0;
}

void VirtualDisplay::deactivate() {
    if (gdiName_.empty()) return;
    DEVMODEW off{};
    off.dmSize = sizeof(off);
    off.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG result = ChangeDisplaySettingsExW(gdiName_.c_str(), &off, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (result == DISP_CHANGE_SUCCESSFUL) result = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        logLine("display: could not remove %s: %s", narrow(gdiName_).c_str(), changeError(result));
    } else {
        logLine("display: %s removed from the desktop", narrow(gdiName_).c_str());
    }
}
