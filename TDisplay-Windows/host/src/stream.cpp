#include "stream.h"

#include <avrt.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "capture.h"
#include "encoder.h"
#include "net.h"
#include "pen.h"
#include "touch.h"
#include "util.h"

namespace {

using namespace proto;

constexpr uint64_t kPingIntervalUs = 100000;
// The USB link to this tablet occasionally stops delivering until something new travels toward the
// tablet. Replies this late mean it has happened, and opening a stream puts a packet on the link no
// matter what any other stream is waiting for. Normal replies take about 20 ms.
constexpr uint64_t kNudgeAfterUs = 45000;
constexpr const char* kNudgeSocket = "tdisplay-nudge";  // Nothing listens here, which is the cheapest answer.
constexpr DWORD kHousekeepingMs = 1000;
// With pings every 100 ms, this much silence means the link or the app is stuck. Reconnecting only
// takes a fraction of a second, so giving up early makes for a much shorter freeze.
constexpr uint64_t kSilenceLimitUs = 2000000;
// Frames on their way with no acknowledgement for this long means the tablet's decoder is stuck.
// Pings still get answered in that state, so the silence check above wouldn't catch it.
constexpr uint64_t kAckLimitUs = 1500000;
// Longest a pen injection should take before it's worth a line in the log.
constexpr uint64_t kSlowInjectUs = 50000;
// How long after the pen's last sample fingers start counting again. The same as the Mac version.
constexpr uint64_t kPenQuietUs = 500000;
constexpr int kSocketTimeoutMs = 4000;
constexpr uint64_t kStatsIntervalUs = 10000000;
// After the screen stops changing, a few more encodes of the same image let it sharpen fully.
constexpr int kRefineFrames = 6;
constexpr uint64_t kSpinUs = 1200;
constexpr size_t kFrameRing = 256;
// A still screen for this long counts as a pause in the statistics.
constexpr uint64_t kPauseUs = 500000;

// Time since an event another thread recorded, which may be slightly newer than the "now" in hand.
uint64_t since(uint64_t now, uint64_t then) {
    return now > then ? now - then : 0;
}

struct Average {
    uint64_t sum = 0;
    uint64_t max = 0;
    uint64_t count = 0;
    void add(uint64_t value) {
        sum += value;
        max = std::max(max, value);
        ++count;
    }
    double meanMs() const { return count ? static_cast<double>(sum) / count / 1000.0 : 0.0; }
    double maxMs() const { return static_cast<double>(max) / 1000.0; }
    void reset() { *this = Average{}; }
};

class Session {
public:
    Session(SOCKET socket, Adb& adb, const std::string& serial, VirtualDisplay& display, const StreamSettings& settings, std::atomic<bool>& stop,
            const std::function<void(const std::wstring&)>& onStreaming, std::wstring* problem)
        : socket_(socket), adb_(adb), serial_(serial), display_(display), settings_(settings), externalStop_(stop), onStreaming_(onStreaming), problem_(problem) {
        ackEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        nudgeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ~Session() {
        CloseHandle(nudgeEvent_);
        CloseHandle(ackEvent_);
        closesocket(socket_);
    }

    StreamEnd run();

private:
    bool handshake();
    bool sendMessage(Type type, const void* payload, uint32_t size);
    bool sendFrame(uint32_t frameId, uint64_t captureUs, bool keyframe);
    bool pingAndCheckLink(uint64_t now);
    bool shouldStop() const { return ended_.load() || externalStop_.load(); }
    bool encodeAndSend(uint64_t captureUs, bool forceKeyframe);
    void videoLoop();
    void inputLoop();
    void housekeepingLoop();
    void logStats(uint64_t now);
    void updateStatusLine();
    void endWith(StreamEnd reason);

    SOCKET socket_;
    Adb& adb_;
    std::string serial_;
    VirtualDisplay& display_;
    StreamSettings settings_;
    std::atomic<bool>& externalStop_;
    const std::function<void(const std::wstring&)>& onStreaming_;
    std::wstring* problem_;

    Capture capture_;
    std::unique_ptr<VideoEncoder> encoder_;
    PenInjector pen_;
    TouchInjector touch_;
    uint64_t lastPenUs_ = 0;  // Input thread only.
    int width_ = 0;
    int height_ = 0;
    int hz_ = 0;
    int streamFps_ = 0;

    std::atomic<bool> ended_{false};
    std::atomic<int> endReason_{static_cast<int>(StreamEnd::ConnectionLost)};
    std::atomic<uint64_t> lastReceiveUs_{0};
    std::atomic<uint64_t> lastAckUs_{0};
    std::atomic<uint64_t> lastPongUs_{0};
    HANDLE nudgeEvent_;
    uint64_t lastNudgeUs_ = 0;
    uint64_t nudges_ = 0;
    uint64_t firstUnackedUs_ = 0;  // When the current run of unacknowledged frames began.
    std::atomic<uint64_t> lastInputLoopUs_{0};
    std::atomic<int> lastMessageType_{0};
    HANDLE ackEvent_;
    std::atomic<uint32_t> ackedFrame_{0};
    std::atomic<uint32_t> sentFrame_{0};
    std::atomic<uint64_t> sentAtUs_[kFrameRing]{};
    bool pauseJustEnded_ = false;
    std::atomic<uint32_t> firstAfterPause_{0};

    std::vector<uint8_t> bitstream_;
    uint64_t lastPingUs_ = 0;
    bool tabletNotReady_ = false;
    std::wstring statusLine_;
    uint64_t lastStatusLineUs_ = 0;

    // Statistics. Encode and send times belong to the video thread, the rest are shared.
    Average encodeTime_;
    Average sendTime_;
    uint64_t bytesSent_ = 0;
    uint64_t framesSent_ = 0;
    uint64_t lastStatsUs_ = 0;
    std::mutex statsMutex_;
    Average roundTrip_;
    Average decodeTime_;
    Average linkRoundTrip_;
    Average afterPause_;
    uint64_t capturedFrames_ = 0;
    uint64_t droppedFrames_ = 0;
    uint64_t penSamples_ = 0;
    uint64_t penStrokes_ = 0;
    uint64_t touchMessages_ = 0;
    uint64_t touchGestures_ = 0;
    uint64_t touchTaps_ = 0;
    bool penTouching_ = false;
    uint64_t contactSamples_ = 0;
    double pressureSum_ = 0;
    float pressureMax_ = 0;
    Status status_{};
};

void Session::endWith(StreamEnd reason) {
    bool expected = false;
    if (ended_.compare_exchange_strong(expected, true)) {
        endReason_ = static_cast<int>(reason);
        SetEvent(ackEvent_);
        // Wakes the other thread out of any blocking socket call.
        shutdown(socket_, SD_BOTH);
    }
}

bool Session::sendMessage(Type type, const void* payload, uint32_t size) {
    const uint32_t header = makeHeader(type, size);
    WSABUF buffers[2];
    buffers[0].buf = const_cast<char*>(reinterpret_cast<const char*>(&header));
    buffers[0].len = sizeof(header);
    buffers[1].buf = const_cast<char*>(static_cast<const char*>(payload));
    buffers[1].len = size;
    return sendGather(socket_, buffers, size ? 2 : 1);
}

bool Session::handshake() {
    setRecvTimeout(socket_, kSocketTimeoutMs);
    setSendTimeout(socket_, kSocketTimeoutMs);
    setNoDelay(socket_);
    // A small buffer keeps frames from queueing out of sight when the cable is busy.
    int sendBuffer = 512 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));

