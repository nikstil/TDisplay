// TDisplay laptop program: a tray icon around the background service.
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <mutex>
#include <string>

#include "service.h"
#include "touch.h"
#include "util.h"

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kMenuLogs = 1;
constexpr UINT kMenuStartup = 2;
constexpr UINT kMenuQuit = 3;
constexpr UINT kMenuHelp = 4;
constexpr UINT kMenuGestures = 5;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAppName[] = L"TDisplay";
constexpr wchar_t kSettingsKey[] = L"Software\\TDisplay";
constexpr wchar_t kHelpFile[] = L"\\Getting started.txt";

std::mutex gStatusMutex;
std::wstring gStatusText = L"Starting";
LinkState gStatusState = LinkState::Waiting;
NOTIFYICONDATAW gTray{};
Service* gService = nullptr;

HICON makeIcon(LinkState state) {
    const COLORREF colors[] = {RGB(150, 150, 150), RGB(52, 199, 89), RGB(255, 159, 10)};
    const COLORREF fill = colors[static_cast<int>(state)];
    constexpr int size = 32;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ previous = SelectObject(dc, color);

    RECT all{0, 0, size, size};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &all, black);
    DeleteObject(black);
    // A small tablet: a filled rounded outline with a dark screen.
    HBRUSH body = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    SelectObject(dc, body);
    SelectObject(dc, pen);
    RoundRect(dc, 2, 6, 30, 26, 6, 6);
    HBRUSH screenBrush = CreateSolidBrush(RGB(20, 20, 20));
    RECT panel{5, 9, 27, 23};
    FillRect(dc, &panel, screenBrush);
    DeleteObject(screenBrush);
    SelectObject(dc, previous);
    DeleteObject(body);
    DeleteObject(pen);

    HDC maskDc = CreateCompatibleDC(screen);
    HGDIOBJ previousMask = SelectObject(maskDc, mask);
    RECT maskAll{0, 0, size, size};
    FillRect(maskDc, &maskAll, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(maskDc, GetStockObject(BLACK_BRUSH));
    SelectObject(maskDc, GetStockObject(BLACK_PEN));
    RoundRect(maskDc, 2, 6, 30, 26, 6, 6);
    SelectObject(maskDc, previousMask);
    DeleteDC(maskDc);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void updateTray(DWORD action) {
    std::wstring text;
    LinkState state;
    {
        std::lock_guard<std::mutex> lock(gStatusMutex);
        text = gStatusText;
        state = gStatusState;
    }
    static HICON icon = nullptr;
    HICON next = makeIcon(state);
    gTray.hIcon = next;
    std::wstring tip = std::wstring(kAppName) + L": " + text;
    wcsncpy_s(gTray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(action, &gTray);
    if (icon) DestroyIcon(icon);
    icon = next;
}

bool startsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void setStartsWithWindows(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path, n) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()), static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// The copy setup installed starts with Windows, because the first thing it's needed for after a reboot is
// keeping the virtual monitor away while no tablet is connected. The menu can still turn that off for good.
void startWithWindowsOnFirstRun() {
    if (GetFileAttributesW((exeDirectory() + L"\\uninstall.exe").c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DWORD chosen = 0;
    DWORD size = sizeof(chosen);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"StartupChosen", RRF_RT_REG_DWORD, nullptr, &chosen, &size) == ERROR_SUCCESS) return;
    setStartsWithWindows(true);
    chosen = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"StartupChosen", REG_DWORD, &chosen, sizeof(chosen));
}

void showMenu(HWND window) {
    std::wstring text;
    {
        std::lock_guard<std::mutex> lock(gStatusMutex);
        text = gStatusText;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (startsWithWindows() ? MF_CHECKED : 0), kMenuStartup, L"Start with Windows");
    AppendMenuW(menu, MF_STRING | (touchGesturesEnabled() ? MF_CHECKED : 0), kMenuGestures, L"Two-finger gestures");
    const std::wstring help = exeDirectory() + kHelpFile;
    if (GetFileAttributesW(help.c_str()) != INVALID_FILE_ATTRIBUTES) AppendMenuW(menu, MF_STRING, kMenuHelp, L"How to connect a tablet");
    AppendMenuW(menu, MF_STRING, kMenuLogs, L"Open log folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit TDisplay");
    POINT at;
    GetCursorPos(&at);
    SetForegroundWindow(window);
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, 0, window, nullptr);
    DestroyMenu(menu);
    switch (choice) {
        case kMenuStartup: setStartsWithWindows(!startsWithWindows()); break;
        case kMenuGestures: setTouchGesturesEnabled(!touchGesturesEnabled()); break;
        case kMenuHelp: ShellExecuteW(nullptr, L"open", help.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kMenuLogs: ShellExecuteW(nullptr, L"open", logDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kMenuQuit: PostQuitMessage(0); break;
        default: break;
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == kTrayMessage) {
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP) showMenu(window);
        return 0;
    }
    if (message == kStatusMessage) {
        updateTray(NIM_MODIFY);
        return 0;
    }
    if (message == taskbarCreated) {
        updateTray(NIM_ADD);
        return 0;
    }
    if (message == WM_CLOSE) {
        // Setup asks a running copy to close before replacing it.
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_ENDSESSION && wparam) {
        if (gService) gService->stop();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\TDisplayHost");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    logInit();
    logLine("==== TDisplay host starting ====");
    startWithWindowsOnFirstRun();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    timeBeginPeriod(1);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"TDisplayHostWindow";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = window;
    gTray.uID = 1;
    gTray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    gTray.uCallbackMessage = kTrayMessage;
    updateTray(NIM_ADD);

    Service service([window](LinkState state, const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lock(gStatusMutex);
            gStatusText = text;
            gStatusState = state;
        }
        PostMessageW(window, kStatusMessage, 0, 0);
    });
    gService = &service;
    service.start();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    service.stop();
    gService = nullptr;
    Shell_NotifyIconW(NIM_DELETE, &gTray);
    timeEndPeriod(1);
    WSACleanup();
    logLine("==== TDisplay host stopped ====");
    CloseHandle(single);
    return 0;
}
