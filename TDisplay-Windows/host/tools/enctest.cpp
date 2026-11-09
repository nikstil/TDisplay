// Measures one encoder on one GPU with nothing else involved: no virtual display, no capture and no tablet.
//   tdisplay-enctest gpu encoder codec width height fps seconds bitrate_mbps preset pattern [output] [last-frame.bgra]
// gpu is part of the adapter's name. encoder is auto, nvenc or mf, and codec is auto, hevc or h264.
// pattern is desktop, text, colors or noise. Prints what it measured. The output file can be checked with ffmpeg,
// and last-frame.bgra holds the picture the stream should end on.
#include <winsock2.h>
#include <windows.h>
#include <avrt.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <random>
#include <string>
#include <vector>

#include "encoder.h"
#include "util.h"

// On a laptop with two GPUs, NVIDIA's Media Foundation encoder only starts for programs asking for that GPU.
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 1;

namespace {

constexpr int kStillFrames = 30;  // The text page stops moving this long before the end, as a page stops being scrolled.
constexpr int kTestKeyframeAfter = 120;

std::vector<uint32_t> drawTextPage(int width, int height) {
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
        for (int line = 10; line < area.bottom - area.top - 28; line += 28) {
            const wchar_t* text = L"The quick brown fox jumps over the lazy dog 0123456789 (TDisplay sharpness check) iIlL1 oO0 rnm";
            TextOutW(dc, area.left + 20, area.top + line, text, static_cast<int>(wcslen(text)));
        }
    }
    GdiFlush();
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    memcpy(pixels.data(), bits, pixels.size() * 4);
    for (uint32_t& p : pixels) p |= 0xFF000000u;
    DeleteDC(dc);
    DeleteObject(bitmap);
    DeleteObject(font);
    return pixels;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 11) {
        puts("usage: tdisplay-enctest gpu encoder codec width height fps seconds bitrate_mbps preset desktop|text|colors|noise [output] [last-frame.bgra]");
        return 2;
    }
    logToConsole();
    const std::wstring gpu = argv[1];
    const std::wstring encoderName = argv[2];
    const std::wstring codecName = argv[3];
    const int width = _wtoi(argv[4]);
    const int height = _wtoi(argv[5]);
    const int fps = _wtoi(argv[6]);
    const int seconds = _wtoi(argv[7]);
    const int bitrate = _wtoi(argv[8]) * 1000000;
    const int preset = _wtoi(argv[9]);
    const std::wstring pattern = argv[10];
    const EncoderKind kind = encoderName == L"nvenc" ? EncoderKind::Nvenc : encoderName == L"mf" ? EncoderKind::MediaFoundation : EncoderKind::Auto;
    const CodecChoice codecChoice = codecName == L"hevc" ? CodecChoice::Hevc : codecName == L"h264" ? CodecChoice::H264 : CodecChoice::Auto;

    Com<IDXGIFactory1> factory;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()));
    Com<IDXGIAdapter1> adapter;
    bool found = false;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        found = wcsstr(desc.Description, gpu.c_str()) != nullptr;
    }
    if (!found) {
        puts("no such GPU");
        return 1;
    }

    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                 device.put(), nullptr, context.put()))) {
        puts("no D3D11 device");
        return 1;
    }
    Com<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device.as(dxgiDevice))) {
        dxgiDevice->SetGPUThreadPriority(7);
        dxgiDevice->SetMaximumFrameLatency(1);
    }
    Com<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.as(multithread))) multithread->SetMultithreadProtected(TRUE);
    Com<ID3D11DeviceContext1> context1;
    context.as(context1);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> input;
    Com<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, input.put())) || FAILED(device->CreateRenderTargetView(input.get(), nullptr, target.put()))) {
        puts("no input texture");
        return 1;
    }

    // A page twice as tall as the screen for the text to scroll through.
    std::vector<uint32_t> page;
    Com<ID3D11Texture2D> pageTexture;
    if (pattern == L"text" || pattern == L"noise") {
        if (pattern == L"text") {
            page = drawTextPage(width, height * 2);
        } else {
            page.resize(static_cast<size_t>(width) * height * 2);
            std::mt19937 random(1);
            for (uint32_t& p : page) p = random() | 0xFF000000u;
        }
        D3D11_TEXTURE2D_DESC pageDesc = desc;
        pageDesc.Height = height * 2;
        pageDesc.Usage = D3D11_USAGE_IMMUTABLE;
        pageDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{page.data(), static_cast<UINT>(width * 4), 0};
        device->CreateTexture2D(&pageDesc, &data, pageTexture.put());
    }

    const auto draw = [&](int frame, int total) {
        if (pattern == L"text" || pattern == L"noise") {
            const int moving = pattern == L"text" ? std::max(0, std::min(frame, total - kStillFrames)) : frame;
            const UINT top = static_cast<UINT>(moving * 4 % height);
            const D3D11_BOX box{0, top, 0, static_cast<UINT>(width), top + height, 1};
            context->CopySubresourceRegion(input.get(), 0, 0, 0, 0, pageTexture.get(), 0, &box);
        } else if (pattern == L"colors") {
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
            const float background[4] = {0.11f, 0.12f, 0.14f, 1.0f};
            context->ClearRenderTargetView(target.get(), background);
            const int barX = frame * 6 % (width - 160);
            const float white[4] = {0.9f, 0.9f, 0.92f, 1};
            D3D11_RECT bar{barX, 0, barX + 160, height};
            context1->ClearView(target.get(), white, &bar, 1);
            const float accent[4] = {0.2f, 0.6f, 1.0f, 1};
            for (int i = 0; i < 12; ++i) {
                const int x = (frame * (i + 2) * 3 + i * 211) % (width - 300);
                const int y = 80 + i * (height - 240) / 12;
                D3D11_RECT rect{x, y, x + 300, y + 90};
                context1->ClearView(target.get(), accent, &rect, 1);
            }
        }
        context->Flush();
    };

    draw(0, 1);
    const uint64_t openStart = nowUs();
    auto encoder = openEncoder(device.get(), input.get(), {width, height, fps, bitrate, preset, 0}, kind, codecChoice);
    if (!encoder) {
        puts("no encoder would start");
        return 1;
    }
    printf("encoder opened in %.1f ms, codec config %zu bytes\n", (nowUs() - openStart) / 1e3, encoder->codecConfig().size());

    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const int total = seconds * fps;
    const uint64_t period = 1000000 / fps;
    std::vector<uint8_t> bitstream;
    std::vector<uint8_t> stream(encoder->codecConfig());
    std::vector<uint32_t> encodeTimes;
    std::vector<uint32_t> sizes;
    std::vector<int> keyframes;
    uint64_t bytes = 0;
    const uint64_t start = nowUs();
    for (int frame = 0; frame < total; ++frame) {
        while (nowUs() - start < frame * period) {
            if (nowUs() - start + 2000 < frame * period) Sleep(1);
            else YieldProcessor();
        }
        draw(frame, total);
        const uint64_t before = nowUs();
        bool keyframe = false;
        if (!encoder->encode(frame + 1, frame == 0 || frame == kTestKeyframeAfter, bitstream, &keyframe)) {
            puts("encode failed");
            return 1;
        }
        encodeTimes.push_back(static_cast<uint32_t>(nowUs() - before));
        sizes.push_back(static_cast<uint32_t>(bitstream.size()));
        if (keyframe) keyframes.push_back(frame);
        bytes += bitstream.size();
        if (argc > 11) stream.insert(stream.end(), bitstream.begin(), bitstream.end());
    }
    const double elapsed = (nowUs() - start) / 1e6;
    if (task) AvRevertMmThreadCharacteristics(task);

    if (argc > 11) {
        if (FILE* output = _wfopen(argv[11], L"wb")) {
            fwrite(stream.data(), 1, stream.size(), output);
            fclose(output);
        }
    }
    if (argc > 12 && !page.empty()) {
        // The whole page, so each frame of the stream can be compared with what it showed.
        if (FILE* file = _wfopen((std::wstring(argv[12]) + L".page").c_str(), L"wb")) {
            fwrite(page.data(), 4, page.size(), file);
            fclose(file);
        }
    }
    if (argc > 12) {
        // Read the input back for comparing with what the stream decodes to.
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> staging;
        device->CreateTexture2D(&stagingDesc, nullptr, staging.put());
        context->CopyResource(staging.get(), input.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            if (FILE* last = _wfopen(argv[12], L"wb")) {
                for (int y = 0; y < height; ++y) fwrite(static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, 4, width, last);
                fclose(last);
            }
            context->Unmap(staging.get(), 0);
        }
    }

    std::sort(encodeTimes.begin(), encodeTimes.end());
    std::vector<uint32_t> sortedSizes = sizes;
    std::sort(sortedSizes.begin(), sortedSizes.end());
    const auto at = [](const std::vector<uint32_t>& v, double p) { return v.empty() ? 0.0 : static_cast<double>(v[static_cast<size_t>(p * (v.size() - 1))]); };
    printf("codec %s, %d frames in %.1f s = %.1f fps, %.1f Mbps\n", encoder->codec() == proto::kCodecHevc ? "HEVC" : "H.264", total, elapsed, total / elapsed, bytes * 8 / elapsed / 1e6);
    printf("encode ms: median %.2f, p95 %.2f, p99 %.2f, max %.2f\n", at(encodeTimes, .5) / 1e3, at(encodeTimes, .95) / 1e3, at(encodeTimes, .99) / 1e3, at(encodeTimes, 1) / 1e3);
    printf("frame KB: median %.1f, p95 %.1f, p99 %.1f, max %.1f\n", at(sortedSizes, .5) / 1024, at(sortedSizes, .95) / 1024, at(sortedSizes, .99) / 1024, at(sortedSizes, 1) / 1024);
    printf("keyframes (%zu):", keyframes.size());
    for (size_t i = 0; i < keyframes.size() && i < 12; ++i) printf(" %d", keyframes[i]);
    printf("\n");
    encoder->close();
    return 0;
}