    uint32_t header = 0;
    Hello hello{};
    if (!recvAll(socket_, &header, sizeof(header)) || (header >> 24) != kHello || (header & kMaxPayload) != sizeof(Hello) ||
        !recvAll(socket_, &hello, sizeof(hello))) {
        tabletNotReady_ = true;
        return false;
    }
    if (hello.version != kVersion) {
        logLine("stream: tablet app speaks protocol %u, this program speaks %u. Reinstall the tablet app.", hello.version, kVersion);
        *problem_ = L"The app on the tablet doesn't match this program. Uninstall it from the tablet, then plug the tablet in again.";
        return false;
    }
    logLine("stream: tablet is %ux%u, up to %.0f Hz, Android SDK %u", hello.width, hello.height, hello.maxRefreshHz, hello.sdk);

    const int requestedHz = std::min(settings_.maxRefreshHz, static_cast<int>(hello.maxRefreshHz + 0.5f));
    if (!display_.find()) {
        logLine("stream: the virtual display driver isn't available. Run scripts\\install-driver.ps1");
        *problem_ = L"The virtual monitor driver isn't installed. Run TDisplay's setup again.";
        return false;
    }
    if (!display_.offersMode(hello.width, hello.height, requestedHz)) {
        logLine("display: this tablet needs %ux%u@%d, which the driver doesn't offer yet", hello.width, hello.height, requestedHz);
        display_.addMode(hello.width, hello.height, requestedHz);
    }
    if (!display_.activate(hello.width, hello.height, requestedHz, &width_, &height_, &hz_)) return false;
    pen_.setTarget(display_.gdiName());
    pen_.setPressureCurve(settings_.penCurvePercent / 100.0, settings_.penMinimumPercent / 100.0);

