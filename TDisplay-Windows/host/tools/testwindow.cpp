// A test target for the virtual display: shows moving content at the display's refresh rate
// and logs every pen event Windows delivers, so pen input can be checked end to end.
//   tdisplay-test mode seconds logfile      (mode: desktop, noise, colors)
#include <winsock2.h>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <random>
#include <vector>

#include "display.h"
#include "util.h"

namespace {

FILE* gPenLog = nullptr;

void logPointer(HWND window, UINT message, WPARAM wparam) {
    if (!gPenLog) return;
    const UINT32 id = GET_POINTERID_WPARAM(wparam);
    POINTER_INPUT_TYPE type = PT_POINTER;
    GetPointerType(id, &type);
    const char* name = message == WM_POINTERDOWN ? "down" : message == WM_POINTERUP ? "up" : message == WM_POINTERUPDATE ? "update"
                       : message == WM_POINTERENTER ? "enter" : message == WM_POINTERLEAVE ? "leave" : "other";
    if (type == PT_PEN) {
        POINTER_PEN_INFO pen{};
        if (GetPointerPenInfo(id, &pen)) {
            fprintf(gPenLog, "%llu pen %s x=%ld y=%ld pressure=%u tiltX=%d tiltY=%d penFlags=0x%x flags=0x%x\n", static_cast<unsigned long long>(nowUs()), name,
                    pen.pointerInfo.ptPixelLocation.x, pen.pointerInfo.ptPixelLocation.y, pen.pressure, pen.tiltX, pen.tiltY, pen.penFlags, pen.pointerInfo.pointerFlags);
        } else {
            fprintf(gPenLog, "%llu pen %s (GetPointerPenInfo failed: %lu)\n", static_cast<unsigned long long>(nowUs()), name, GetLastError());
        }
    } else {
        fprintf(gPenLog, "%llu type=%d %s\n", static_cast<unsigned long long>(nowUs()), static_cast<int>(type), name);
    }
    fflush(gPenLog);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_POINTERDOWN:
        case WM_POINTERUP:
        case WM_POINTERUPDATE:
        case WM_POINTERENTER:
        case WM_POINTERLEAVE:
            logPointer(window, message, wparam);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc < 3) {
        puts("usage: tdisplay-test desktop|noise|colors|text seconds [penlog.txt] [reference.bgra]");
        return 2;
    }
    const std::wstring mode = argv[1];
    const int seconds = _wtoi(argv[2]);
    if (argc > 3) gPenLog = _wfopen(argv[3], L"w");

