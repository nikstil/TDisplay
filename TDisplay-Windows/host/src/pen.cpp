#include "pen.h"

#include <algorithm>
#include <cmath>

#include "util.h"

namespace {

// Windows cancels a pointer that stops receiving input; this keeps it alive well before that.
constexpr uint64_t kRepeatUs = 40000;
// Android reports the pen leaving hover just before it touches down. Waiting this long before
// telling Windows avoids a false out-of-range between hover and contact.
constexpr uint64_t kLeaveDelayUs = 60000;
// A pen lifted straight out of range after touching may never report leaving. Silence this long
// while hovering is treated as gone; a pen that's really still there comes back on its next sample.
constexpr uint64_t kHoverSilenceUs = 500000;

using CreateDeviceFn = HSYNTHETICPOINTERDEVICE(WINAPI*)(POINTER_INPUT_TYPE, ULONG, POINTER_FEEDBACK_MODE);
using InjectFn = BOOL(WINAPI*)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32);
using DestroyFn = VOID(WINAPI*)(HSYNTHETICPOINTERDEVICE);

CreateDeviceFn createDevice() {
    return reinterpret_cast<CreateDeviceFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "CreateSyntheticPointerDevice"));
}
InjectFn injectFn() {
    static InjectFn fn = reinterpret_cast<InjectFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "InjectSyntheticPointerInput"));
    return fn;
}
DestroyFn destroyFn() {
    return reinterpret_cast<DestroyFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "DestroySyntheticPointerDevice"));
}

}  // namespace

PenInjector::PenInjector() {
    auto create = createDevice();
    if (create) device_ = create(PT_PEN, 1, POINTER_FEEDBACK_DEFAULT);
    if (!device_) logLine("pen: could not create a pen device (error %lu)", GetLastError());
}

PenInjector::~PenInjector() {
    release();
    if (device_) {
        if (auto destroy = destroyFn()) destroy(static_cast<HSYNTHETICPOINTERDEVICE>(device_));
    }
}

void PenInjector::setTarget(const std::wstring& gdiName) {
    target_ = gdiName;
    refreshRect();
}

void PenInjector::setPressureCurve(double exponent, double minimum) {
    pressureExponent_ = std::clamp(exponent, 0.1, 3.0);
    pressureMinimum_ = std::clamp(minimum, 0.0, 0.9);
}

void PenInjector::refreshRect() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(target_.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
        // Injected positions are measured from the top-left of the whole virtual screen, not from
        // the primary display, and a screen placed left of or above the primary makes those differ.
        RECT rect;
        rect.left = mode.dmPosition.x - GetSystemMetrics(SM_XVIRTUALSCREEN);
        rect.top = mode.dmPosition.y - GetSystemMetrics(SM_YVIRTUALSCREEN);
        rect.right = rect.left + static_cast<LONG>(mode.dmPelsWidth);
        rect.bottom = rect.top + static_cast<LONG>(mode.dmPelsHeight);
        std::lock_guard<std::mutex> lock(rectMutex_);
        rect_ = rect;
    }
}

bool PenInjector::inject(UINT32 flags) {
    if (!device_) return false;
    POINTER_TYPE_INFO info{};
    info.type = PT_PEN;
    POINTER_PEN_INFO& pen = info.penInfo;
    pen.pointerInfo.pointerType = PT_PEN;
    pen.pointerInfo.pointerFlags = flags;
    pen.pointerInfo.ptPixelLocation = point_;
    pen.penFlags = penFlags_;
    pen.penMask = PEN_MASK_PRESSURE | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
    pen.pressure = pressure_;
    pen.tiltX = tiltX_;
    pen.tiltY = tiltY_;
    const BOOL ok = injectFn()(static_cast<HSYNTHETICPOINTERDEVICE>(device_), &info, 1);
    if (!ok && !loggedFailure_) {
        logLine("pen: inject failed (error %lu, flags 0x%x, at %ld,%ld)", GetLastError(), flags, point_.x, point_.y);
        loggedFailure_ = true;
    }
    return ok != FALSE;
}