    if (!capture_.open(display_.gdiName())) {
        // A newly attached display can take a moment to appear to DXGI.
        Sleep(300);
        if (!capture_.open(display_.gdiName())) return false;
    }
    width_ = capture_.width();
    height_ = capture_.height();
    // Enough to fill the round trip of about 24 ms at this rate without letting a queue build up.
    streamFps_ = settings_.streamFps > 0 ? std::min(settings_.streamFps, hz_) : hz_;
    if (settings_.maxFramesInFlight <= 0) settings_.maxFramesInFlight = std::clamp(streamFps_ * 24 / 1000 + 1, 2, 5);
    const VideoEncoder::Params params{width_, height_, streamFps_, settings_.bitrate, settings_.preset, settings_.minQp};
    encoder_ = openEncoder(capture_.device(), capture_.output(), params, settings_.encoder, settings_.codec);
    if (!encoder_) {
        *problem_ = L"The video encoder wouldn't start. Update your graphics driver, and close anything else that's recording the screen.";
        return false;
    }

    Config config{static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), static_cast<uint32_t>(hz_),
                  static_cast<uint32_t>(settings_.bitrate), encoder_->codec()};
    const std::vector<uint8_t>& codecConfig = encoder_->codecConfig();
    std::vector<uint8_t> message(sizeof(config) + codecConfig.size());
    memcpy(message.data(), &config, sizeof(config));
    memcpy(message.data() + sizeof(config), codecConfig.data(), codecConfig.size());
    return sendMessage(kConfig, message.data(), static_cast<uint32_t>(message.size()));
}

bool Session::sendFrame(uint32_t frameId, uint64_t captureUs, bool keyframe) {
    struct {
        uint32_t header;
        FrameHeader frame;
    } prefix;
    static_assert(sizeof(prefix.frame) == 16);
    const uint32_t payload = static_cast<uint32_t>(sizeof(FrameHeader) + bitstream_.size());
    if (payload > kMaxPayload) {
        logLine("stream: frame too large (%u bytes)", payload);
        return false;
    }
    prefix.header = makeHeader(kFrame, payload);
    prefix.frame.frameId = frameId;
    prefix.frame.captureUs = captureUs;
    prefix.frame.keyframe = keyframe ? 1 : 0;
    WSABUF buffers[2];
    buffers[0].buf = reinterpret_cast<char*>(&prefix);
    buffers[0].len = sizeof(uint32_t) + sizeof(FrameHeader);
    buffers[1].buf = reinterpret_cast<char*>(bitstream_.data());
    buffers[1].len = static_cast<ULONG>(bitstream_.size());
    return sendGather(socket_, buffers, 2);
}

