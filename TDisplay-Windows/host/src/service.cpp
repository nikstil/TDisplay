#include "service.h"

#include "adb.h"
#include "display.h"
#include "protocol.h"
#include "stream.h"
#include "util.h"

namespace {

// Private so that other software's adb can't interfere with ours.
constexpr int kAdbServerPort = 15037;
// A replug or app restart inside this window leaves windows on the tablet where they are.
constexpr uint64_t kDisplayGraceUs = 20000000;
constexpr uint64_t kReturnedDisplayGraceUs = 3000000;
// How long a plugged-in tablet can go missing from adb before restarting the server, and how
// often to retry that if something else is holding the USB connection. The time counts from when
// the tablet's USB interface appears, so a tablet plugged back in gets the same time as any other
// for adb to find it, instead of setting off a restart the moment it's back.
constexpr uint64_t kUnseenDeviceUs = 1500000;
constexpr uint64_t kServerRestartGapUs = 15000000;
const std::string kLaunchCommand = std::string("am start -n ") + proto::kAppId + "/com.tdisplay.DisplayActivity";
constexpr int kOldestAndroidSdk = 30;  // Android 11.
// The app may not open on the first try: just after an update replaces a pinned app, Android is still leaving app pinning
// and refuses to start anything for a moment. It's asked again this many times, this far apart.
constexpr int kLaunchTries = 4;
constexpr uint64_t kLaunchRetryUs = 3000000;
// While waiting for the app, this often it checks the app is still there and current, so one removed or replaced while
// the tablet stays plugged in gets reinstalled, as it would on a replug.
constexpr uint64_t kAppCheckUs = 5000000;
// An install that fails for no reason the user could fix, such as Android still finishing an uninstall, is tried again
// at the next check, up to this many times.
constexpr int kReinstallTries = 3;

std::wstring adbPath() {
    std::wstring beside = exeDirectory() + L"\\adb\\adb.exe";
    if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) return beside;
    return exeDirectory() + L"\\..\\tools\\android-sdk\\platform-tools\\adb.exe";
}

int readSetting(const wchar_t* name, int fallback) {
    const std::wstring path = logDirectory() + L"\\config.ini";
    return GetPrivateProfileIntW(L"stream", name, fallback, path.c_str());
}

std::wstring readWord(const wchar_t* name) {
    const std::wstring path = logDirectory() + L"\\config.ini";
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"stream", name, L"", value, 32, path.c_str());
    return value;
}

StreamSettings loadSettings() {
    StreamSettings s;
    s.maxRefreshHz = readSetting(L"refresh", s.maxRefreshHz);
    s.streamFps = readSetting(L"stream_fps", s.streamFps);
    s.bitrate = readSetting(L"bitrate_mbps", s.bitrate / 1000000) * 1000000;
    const std::wstring encoder = readWord(L"encoder");
    if (_wcsicmp(encoder.c_str(), L"nvenc") == 0) s.encoder = EncoderKind::Nvenc;
    if (_wcsicmp(encoder.c_str(), L"mf") == 0) s.encoder = EncoderKind::MediaFoundation;
    const std::wstring codec = readWord(L"codec");
    if (_wcsicmp(codec.c_str(), L"hevc") == 0) s.codec = CodecChoice::Hevc;
    if (_wcsicmp(codec.c_str(), L"h264") == 0) s.codec = CodecChoice::H264;
    s.preset = readSetting(L"preset", s.preset);
    s.minQp = readSetting(L"min_qp", s.minQp);
    s.maxFramesInFlight = readSetting(L"max_frames_in_flight", s.maxFramesInFlight);
    s.penCurvePercent = readSetting(L"pen_pressure_curve", s.penCurvePercent);
    s.penMinimumPercent = readSetting(L"pen_pressure_minimum", s.penMinimumPercent);
    return s;
}

// The app's version code comes from the protocol version and app revision, so a mismatch always gets replaced.
bool appIsCurrent(Adb& adb, const std::string& serial) {
    std::string output;
    adb.shell(serial, std::string("dumpsys package ") + proto::kAppId + " | grep -m1 versionCode", &output);
    const size_t at = output.find("versionCode=");
    return at != std::string::npos && static_cast<uint32_t>(strtoul(output.c_str() + at + 12, nullptr, 10)) == proto::kAppVersionCode;
}

