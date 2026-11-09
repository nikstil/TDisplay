#include "touch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>

#include "util.h"

namespace {

// Windows cancels a touch that stops receiving input; this keeps it alive well before that.
constexpr uint64_t kRepeatUs = 40000;
constexpr wchar_t kSettingsKey[] = L"Software\\TDisplay";
constexpr wchar_t kGesturesValue[] = L"TouchGestures";

// How far, in the tablet's pixels (about 11 to a millimeter), the fingers must move together, apart or past each other
// before that becomes the gesture. Twisting also has to turn far enough, since pinches rarely keep their angle exactly.
constexpr double kPanStartPx = 30;
constexpr double kZoomStartPx = 35;
constexpr double kRotateStartPx = 40;
constexpr double kRotateMinDegrees = 10;
// A two-finger tap: both down and up again within this time, moving no further than this.
constexpr uint64_t kTapUs = 300000;
constexpr double kTapSlopPx = 30;
// A three-finger tap: from the first finger down to the first one up again. Fingers rarely land together, so it's longer.
constexpr uint64_t kThreeTapUs = 450000;

constexpr double kPi = 3.14159265358979323846;

using CreateDeviceFn = HSYNTHETICPOINTERDEVICE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
using InjectFn = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
using DestroyFn = VOID(WINAPI*)(HSYNTHETICPOINTERDEVICE);

FARPROC user32(const char* name) {
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}

// -1 until read from the registry.
std::atomic<int> gEnabled{-1};

double degrees(double radians) {
    double d = radians * 180 / kPi;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// Ctrl+Z to whatever has the keyboard, which is the program the pen last worked in.
void sendUndo() {
    INPUT keys[4]{};
    for (INPUT& key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_CONTROL;
    keys[1].ki.wVk = 'Z';
    keys[2].ki.wVk = 'Z';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_CONTROL;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, keys, sizeof(INPUT));
}

// The name of the program in front, such as "photoshop.exe", in lower case.
std::wstring foregroundProgram() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    std::wstring name;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            name = path;
            name = name.substr(name.find_last_of(L'\\') + 1);
            for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
        }
        CloseHandle(process);
    }
    return name;
}

// Redo is Ctrl+Shift+Z in Photoshop, Krita and most programs, but Ctrl+Y in these. Ctrl+Y in Photoshop toggles proof
// colours instead, so it isn't a safe default.
bool redoIsCtrlY(const std::wstring& program) {
    static const wchar_t* const kCtrlY[] = {L"clipstudiopaint.exe", L"mspaint.exe", L"paintdotnet.exe", L"winword.exe", L"excel.exe",
                                            L"powerpnt.exe", L"onenote.exe", L"notepad.exe", L"sai.exe", L"sai2.exe"};
    for (const wchar_t* name : kCtrlY) {
        if (program == name) return true;
    }
    return false;
}

void sendRedo() {
    const std::wstring program = foregroundProgram();
    const bool ctrlY = redoIsCtrlY(program);
    logLine("touch: three-finger tap, sending redo (%s) to %s", ctrlY ? "Ctrl+Y" : "Ctrl+Shift+Z", narrow(program).c_str());
    INPUT keys[6]{};
    int n = 0;
    const auto key = [&](WORD vk, bool up) {
        keys[n].type = INPUT_KEYBOARD;
        keys[n].ki.wVk = vk;
        keys[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        n++;
    };
    key(VK_CONTROL, false);
    if (!ctrlY) key(VK_SHIFT, false);
    key(ctrlY ? 'Y' : 'Z', false);
    key(ctrlY ? 'Y' : 'Z', true);
    if (!ctrlY) key(VK_SHIFT, true);
    key(VK_CONTROL, true);
    SendInput(n, keys, sizeof(INPUT));
}

}  // namespace

bool touchGesturesEnabled() {
    int enabled = gEnabled.load();
    if (enabled < 0) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kGesturesValue, RRF_RT_REG_DWORD, nullptr, &value, &size);
        enabled = value ? 1 : 0;
        gEnabled = enabled;
    }
    return enabled != 0;
}

void setTouchGesturesEnabled(bool enabled) {
    const DWORD value = enabled ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kGesturesValue, REG_DWORD, &value, sizeof(value));
    gEnabled = enabled ? 1 : 0;
    logLine("touch: gestures %s", enabled ? "on" : "off");
}

TouchInjector::TouchInjector() {
    if (auto create = reinterpret_cast<CreateDeviceFn>(user32("CreateSyntheticPointerDevice"))) {
        device_ = create(PT_TOUCH, 2, POINTER_FEEDBACK_DEFAULT);
    }
    if (!device_) logLine("touch: could not create a touch device (error %lu)", GetLastError());
}

TouchInjector::~TouchInjector() {
    release();
    if (device_) {
        if (auto destroy = reinterpret_cast<DestroyFn>(user32("DestroySyntheticPointerDevice"))) destroy(static_cast<HSYNTHETICPOINTERDEVICE>(device_));
    }
}