bool Session::pingAndCheckLink(uint64_t now) {
    if (since(now, lastReceiveUs_.load()) > kSilenceLimitUs) {
        logLine("stream: nothing from the tablet for %.1f s (last message type 0x%02x, input thread last ran %.0f ms ago, sent %u, acked %u)",
                since(now, lastReceiveUs_.load()) / 1e6, lastMessageType_.load(), since(now, lastInputLoopUs_.load()) / 1e3, sentFrame_.load(), ackedFrame_.load());
        return false;
    }
    const bool framesLate = sentFrame_.load() != ackedFrame_.load() && since(now, lastAckUs_.load()) > kNudgeAfterUs && since(now, firstUnackedUs_) > kNudgeAfterUs;
    const bool pongLate = lastPingUs_ > lastPongUs_.load() && since(now, lastPingUs_) > kNudgeAfterUs;
    if ((framesLate || pongLate) && since(now, lastNudgeUs_) > kNudgeAfterUs) {
        lastNudgeUs_ = now;
        ++nudges_;
        SetEvent(nudgeEvent_);
    }
    if (sentFrame_.load() != ackedFrame_.load() && since(now, lastAckUs_.load()) > kAckLimitUs && since(now, firstUnackedUs_) > kAckLimitUs) {
        logLine("stream: no frame decoded for %.1f s (sent %u, acked %u; last message from the tablet %.0f ms ago, type 0x%02x)", since(now, lastAckUs_.load()) / 1e6,
                sentFrame_.load(), ackedFrame_.load(), since(now, lastReceiveUs_.load()) / 1e3, lastMessageType_.load());
        return false;
    }
    if (now - lastPingUs_ >= kPingIntervalUs) {
        lastPingUs_ = now;
        Ping ping{now};
        if (!sendMessage(kPing, &ping, sizeof(ping))) {
            logLine("stream: ping could not be sent (error %d)", WSAGetLastError());
            return false;
        }
    }
    return true;
}

bool Session::encodeAndSend(uint64_t captureUs, bool forceKeyframe) {
    const uint32_t frameId = sentFrame_.load() + 1;
    const uint64_t encodeStart = nowUs();
    if (sentFrame_.load() == ackedFrame_.load()) firstUnackedUs_ = encodeStart;
    if (pauseJustEnded_) {
        pauseJustEnded_ = false;
        firstAfterPause_ = frameId;
    }
    bool keyframe = false;
    if (!encoder_->encode(frameId, forceKeyframe, bitstream_, &keyframe)) return false;
    const uint64_t encodeEnd = nowUs();
    sentAtUs_[frameId % kFrameRing] = captureUs;
    sentFrame_ = frameId;
    if (!sendFrame(frameId, captureUs, keyframe)) {
        logLine("stream: frame could not be sent (error %d)", WSAGetLastError());
        return false;
    }
    const uint64_t sendEnd = nowUs();
    encodeTime_.add(encodeEnd - encodeStart);
    sendTime_.add(sendEnd - encodeEnd);
    bytesSent_ += bitstream_.size();
    ++framesSent_;
    return true;
}

void Session::logStats(uint64_t now) {
    if (now - lastStatsUs_ < kStatsIntervalUs) return;
    const double seconds = (now - lastStatsUs_) / 1e6;
    std::lock_guard<std::mutex> lock(statsMutex_);
    logLine("stream: %.1f fps of %.1f captured, %.1f Mbps | encode %.1f ms (max %.1f), send %.1f (max %.1f) | link rtt %.1f (max %.1f) | capture-to-ack %.1f (max %.1f), decode %.1f (max %.1f) | dropped %llu, pen %llu, nudges %llu | tablet %.0f Hz, saver %u, thermal %u | pen down %llu samples in %llu strokes, raw pressure avg %.3f max %.3f | touch %llu, two-finger gestures %llu, taps %llu | first frame after a pause %.1f (max %.1f)",
            framesSent_ / seconds, capturedFrames_ / seconds, bytesSent_ * 8.0 / seconds / 1e6, encodeTime_.meanMs(), encodeTime_.maxMs(), sendTime_.meanMs(),
            sendTime_.maxMs(), linkRoundTrip_.meanMs(), linkRoundTrip_.maxMs(), roundTrip_.meanMs(), roundTrip_.maxMs(), decodeTime_.meanMs(),
            decodeTime_.maxMs(), static_cast<unsigned long long>(droppedFrames_), static_cast<unsigned long long>(penSamples_), static_cast<unsigned long long>(nudges_), status_.refreshHz, status_.powerSave,
            status_.thermal, static_cast<unsigned long long>(contactSamples_), static_cast<unsigned long long>(penStrokes_),
            contactSamples_ ? pressureSum_ / contactSamples_ : 0.0, pressureMax_, static_cast<unsigned long long>(touchMessages_),
            static_cast<unsigned long long>(touchGestures_), static_cast<unsigned long long>(touchTaps_), afterPause_.meanMs(), afterPause_.maxMs());
    touchMessages_ = 0;
    touchGestures_ = 0;
    touchTaps_ = 0;
    encodeTime_.reset();
    sendTime_.reset();
    roundTrip_.reset();
    decodeTime_.reset();
    linkRoundTrip_.reset();
    afterPause_.reset();
    capturedFrames_ = 0;
    nudges_ = 0;
    droppedFrames_ = 0;
    penSamples_ = 0;
    penStrokes_ = 0;
    contactSamples_ = 0;
    pressureSum_ = 0;
    pressureMax_ = 0;
    bytesSent_ = 0;
    framesSent_ = 0;
    lastStatsUs_ = now;
}