// What to tell the user about an app that won't install, in the words of the usual causes.
std::wstring installProblem(const std::string& reason) {
    if (reason.find("USER_RESTRICTED") != std::string::npos || reason.find("canceled by user") != std::string::npos) {
        return L"The tablet blocked the app. Allow installing over USB in its Developer options, then plug it in again.";
    }
    if (reason.find("VERIFICATION") != std::string::npos) return L"Google Play Protect blocked the app. Allow it on the tablet, then plug it in again.";
    if (reason.find("UPDATE_INCOMPATIBLE") != std::string::npos) return L"A different copy of the app is on the tablet. Uninstall it from the tablet, then plug it in again.";
    if (reason.find("INSUFFICIENT_STORAGE") != std::string::npos) return L"The tablet is out of storage space. Free some up, then plug it in again.";
    return L"Could not install the tablet app. See the log.";
}

bool serviceIsRunning(const wchar_t* name) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;
    bool running = false;
    if (SC_HANDLE service = OpenServiceW(manager, name, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        running = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return running;
}

}  // namespace

Service::Service(StatusCallback onStatus) : onStatus_(std::move(onStatus)) {
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Service::~Service() {
    stop();
    CloseHandle(wake_);
}

void Service::start() {
    thread_ = std::thread([this] { loop(); });
}

void Service::stop() {
    stop_ = true;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
}

void Service::setStatus(LinkState state, const std::wstring& text) {
    if (text == lastStatus_) return;
    lastStatus_ = text;
    logLine("status: %s", narrow(text).c_str());
    onStatus_(state, text);
}

void Service::loop() {
    Adb adb(adbPath(), kAdbServerPort);
    VirtualDisplay display;

    bool devicePresent = false;
    bool mayLaunchApp = true;
    int launchesLeft = kLaunchTries;
    uint64_t lastLaunchUs = 0;
    uint64_t lastAppCheckUs = 0;
    int reinstallFailures = 0;
    std::wstring tabletProblem;  // Something only the user can fix, shown until the tablet is unplugged.
    // The display can already be on the desktop, say after a crash or a fresh driver install.
    // With no tablet showing it, it would swallow windows, so it gets the same grace period.
    bool displayActive = display.find() && display.isActive();
    uint64_t displayIdleSinceUs = nowUs();
    uint64_t unseenSinceUs = 0;  // When the USB interface appeared with adb not listing the tablet; 0 while it hasn't.
    uint64_t lastServerRestartUs = 0;

    const auto wait = [this](DWORD ms) { WaitForSingleObject(wake_, ms); };
    const auto releaseDisplayIfIdle = [&] {
        // Windows can put the display back by itself, for example when the laptop wakes from sleep.
        // Nothing is showing it, so it goes again after only a moment for the tablet to reappear.
        if (!displayActive && display.find() && display.isActive()) {
            logLine("display: Windows put the display back with no tablet connected");
            displayActive = true;
            displayIdleSinceUs = nowUs() - kDisplayGraceUs + kReturnedDisplayGraceUs;
        }
        if (displayActive && nowUs() - displayIdleSinceUs > kDisplayGraceUs) {
            display.deactivate();
            displayActive = false;
        }
    };

    while (!stop_) {
        if (!adb.ensureServer()) {
            setStatus(LinkState::Problem, L"adb could not start");
            wait(2000);
            continue;
        }
        std::vector<AdbDevice> devices;
        adb.listUsbDevices(devices);

        const AdbDevice* ready = nullptr;
        const AdbDevice* unauthorized = nullptr;
        for (const AdbDevice& device : devices) {
            if (device.state == "device" && !ready) ready = &device;
            if (device.state == "unauthorized") unauthorized = &device;
        }
        if (!ready) {
            devicePresent = false;
            mayLaunchApp = true;
            launchesLeft = kLaunchTries;
            tabletProblem.clear();
            reinstallFailures = 0;
            releaseDisplayIfIdle();
            // adb can lose track of a device after its connection resets, and then never see it
            // again though it's still plugged in. A fresh server looks at USB from scratch.
            const bool interfacePresent = !unauthorized && Adb::usbInterfacePresent();
            if (!interfacePresent) {
                unseenSinceUs = 0;
            } else if (!unseenSinceUs) {
                unseenSinceUs = nowUs();
            }
            const bool unseen = interfacePresent && nowUs() - unseenSinceUs > kUnseenDeviceUs;
            if (unauthorized) {
                setStatus(LinkState::Problem, L"Tap \"Allow\" on the tablet to trust this laptop");
            } else if (unseen && serviceIsRunning(L"SuperDisplay")) {
                setStatus(LinkState::Problem, L"SuperDisplay is running and holding the USB connection. Stop it to use TDisplay.");
            } else if (unseen) {
                setStatus(LinkState::Problem, L"The tablet is connected, but another program's adb may be holding it");
                if (nowUs() - lastServerRestartUs > kServerRestartGapUs) {
                    lastServerRestartUs = nowUs();
                    adb.killServer();
                }
            } else {
                setStatus(LinkState::Waiting, L"Waiting for the tablet. Connect the USB cable and turn on USB debugging.");
            }
            wait(300);
            continue;
        }
        if (!devicePresent) {
            devicePresent = true;
            unseenSinceUs = 0;
            mayLaunchApp = true;
            launchesLeft = kLaunchTries;
            logLine("adb: found %s (%s)", ready->serial.c_str(), ready->model.c_str());
            std::string sdk;
            adb.shell(ready->serial, "getprop ro.build.version.sdk", &sdk);
            if (atoi(sdk.c_str()) > 0 && atoi(sdk.c_str()) < kOldestAndroidSdk) {
                logLine("adb: the tablet runs Android SDK %d", atoi(sdk.c_str()));
                tabletProblem = L"This tablet's Android is too old. TDisplay needs Android 11 or newer.";
            } else if (!appIsCurrent(adb, ready->serial)) {
                const std::wstring apk = exeDirectory() + L"\\TDisplay.apk";
                setStatus(LinkState::Waiting, L"Installing the app on the tablet");
                std::string reason;
                if (!adb.install(ready->serial, apk, &reason)) tabletProblem = installProblem(reason);
            }
            lastAppCheckUs = nowUs();
        }
        if (!tabletProblem.empty()) {
            releaseDisplayIfIdle();
            setStatus(LinkState::Problem, tabletProblem);
            wait(500);
            continue;
        }
        const std::string serial = ready->serial;

        std::string error;
        SOCKET socket = adb.openAbstract(serial, proto::kSocketName, &error);
        if (socket == INVALID_SOCKET) {
            if (nowUs() - lastAppCheckUs > kAppCheckUs) {
                lastAppCheckUs = nowUs();
                if (!appIsCurrent(adb, serial)) {
                    logLine("adb: the tablet app is missing or out of date, installing it again");
                    const std::wstring apk = exeDirectory() + L"\\TDisplay.apk";
                    setStatus(LinkState::Waiting, L"Installing the app on the tablet");
                    std::string reason;
                    if (!adb.install(serial, apk, &reason)) {
                        const std::wstring problem = installProblem(reason);
                        if (++reinstallFailures >= kReinstallTries || problem != installProblem(std::string())) {
                            tabletProblem = problem;
                        } else {
                            setStatus(LinkState::Waiting, L"Installing the app on the tablet, trying again");
                        }
                        continue;
                    }
                    reinstallFailures = 0;
                    // Like a replug: open it, even if it had been closed on purpose before.
                    mayLaunchApp = true;
                    launchesLeft = kLaunchTries;
                    lastLaunchUs = 0;
                }
            }
            if (mayLaunchApp && launchesLeft > 0 && nowUs() - lastLaunchUs > kLaunchRetryUs) {
                logLine("adb: starting the tablet app%s", launchesLeft < kLaunchTries ? " again" : "");
                --launchesLeft;
                lastLaunchUs = nowUs();
                adb.shell(serial, kLaunchCommand);
            }
            releaseDisplayIfIdle();
            setStatus(LinkState::Waiting, L"Waiting for the TDisplay app on the tablet");
            wait(250);
            continue;
        }

        setStatus(LinkState::Waiting, L"Connecting");
        displayActive = true;
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        const uint64_t started = nowUs();
        std::wstring problem;
        const StreamEnd end = runStream(socket, adb, serial, display, loadSettings(), stop_, [this](const std::wstring& line) { setStatus(LinkState::Streaming, line); }, &problem);
        SetThreadExecutionState(ES_CONTINUOUS);
        displayIdleSinceUs = nowUs();

        const double seconds = (nowUs() - started) / 1e6;
        switch (end) {
            case StreamEnd::AppHidden:
                // The user stepped away on purpose; don't pull the app back up.
                logLine("session: ended by the tablet after %.1f s", seconds);
                mayLaunchApp = false;
                setStatus(LinkState::Waiting, L"Tablet app closed. Open TDisplay PC on the tablet to reconnect.");
                break;
            case StreamEnd::ConnectionLost:
                logLine("session: connection lost after %.1f s, reconnecting", seconds);
                mayLaunchApp = true;
                launchesLeft = kLaunchTries;
                setStatus(LinkState::Waiting, L"Reconnecting");
                break;
            case StreamEnd::SetupFailed:
                logLine("session: setup failed after %.1f s", seconds);
                setStatus(LinkState::Problem, problem.empty() ? L"Could not start. See the log." : problem);
                wait(1500);
                break;
            case StreamEnd::NotReady:
                wait(250);
                break;
            case StreamEnd::Stopped:
                break;
        }
    }

    if (displayActive) display.deactivate();
}