void PenInjector::apply(const proto::PenSample& sample, uint64_t nowUs) {
    lastSampleUs_ = nowUs;

    const bool wantContact = (sample.flags & proto::kPenContact) != 0;
    const bool wantInRange = wantContact || (sample.flags & proto::kPenInRange) != 0;

    // A tool change can't happen in place: Windows treats pen and eraser as separate strokes.
    uint32_t tool = sample.flags & proto::kPenEraser;
    if (contact_ && wantContact) tool = tool_;
    if (inRange_ && tool != tool_ && !contact_) {
        inject(POINTER_FLAG_UPDATE);
        inRange_ = false;
    }
    tool_ = tool;

    RECT rect;
    {
        std::lock_guard<std::mutex> lock(rectMutex_);
        rect = rect_;
    }
    const int width = std::max<LONG>(rect.right - rect.left, 1);
    const int height = std::max<LONG>(rect.bottom - rect.top, 1);
    point_.x = rect.left + std::clamp(static_cast<int>(std::lround(sample.x * (width - 1))), 0, width - 1);
    point_.y = rect.top + std::clamp(static_cast<int>(std::lround(sample.y * (height - 1))), 0, height - 1);
    const double shaped = pressureMinimum_ + (1.0 - pressureMinimum_) * std::pow(std::clamp<double>(sample.pressure, 0.0, 1.0), pressureExponent_);
    pressure_ = wantContact ? static_cast<uint32_t>(std::clamp(std::lround(shaped * 1024.0), 1L, 1024L)) : 0;
    tiltX_ = std::clamp(static_cast<int>(std::lround(sample.tiltX)), -90, 90);
    tiltY_ = std::clamp(static_cast<int>(std::lround(sample.tiltY)), -90, 90);
    penFlags_ = PEN_FLAG_NONE;
    if (sample.flags & proto::kPenBarrel) penFlags_ |= PEN_FLAG_BARREL;
    // "Inverted" means the eraser end is the one in range; "eraser" means it's touching.
    if (tool) penFlags_ |= wantContact ? PEN_FLAG_INVERTED | PEN_FLAG_ERASER : PEN_FLAG_INVERTED;

    if (sample.flags & proto::kPenCanceled) {
        if (contact_) inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP | POINTER_FLAG_CANCELED);
        contact_ = false;
        inject(POINTER_FLAG_UPDATE);
        inRange_ = false;
        leaveAtUs_ = 0;
        lastInjectUs_ = nowUs;
        return;
    }

    if (!wantInRange) {
        // Hold this back briefly in case contact follows straight away.
        if (inRange_ && !leaveAtUs_) leaveAtUs_ = nowUs + kLeaveDelayUs;
        if (contact_) {
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP);
            contact_ = false;
            lastInjectUs_ = nowUs;
        }
        return;
    }
    leaveAtUs_ = 0;

    UINT32 flags = POINTER_FLAG_INRANGE;
    if (wantContact) {
        flags |= POINTER_FLAG_INCONTACT;
        flags |= contact_ ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN;
    } else {
        flags |= contact_ ? POINTER_FLAG_UP : POINTER_FLAG_UPDATE;
    }
    inject(flags);
    inRange_ = true;
    contact_ = wantContact;
    lastInjectUs_ = nowUs;
}

void PenInjector::tick(uint64_t nowUs) {
    if (leaveAtUs_ && nowUs >= leaveAtUs_) {
        leaveAtUs_ = 0;
        inject(POINTER_FLAG_UPDATE);
        inRange_ = false;
        lastInjectUs_ = nowUs;
        return;
    }
    if (inRange_ && !contact_ && nowUs - lastSampleUs_ >= kHoverSilenceUs) {
        inject(POINTER_FLAG_UPDATE);
        inRange_ = false;
        lastInjectUs_ = nowUs;
        return;
    }
    if (inRange_ && nowUs - lastInjectUs_ >= kRepeatUs) {
        UINT32 flags = POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
        if (contact_) flags |= POINTER_FLAG_INCONTACT;
        inject(flags);
        lastInjectUs_ = nowUs;
    }
}

int PenInjector::msUntilTick(uint64_t nowUs) const {
    if (!inRange_) return 250;
    uint64_t next = lastInjectUs_ + kRepeatUs;
    if (leaveAtUs_ && leaveAtUs_ < next) next = leaveAtUs_;
    if (next <= nowUs) return 0;
    return static_cast<int>((next - nowUs) / 1000) + 1;
}

void PenInjector::release() {
    if (contact_) {
        inject(POINTER_FLAG_INRANGE | POINTER_FLAG_UP);
        contact_ = false;
    }
    if (inRange_) {
        inject(POINTER_FLAG_UPDATE);
        inRange_ = false;
    }
    leaveAtUs_ = 0;
}