// Once a second is plenty for something that only changes when the user changes a setting.
void Session::updateStatusLine() {
    const uint64_t now = nowUs();
    if (now - lastStatusLineUs_ < 1000000) return;
    lastStatusLineUs_ = now;
    bool powerSave;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        powerSave = status_.powerSave != 0;
    }
    std::wstring line = L"Connected: " + std::to_wstring(width_) + L"x" + std::to_wstring(height_) + L" at " + std::to_wstring(hz_) + L" Hz";
    if (powerSave && hz_ > 60) line += L". Battery Saver is limiting the tablet to 60 Hz.";
    if (line != statusLine_) {
        statusLine_ = line;
        onStreaming_(line);
    }
}

void Session::videoLoop() {
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    if (task) AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    bool needKeyframe = true;
    bool haveFrame = false;
    bool pending = false;  // A new image is ready but sending it now would exceed the panel's rate.
    bool announced = false;
    int refineLeft = 0;
    uint64_t lastEncodeUs = 0;
    uint64_t pendingCaptureUs = 0;
    uint64_t lastNewFrameUs = nowUs();
    // Windows can compose faster than the panel refreshes when another screen is faster; frames
    // beyond the panel's rate would only cost bandwidth and heat. The margin absorbs clock jitter.
    const uint64_t minGapUs = 1000000 / std::max(streamFps_, 1) - 100;
    const uint64_t refineGapUs = 2000000 / std::max(hz_, 1);
    const UINT frameMs = std::max(1, 1000 / std::max(hz_, 1));
    lastStatsUs_ = nowUs();

    const auto send = [&](uint64_t captureUs) {
        // Pacing is measured start to start, so the time spent encoding doesn't slow the rate.
        lastEncodeUs = nowUs();
        if (!encodeAndSend(captureUs, needKeyframe)) return false;
        needKeyframe = false;
        if (!announced) {
            announced = true;
            logLine("stream: first frame sent");
        }
        return true;
    };

    while (!shouldStop()) {
        uint64_t now = nowUs();
        if (!pingAndCheckLink(now)) break;
        logStats(now);
        if (announced) updateStatusLine();

        // Don't get ahead of the tablet. Anything newer that appears while waiting simply
        // replaces what the capture is holding, so waiting never adds stale frames.
        if (sentFrame_.load() - ackedFrame_.load() >= static_cast<uint32_t>(settings_.maxFramesInFlight)) {
            WaitForSingleObject(ackEvent_, 20);
            continue;
        }

        const uint64_t sinceEncode = now - lastEncodeUs;
        // Waits in the capture call only have millisecond precision, so the last stretch is a short
        // spin. It only happens while the screen is changing faster than the panel can show.
        if (pending && sinceEncode + kSpinUs >= minGapUs) {
            while (nowUs() - lastEncodeUs < minGapUs) YieldProcessor();
            pending = false;
            if (!send(pendingCaptureUs)) break;
            continue;
        }

        UINT timeout = refineLeft > 0 ? frameMs : 20;
        if (pending) timeout = static_cast<UINT>((minGapUs - sinceEncode - kSpinUs) / 1000 + 1);
        const Capture::Result result = capture_.next(timeout);
        const uint64_t captureUs = nowUs();
        if (result == Capture::Result::Lost) {
            // Normal around UAC prompts, mode changes and locking the laptop.
            while (!shouldStop() && !capture_.reopen()) {
                if (capture_.width() != width_ || capture_.height() != height_ || capture_.deviceLost()) {
                    logLine("stream: display size changed or GPU reset; restarting session");
                    endWith(StreamEnd::ConnectionLost);
                    break;
                }
                now = nowUs();
                if (!pingAndCheckLink(now)) {
                    endWith(StreamEnd::ConnectionLost);
                    break;
                }
                Sleep(50);
            }
            continue;
        }
        if (result == Capture::Result::NewFrame) {
            ++capturedFrames_;
            if (captureUs - lastNewFrameUs > kPauseUs) pauseJustEnded_ = true;
            lastNewFrameUs = captureUs;
            haveFrame = true;
            pending = true;
            pendingCaptureUs = captureUs;
            refineLeft = kRefineFrames;
        } else if (!pending && haveFrame && refineLeft > 0 && captureUs - lastEncodeUs >= refineGapUs) {
            --refineLeft;
            if (!send(captureUs)) break;
        }
    }

    endWith(ended_.load() ? static_cast<StreamEnd>(endReason_.load())
                          : (externalStop_.load() ? StreamEnd::Stopped : StreamEnd::ConnectionLost));
    if (task) AvRevertMmThreadCharacteristics(task);
}