bool TouchInjector::inject(UINT32 flags) {
    static const auto injectFn = reinterpret_cast<InjectFn>(user32("InjectSyntheticPointerInput"));
    if (!device_ || !injectFn) return false;
    POINTER_TYPE_INFO info[2]{};
    for (int i = 0; i < 2; i++) {
        info[i].type = PT_TOUCH;
        POINTER_TOUCH_INFO& touch = info[i].touchInfo;
        touch.pointerInfo.pointerType = PT_TOUCH;
        touch.pointerInfo.pointerId = static_cast<UINT32>(i);
        touch.pointerInfo.pointerFlags = flags;
        touch.pointerInfo.ptPixelLocation = points_[i];
        // A fingertip-sized contact, for programs that look at it.
        touch.touchMask = TOUCH_MASK_CONTACTAREA;
        touch.rcContact = {points_[i].x - 4, points_[i].y - 4, points_[i].x + 4, points_[i].y + 4};
    }
    const BOOL ok = injectFn(static_cast<HSYNTHETICPOINTERDEVICE>(device_), info, 2);
    if (!ok && !loggedFailure_) {
        logLine("touch: inject failed (error %lu, flags 0x%x, at %ld,%ld and %ld,%ld)", GetLastError(), flags, points_[0].x, points_[0].y,
                points_[1].x, points_[1].y);
        loggedFailure_ = true;
    }
    return ok != FALSE;
}

void TouchInjector::lift(bool canceled) {
    if (down_) inject(canceled ? POINTER_FLAG_UP | POINTER_FLAG_CANCELED : POINTER_FLAG_UP);
    down_ = false;
    mode_ = Mode::Idle;
}

void TouchInjector::endTouch() {
    touching_ = false;
    mostFingers_ = 0;
    fingersMoved_ = false;
    threeTapChecked_ = false;
    seen_ = 0;
}

void TouchInjector::checkTwoFingerTap(uint32_t count, uint64_t nowUs) {
    // A finger lifting this soon after two landed, with neither having gone anywhere, was a tap.
    if (mode_ == Mode::Undecided && count < 2 && mostFingers_ == 2 && !tapSpoiled_ && nowUs - startUs_ <= kTapUs) {
        ++taps_;
        logLine("touch: two-finger tap, sending undo");
        sendUndo();
    }
}

void TouchInjector::shape() {
    const Point c0{(start_[0].x + start_[1].x) / 2, (start_[0].y + start_[1].y) / 2};
    const Point c{(now_[0].x + now_[1].x) / 2, (now_[0].y + now_[1].y) / 2};
    const double d0 = std::max(std::hypot(start_[1].x - start_[0].x, start_[1].y - start_[0].y), 1.0);
    const double d = std::max(std::hypot(now_[1].x - now_[0].x, now_[1].y - now_[0].y), 1.0);
    const double a0 = std::atan2(start_[1].y - start_[0].y, start_[1].x - start_[0].x);
    const double a = std::atan2(now_[1].y - now_[0].y, now_[1].x - now_[0].x);

    Point p[2];
    if (mode_ == Mode::Pan || mode_ == Mode::Undecided) {
        // Both fingers keep their starting distance and angle, and move with their midpoint.
        for (int i = 0; i < 2; i++) p[i] = {start_[i].x + c.x - c0.x, start_[i].y + c.y - c0.y};
    } else {
        // Zoom keeps the starting angle; rotate keeps the starting distance and stays where it began.
        const Point center = mode_ == Mode::Zoom ? c : c0;
        const double half = (mode_ == Mode::Zoom ? d : d0) / 2;
        const double angle = mode_ == Mode::Zoom ? a0 : a;
        p[0] = {center.x - half * std::cos(angle), center.y - half * std::sin(angle)};
        p[1] = {center.x + half * std::cos(angle), center.y + half * std::sin(angle)};
    }
    for (int i = 0; i < 2; i++) {
        points_[i].x = std::clamp(static_cast<LONG>(std::lround(p[i].x)), rect_.left, std::max(rect_.right - 1, rect_.left));
        points_[i].y = std::clamp(static_cast<LONG>(std::lround(p[i].y)), rect_.top, std::max(rect_.bottom - 1, rect_.top));
    }
}

