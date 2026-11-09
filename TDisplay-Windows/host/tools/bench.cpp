// Measures capture and encoding on the virtual display without needing the tablet.
//   tdisplay-bench width height hz seconds bitrate_mbps preset [output.hevc] [nvenc|mf]
// Prints what it measured. The optional output file can be checked with ffmpeg.
#include <winsock2.h>
#include <windows.h>
#include <avrt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "capture.h"
#include "display.h"
#include "encoder.h"
#include "util.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 7) {
        puts("usage: tdisplay-bench width height hz seconds bitrate_mbps preset [output.hevc] [nvenc|mf]");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int width = _wtoi(argv[1]);
    const int height = _wtoi(argv[2]);
    const int hz = _wtoi(argv[3]);
    const int seconds = _wtoi(argv[4]);
    const int bitrate = _wtoi(argv[5]) * 1000000;
    const int preset = _wtoi(argv[6]);
    FILE* output = argc > 7 && wcscmp(argv[7], L"-") != 0 ? _wfopen(argv[7], L"wb") : nullptr;
    const std::wstring kind = argc > 8 ? argv[8] : L"";
    const EncoderKind encoderKind = kind == L"nvenc" ? EncoderKind::Nvenc : kind == L"mf" ? EncoderKind::MediaFoundation : EncoderKind::Auto;

    VirtualDisplay display;
    if (!display.find()) {
        puts("virtual display not found");
        return 1;
    }
    int actualWidth = 0, actualHeight = 0, actualHz = 0;
    if (!display.activate(width, height, hz, &actualWidth, &actualHeight, &actualHz)) {
        puts("could not set the display mode");
        return 1;
    }
    printf("display %s: %dx%d@%d\n", narrow(display.gdiName()).c_str(), actualWidth, actualHeight, actualHz);
    Sleep(500);

    Capture capture;
    if (!capture.open(display.gdiName())) {
        puts("capture failed; see host.log");
        return 1;
    }
    auto encoder = openEncoder(capture.device(), capture.output(), {capture.width(), capture.height(), actualHz, bitrate, preset, 0}, encoderKind, CodecChoice::Auto);
    if (!encoder) {
        puts("encoder failed; see host.log");
        return 1;
    }
    if (output) fwrite(encoder->codecConfig().data(), 1, encoder->codecConfig().size(), output);

    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);

    std::vector<uint8_t> bitstream;
    std::vector<uint32_t> encodeTimes;
    std::vector<uint32_t> gaps;
    uint64_t bytes = 0;
    uint32_t frame = 0;
    uint32_t lost = 0;
    size_t largest = 0;
    const uint64_t start = nowUs();
    uint64_t lastFrame = 0;
    while (nowUs() - start < static_cast<uint64_t>(seconds) * 1000000) {
        const Capture::Result result = capture.next(20);
        if (result == Capture::Result::Lost) {
            ++lost;
            while (!capture.reopen()) Sleep(20);
            continue;
        }
        if (result != Capture::Result::NewFrame) continue;
        const uint64_t before = nowUs();
        if (lastFrame) gaps.push_back(static_cast<uint32_t>(before - lastFrame));
        lastFrame = before;
        bool keyframe = false;
        ++frame;
        if (!encoder->encode(frame, frame == 1, bitstream, &keyframe)) {
            puts("encode failed; see host.log");
            return 1;
        }
        encodeTimes.push_back(static_cast<uint32_t>(nowUs() - before));
        bytes += bitstream.size();
        largest = std::max(largest, bitstream.size());
        if (output) fwrite(bitstream.data(), 1, bitstream.size(), output);
    }
    const double elapsed = (nowUs() - start) / 1e6;
    if (task) AvRevertMmThreadCharacteristics(task);
    if (output) fclose(output);

    std::sort(encodeTimes.begin(), encodeTimes.end());
    std::sort(gaps.begin(), gaps.end());
    const auto at = [](const std::vector<uint32_t>& v, double p) { return v.empty() ? 0.0 : v[static_cast<size_t>(p * (v.size() - 1))] / 1000.0; };
    printf("frames %u in %.1f s = %.1f fps, %.1f Mbps, largest frame %.0f KB, duplication restarts %u\n", frame, elapsed, frame / elapsed,
           bytes * 8 / elapsed / 1e6, largest / 1024.0, lost);
    printf("encode ms: median %.2f, p95 %.2f, p99 %.2f, max %.2f\n", at(encodeTimes, .5), at(encodeTimes, .95), at(encodeTimes, .99), at(encodeTimes, 1));
    printf("frame gap ms: median %.2f, p95 %.2f, p99 %.2f, max %.2f\n", at(gaps, .5), at(gaps, .95), at(gaps, .99), at(gaps, 1));
    encoder->close();
    capture.close();
    display.deactivate();
    return 0;
}