// Everything that may block for a while and doesn't need to be quick lives here, away from the threads
// that carry the stream.
void Session::housekeepingLoop() {
    uint64_t lastRefreshUs = 0;
    while (!shouldStop()) {
        if (WaitForSingleObject(nudgeEvent_, kHousekeepingMs) == WAIT_OBJECT_0 && !shouldStop()) {
            std::string ignored;
            SOCKET nudge = adb_.openAbstract(serial_, kNudgeSocket, &ignored);
            if (nudge != INVALID_SOCKET) closesocket(nudge);
        }
        const uint64_t now = nowUs();
        if (now - lastRefreshUs >= kHousekeepingMs * 1000ull) {
            lastRefreshUs = now;
            pen_.refreshRect();
        }
    }
}

void Session::inputLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    char payload[256];
    WSAPOLLFD poll{};
    poll.fd = socket_;
    poll.events = POLLRDNORM;
    while (!shouldStop()) {
        lastInputLoopUs_ = nowUs();
        const int ready = WSAPoll(&poll, 1, std::min(pen_.msUntilTick(lastInputLoopUs_.load()), touch_.msUntilTick(lastInputLoopUs_.load())));
        lastInputLoopUs_ = nowUs();
        if (ready < 0) {
            logLine("stream: waiting for tablet data failed (error %d)", WSAGetLastError());
            endWith(StreamEnd::ConnectionLost);
            break;
        }
        if (ready > 0) {
            uint32_t header = 0;
            if (!recvAll(socket_, &header, sizeof(header))) {
                if (!shouldStop()) logLine("stream: tablet connection closed (error %d)", WSAGetLastError());
                endWith(StreamEnd::ConnectionLost);
                break;
            }
            const uint32_t size = header & kMaxPayload;
            if (size > sizeof(payload) || (size && !recvAll(socket_, payload, size))) {
                logLine("stream: bad message from the tablet (type %u, %u bytes)", header >> 24, size);
                endWith(StreamEnd::ConnectionLost);
                break;
            }
            const uint64_t now = nowUs();
            lastReceiveUs_ = now;
            lastMessageType_ = static_cast<int>(header >> 24);
            switch (header >> 24) {
                case kPen:
                    if (size == sizeof(PenSample)) {
                        const PenSample& sample = *reinterpret_cast<const PenSample*>(payload);
                        pen_.apply(sample, now);
                        lastPenUs_ = now;
                        // The pen takes over from fingers straight away.
                        if (pen_.penNear()) touch_.release();
                        const uint64_t injectUs = nowUs() - now;
                        if (injectUs > kSlowInjectUs) logLine("stream: pen injection took %.0f ms", injectUs / 1e3);
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        ++penSamples_;
                        const bool touching = (sample.flags & kPenContact) != 0;
                        if (touching) {
                            if (!penTouching_) ++penStrokes_;
                            ++contactSamples_;
                            pressureSum_ += sample.pressure;
                            pressureMax_ = std::max(pressureMax_, sample.pressure);
                        }
                        penTouching_ = touching;
                    }
                    break;
                case kAck:
                    if (size == sizeof(Ack)) {
                        const Ack& ack = *reinterpret_cast<const Ack*>(payload);
                        ackedFrame_ = ack.frameId;
                        lastAckUs_ = now;
                        SetEvent(ackEvent_);
                        const uint64_t captured = sentAtUs_[ack.frameId % kFrameRing].load();
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        if (captured) roundTrip_.add(now - captured);
                        // The tablet may skip straight to a newer picture, so the first one it reports counts.
                        const uint32_t firstAfterPause = firstAfterPause_.load();
                        if (firstAfterPause && captured && ack.frameId - firstAfterPause < 4u) {
                            afterPause_.add(now - captured);
                            firstAfterPause_ = 0;
                        }
                        decodeTime_.add(ack.decodeUs);
                        droppedFrames_ += ack.droppedFrames;
                    }
                    break;
                case kStatus:
                    if (size == sizeof(Status)) {
                        const Status& status = *reinterpret_cast<const Status*>(payload);
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        if (status.powerSave != status_.powerSave || status.thermal != status_.thermal) {
                            logLine("stream: tablet Battery Saver %s, thermal status %u, panel %.0f Hz", status.powerSave ? "on" : "off", status.thermal, status.refreshHz);
                        }
                        status_ = status;
                    }
                    break;
                case kGoodbye:
                    logLine("stream: tablet app closed the session");
                    endWith(StreamEnd::AppHidden);
                    break;
                case kPong:
                    if (size == sizeof(Pong)) {
                        const Pong& pong = *reinterpret_cast<const Pong*>(payload);
                        lastPongUs_ = now;
                        std::lock_guard<std::mutex> lock(statsMutex_);
                        linkRoundTrip_.add(now - pong.laptopUs);
                    }
                    break;
                case kTouch: {
                    // Fingers count only once the pen has been away for a moment, so a hand resting on the screen
                    // between strokes doesn't scroll or zoom.
                    const bool allowed = touchGesturesEnabled() && !pen_.penNear() && now - lastPenUs_ > kPenQuietUs;
                    const uint64_t before = touch_.gestures();
                    const uint64_t tapsBefore = touch_.taps();
                    touch_.apply(reinterpret_cast<const uint8_t*>(payload), size, pen_.rect(), allowed, now);
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    ++touchMessages_;
                    touchGestures_ += touch_.gestures() - before;
                    touchTaps_ += touch_.taps() - tapsBefore;
                    break;
                }
                default:
                    break;
            }
        }
        pen_.tick(nowUs());
        touch_.tick(nowUs());
    }
    touch_.release();
    pen_.release();
}

StreamEnd Session::run() {
    lastReceiveUs_ = nowUs();
    if (!handshake()) return tabletNotReady_ ? StreamEnd::NotReady : StreamEnd::SetupFailed;
    lastReceiveUs_ = nowUs();
    std::thread input([this] { inputLoop(); });
    std::thread housekeeping([this] { housekeepingLoop(); });
    videoLoop();
    SetEvent(nudgeEvent_);
    input.join();
    housekeeping.join();
    encoder_->close();
    capture_.close();
    if (externalStop_.load()) return StreamEnd::Stopped;
    return static_cast<StreamEnd>(endReason_.load());
}

}  // namespace

StreamEnd runStream(SOCKET socket, Adb& adb, const std::string& serial, VirtualDisplay& display, const StreamSettings& settings, std::atomic<bool>& stop,
                    const std::function<void(const std::wstring&)>& onStreaming, std::wstring* problem) {
    Session session(socket, adb, serial, display, settings, stop, onStreaming, problem);
    return session.run();
}