void TouchInjector::apply(const uint8_t* payload, uint32_t size, const RECT& rect, bool allowed, uint64_t nowUs) {
    if (size < 4) return;
    uint32_t info;
    memcpy(&info, payload, 4);
    const uint32_t count = info & 0xFF;
    if (count > proto::kTouchMaxFingers || size < 4 + count * sizeof(proto::TouchFinger)) return;
    proto::TouchFinger fingers[proto::kTouchMaxFingers];
    memcpy(fingers, payload + 4, count * sizeof(proto::TouchFinger));

    if (!allowed || (info & proto::kTouchCanceled)) {
        lift(true);
        endTouch();
        return;
    }

    rect_ = rect;
    const double width = std::max<LONG>(rect.right - rect.left, 1);
    const double height = std::max<LONG>(rect.bottom - rect.top, 1);
    Point at[proto::kTouchMaxFingers];
    for (uint32_t i = 0; i < count; i++) at[i] = {rect.left + fingers[i].x * (width - 1), rect.top + fingers[i].y * (height - 1)};

    // Where each finger first touched, to tell a tap from a movement.
    if (!touching_ && count > 0) {
        touching_ = true;
        touchStartUs_ = nowUs;
    }
    mostFingers_ = std::max(mostFingers_, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = 0;
        while (k < seen_ && seenIds_[k] != fingers[i].id) k++;
        if (k == seen_) {
            if (seen_ < proto::kTouchMaxFingers) {
                seenIds_[seen_] = fingers[i].id;
                seenAt_[seen_++] = at[i];
            }
        } else if (std::hypot(at[i].x - seenAt_[k].x, at[i].y - seenAt_[k].y) > kTapSlopPx) {
            fingersMoved_ = true;
        }
    }

    if (mostFingers_ >= 3) {
        // Three or more fingers are never a two-finger gesture, and one already on its way to Windows is called off. The
        // first finger to lift after exactly three landed quickly and stayed put makes a three-finger tap.
        lift(true);
        if (count < 3 && !threeTapChecked_) {
            threeTapChecked_ = true;
            if (mostFingers_ == 3 && !fingersMoved_ && nowUs - touchStartUs_ <= kThreeTapUs) {
                ++taps_;
                sendRedo();
            }
        }
        if (count == 0) endTouch();
        return;
    }
    if (count != 2) {
        checkTwoFingerTap(count, nowUs);
        lift(false);
        if (count == 0) endTouch();
        return;
    }

    // A different pair of fingers is a new gesture.
    if (mode_ != Mode::Idle && (fingers[0].id != ids_[0] || fingers[1].id != ids_[1])) lift(false);
    if (mode_ == Mode::Idle) {
        mode_ = Mode::Undecided;
        ids_[0] = fingers[0].id;
        ids_[1] = fingers[1].id;
        start_[0] = now_[0] = at[0];
        start_[1] = now_[1] = at[1];
        startUs_ = nowUs;
        tapSpoiled_ = false;
        return;
    }
    now_[0] = at[0];
    now_[1] = at[1];

    if (mode_ == Mode::Undecided) {
        // How the fingers moved since they came down: together (pan), apart or together along the line between them
        // (zoom), or past each other across it (rotate). The biggest decides, once it's past its threshold.
        const Point dp{now_[0].x - start_[0].x, now_[0].y - start_[0].y};
        const Point dq{now_[1].x - start_[1].x, now_[1].y - start_[1].y};
        if (std::hypot(dp.x, dp.y) > kTapSlopPx || std::hypot(dq.x, dq.y) > kTapSlopPx || nowUs - startUs_ > kTapUs) tapSpoiled_ = true;
        const Point axis{start_[1].x - start_[0].x, start_[1].y - start_[0].y};
        const double length = std::max(std::hypot(axis.x, axis.y), 1.0);
        const Point along{axis.x / length, axis.y / length};
        const Point apart{dq.x - dp.x, dq.y - dp.y};
        const double panned = std::hypot((dp.x + dq.x) / 2, (dp.y + dq.y) / 2);
        const double pinched = std::abs(apart.x * along.x + apart.y * along.y);
        const double twisted = std::abs(apart.x * along.y - apart.y * along.x);
        const double turned = std::abs(degrees(std::atan2(now_[1].y - now_[0].y, now_[1].x - now_[0].x) - std::atan2(axis.y, axis.x)));
        const double biggest = std::max({panned, pinched, twisted});
        Mode decided = Mode::Undecided;
        if (panned == biggest && panned >= kPanStartPx) {
            decided = Mode::Pan;
        } else if (pinched == biggest && pinched >= kZoomStartPx) {
            decided = Mode::Zoom;
        } else if (twisted == biggest && twisted >= kRotateStartPx && turned >= kRotateMinDegrees) {
            decided = Mode::Rotate;
        }
        if (decided == Mode::Undecided) return;
        mode_ = decided;
        // Windows gets the fingers where they started, then straight away where the gesture has taken them.
        points_[0] = {static_cast<LONG>(std::lround(start_[0].x)), static_cast<LONG>(std::lround(start_[0].y))};
        points_[1] = {static_cast<LONG>(std::lround(start_[1].x)), static_cast<LONG>(std::lround(start_[1].y))};
        if (!inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN)) {
            mode_ = Mode::Idle;
            return;
        }
        down_ = true;
        ++gestures_;
    }
    shape();
    inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE);
    lastInjectUs_ = nowUs;
}

void TouchInjector::tick(uint64_t nowUs) {
    if (down_ && nowUs - lastInjectUs_ >= kRepeatUs) {
        inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE);
        lastInjectUs_ = nowUs;
    }
}

int TouchInjector::msUntilTick(uint64_t nowUs) const {
    if (!down_) return 250;
    const uint64_t next = lastInjectUs_ + kRepeatUs;
    if (next <= nowUs) return 0;
    return static_cast<int>((next - nowUs) / 1000) + 1;
}

void TouchInjector::release() {
    lift(true);
}