    VirtualDisplay display;
    if (!display.find()) {
        puts("virtual display not found");
        return 1;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(display.gdiName().c_str(), ENUM_CURRENT_SETTINGS, &dm, 0) || dm.dmPelsWidth == 0) {
        puts("virtual display is not on the desktop");
        return 1;
    }
    // Soak mode keeps the screen changing from a small corner box, leaving the rest usable.
    const bool pulse = mode.rfind(L"pulse", 0) == 0;
    const bool soak = mode == L"soak" || pulse;
    const int width = soak ? 640 : dm.dmPelsWidth;
    const int height = soak ? 360 : dm.dmPelsHeight;
    const int left = dm.dmPosition.x + (soak ? dm.dmPelsWidth - width : 0);
    const int top = dm.dmPosition.y + (soak ? dm.dmPelsHeight - height : 0);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"TDisplayTestWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"TDisplay test", WS_POPUP | WS_VISIBLE, left, top, width, height,
                                  nullptr, nullptr, windowClass.hInstance, nullptr);

    // Drawing on the GPU that owns the virtual display, so nothing has to cross between GPUs.
    Com<IDXGIFactory2> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.put()));
    Com<IDXGIAdapter1> adapter;
    bool ownsDisplay = false;
    for (UINT a = 0; !ownsDisplay && factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a) {
        Com<IDXGIOutput> output;
        for (UINT o = 0; !ownsDisplay && adapter->EnumOutputs(o, output.put()) == S_OK; ++o) {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            ownsDisplay = display.gdiName() == desc.DeviceName;
        }
    }
    if (!ownsDisplay) adapter.reset();
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr,
                                 context.put()))) {
        puts("no D3D11 device");
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = width;
    swapDesc.Height = height;
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Com<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(device.get(), window, &swapDesc, nullptr, nullptr, swapChain.put()))) {
        puts("no swap chain");
        return 1;
    }
    Com<ID3D11Texture2D> backBuffer;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.put()));
    Com<ID3D11RenderTargetView> target;
    device->CreateRenderTargetView(backBuffer.get(), nullptr, target.put());
    Com<ID3D11DeviceContext1> context1;
    context.as(context1);

    // Noise is the worst case for an encoder: nothing compresses and everything moves.
    // Text is the sharpness check: small light-on-dark, dark-on-light and colored type.
    Com<ID3D11Texture2D> noise;
    if (mode == L"noise" || mode == L"text") {
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
        if (mode == L"noise") {
            std::mt19937 random(1);
            for (uint32_t& p : pixels) p = random() | 0xFF000000u;
        } else {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            HDC dc = CreateCompatibleDC(nullptr);
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            SelectObject(dc, bitmap);
            const COLORREF backgrounds[] = {RGB(255, 255, 255), RGB(30, 30, 30), RGB(30, 30, 30), RGB(255, 255, 255)};
            const COLORREF inks[] = {RGB(0, 0, 0), RGB(235, 235, 235), RGB(255, 80, 80), RGB(0, 0, 200)};
            HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SelectObject(dc, font);
            for (int q = 0; q < 4; ++q) {
                RECT area{q % 2 * width / 2, q / 2 * height / 2, (q % 2 + 1) * width / 2, (q / 2 + 1) * height / 2};
                HBRUSH brush = CreateSolidBrush(backgrounds[q]);
                FillRect(dc, &area, brush);
                DeleteObject(brush);
                SetBkColor(dc, backgrounds[q]);
                SetTextColor(dc, inks[q]);
                for (int line = 0; line < 30; ++line) {
                    const wchar_t* text = L"The quick brown fox jumps over the lazy dog 0123456789 (TDisplay sharpness check) iIlL1 oO0 rnm";
                    TextOutW(dc, area.left + 20, area.top + 10 + line * 28, text, static_cast<int>(wcslen(text)));
                }
            }
            GdiFlush();
            memcpy(pixels.data(), bits, pixels.size() * 4);
            for (uint32_t& p : pixels) p |= 0xFF000000u;
            DeleteDC(dc);
            if (argc > 4) {
                if (FILE* reference = _wfopen(argv[4], L"wb")) {
                    fwrite(pixels.data(), 4, pixels.size(), reference);
                    fclose(reference);
                }
            }
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), static_cast<UINT>(width * 4), 0};
        device->CreateTexture2D(&desc, &data, noise.put());
    }

    const float background[4] = {0.11f, 0.12f, 0.14f, 1.0f};
    const uint64_t end = nowUs() + static_cast<uint64_t>(seconds) * 1000000;
    uint64_t frame = 0;
    // "pulse3000" changes for a few frames every 3000 ms, like picking up a pen after a pause.
    const uint64_t pulseGapUs = pulse ? static_cast<uint64_t>(_wtoi(mode.c_str() + 5)) * 1000 : 0;
    uint64_t nextPulseUs = nowUs() + pulseGapUs;
    int pulseFramesLeft = 0;
    MSG message;
    while (nowUs() < end) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return 0;
            DispatchMessageW(&message);
        }
        if (pulse) {
            if (pulseFramesLeft == 0) {
                if (nowUs() < nextPulseUs) {
                    Sleep(1);
                    continue;
                }
                pulseFramesLeft = 4;
                nextPulseUs = nowUs() + pulseGapUs;
            }
            --pulseFramesLeft;
        }
        if (mode == L"text") {
            context->CopySubresourceRegion(backBuffer.get(), 0, 0, 0, 0, noise.get(), 0, nullptr);
        } else if (mode == L"noise") {
            const int shift = static_cast<int>(frame * 7 % width);
            D3D11_BOX left{static_cast<UINT>(shift), 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), 1};
            context->CopySubresourceRegion(backBuffer.get(), 0, 0, 0, 0, noise.get(), 0, &left);
            if (shift > 0) {
                D3D11_BOX right{0, 0, 0, static_cast<UINT>(shift), static_cast<UINT>(height), 1};
                context->CopySubresourceRegion(backBuffer.get(), 0, width - shift, 0, 0, noise.get(), 0, &right);
            }
        } else if (mode == L"colors") {
            context->ClearRenderTargetView(target.get(), background);
            // A gray ramp across the top half, and primaries and secondaries across the bottom.
            constexpr int steps = 16;
            for (int i = 0; i < steps; ++i) {
                const float v = i / static_cast<float>(steps - 1);
                const float color[4] = {v, v, v, 1};
                D3D11_RECT rect{i * width / steps, 0, (i + 1) * width / steps, height / 2};
                context1->ClearView(target.get(), color, &rect, 1);
            }
            const float bars[6][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 1, 1}};
            for (int i = 0; i < 6; ++i) {
                D3D11_RECT rect{i * width / 6, height / 2, (i + 1) * width / 6, height};
                context1->ClearView(target.get(), bars[i], &rect, 1);
            }
        } else {
            // Something desktop-like: a still background with a few things moving.
            context->ClearRenderTargetView(target.get(), background);
            const int barX = static_cast<int>(frame * 6 % (width - 160));
            const float white[4] = {0.9f, 0.9f, 0.92f, 1};
            D3D11_RECT bar{barX, 0, barX + 160, height};
            context1->ClearView(target.get(), white, &bar, 1);
            const float accent[4] = {0.2f, 0.6f, 1.0f, 1};
            for (int i = 0; i < 12; ++i) {
                const int x = static_cast<int>((frame * (i + 2) * 3 + i * 211) % (width - 300));
                const int y = 80 + i * (height - 240) / 12;
                D3D11_RECT box{x, y, x + 300, y + 90};
                context1->ClearView(target.get(), accent, &box, 1);
            }
        }
        swapChain->Present(1, 0);
        ++frame;
    }
    printf("presented %llu frames in %d s\n", static_cast<unsigned long long>(frame), seconds);
    DestroyWindow(window);
    if (gPenLog) fclose(gPenLog);
    return 0;
}
