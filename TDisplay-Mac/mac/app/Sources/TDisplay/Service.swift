// The background loop, ported from host/src/service.cpp: waits for the tablet on USB, runs sessions, and reconnects.
import CoreGraphics
import Foundation
import IOKit.pwr_mgt

enum LinkState {
    case waiting, streaming, problem
}

final class Service: @unchecked Sendable {
    // Private so that other software's adb can't interfere with ours.
    private static let adbServerPort = 15037
    // A replug or app restart inside this window leaves windows on the tablet where they are.
    private static let displayGraceUs: UInt64 = 20_000_000
    // How long a plugged-in tablet can go missing from adb before restarting the server, and how often to retry that if
    // something else is holding the USB connection. The time counts from when the tablet's USB connection appears, so a
    // tablet plugged back in gets the same time as any other for adb to find it.
    private static let unseenDeviceUs: UInt64 = 1_500_000
    private static let serverRestartGapUs: UInt64 = 15_000_000
    private static let launchCommand = "am start -n com.tdisplay/.DisplayActivity"
    private static let oldestAndroidSdk = 30  // Android 11.

    private let onStatus: (LinkState, String) -> Void
    private let stopping = Shared<Bool>(false)
    private let asleep = Shared<Bool>(false)
    private let rearrange = Shared<Bool>(false)
    private let wake = Event()
    private let finished = DispatchSemaphore(value: 0)
    private var lastStatus = ""

    init(onStatus: @escaping (LinkState, String) -> Void) {
        self.onStatus = onStatus
    }

    func start() {
        let thread = Thread { [self] in
            loop()
            finished.signal()
        }
        thread.qualityOfService = .userInteractive
        thread.name = "tdisplay-service"
        thread.start()
    }

    func stop() {
        stopping.put(true)
        wake.set()
        _ = finished.wait(timeout: .now() + 5)
    }

    // The Mac is about to sleep. The session ends now rather than finding out after waking that the cable went quiet,
    // and the display stays for the tablet to come back to.
    func sleep() {
        logLine("power: the Mac is going to sleep")
        asleep.put(true)
        wake.set()
    }

    // Called for the system waking and for the screens waking, whichever comes first. A dark wake, with the lid closed
    // or the screens off, is neither, and the session stays ended through it.
    func woke() {
        guard asleep.exchange(false) else { return }
        logLine("power: the Mac woke up")
        wake.set()
    }

    // A new choice in the menu. A session in progress ends and starts again at once in the new arrangement.
    func arrangementChanged() {
        logLine("arrangement: \(Arrangement.current.title)")
        rearrange.put(true)
        wake.set()
    }

    private func setStatus(_ state: LinkState, _ text: String) {
        guard text != lastStatus else { return }
        lastStatus = text
        logLine("status: \(text)")
        onStatus(state, text)
    }

    // The app's version code is the protocol version, so a mismatch always gets replaced; so does the build from before
    // touches, which has the same version code and an older version name.
    private func appIsCurrent(_ adb: Adb, _ serial: String) -> Bool {
        guard let output = adb.shell(serial: serial, "dumpsys package com.tdisplay | grep -m2 -E 'versionCode|versionName'"),
              let code = output.range(of: "versionCode="), let name = output.range(of: "versionName=") else { return false }
        let versionName = output[name.upperBound...].prefix { !$0.isWhitespace }
        return UInt32(output[code.upperBound...].prefix { $0.isNumber }) == Proto.version && versionName == Proto.appVersionName
    }

    // What to tell the user about an app that won't install, in the words of the usual causes.
    private func installProblem(_ reason: String) -> String {
        if reason.contains("USER_RESTRICTED") || reason.contains("canceled by user") {
            return "The tablet blocked the app. Allow installing over USB in its Developer options, then plug it in again."
        }
        if reason.contains("VERIFICATION") { return "Google Play Protect blocked the app. Allow it on the tablet, then plug it in again." }
        if reason.contains("UPDATE_INCOMPATIBLE") { return "A different copy of the app is on the tablet. Uninstall it from the tablet, then plug it in again." }
        if reason.contains("INSUFFICIENT_STORAGE") { return "The tablet is out of storage space. Free some up, then plug it in again." }
        return "Could not install the tablet app. See the log."
    }

    private func loop() {
        let adb = Adb(exe: Paths.adb, port: Service.adbServerPort)
        let display = VirtualDisplay()
        var devicePresent = false
        var mayLaunchApp = true
        var tabletProblem = ""  // Something only the user can fix, shown until the tablet is unplugged.
        // What to say while the tablet app isn't answering. After the user left it, the useful thing is how to come back.
        var waitingForApp = "Waiting for the TDisplay app on the tablet"
        var displayIdleSinceUs = nowUs()
        var unseenSinceUs: UInt64 = 0  // When the tablet's USB connection appeared with adb not listing it; 0 while it hasn't.
        var lastServerRestartUs: UInt64 = 0
        var askedForCapture = false
        let stop = { [self] in stopping.get() }
        let stopSession = { [self] in stopping.get() || asleep.get() || rearrange.get() }
        let wait = { [self] (ms: Int) in _ = wake.wait(ms: ms) }
        let releaseDisplayIfIdle = {
            if display.isActive, nowUs() - displayIdleSinceUs > Service.displayGraceUs { display.deactivate() }
        }

        while !stop() {
            if asleep.get() {
                setStatus(.waiting, "The Mac is asleep")
                wait(1000)
                continue
            }
            // With no session to restart, a display kept for the grace period goes or moves straight away.
            if rearrange.exchange(false) {
                if Arrangement.current == .mirror { display.deactivate() } else { display.place(Arrangement.current) }
            }
            guard adb.ensureServer() else {
                setStatus(.problem, "adb could not start")
                wait(2000)
                continue
            }
            let devices = adb.listUsbDevices() ?? []
            let ready = devices.first { $0.state == "device" }
            let unauthorized = devices.contains { $0.state == "unauthorized" }
            guard let ready else {
                devicePresent = false
                mayLaunchApp = true
                tabletProblem = ""
                waitingForApp = "Waiting for the TDisplay app on the tablet"
                releaseDisplayIfIdle()
                // adb can lose track of a device after its connection resets, and then never see it again though it's
                // still plugged in. A fresh server looks at USB from scratch.
                let interfacePresent = !unauthorized && Adb.usbInterfacePresent()
                if !interfacePresent {
                    unseenSinceUs = 0
                } else if unseenSinceUs == 0 {
                    unseenSinceUs = nowUs()
                }
                let unseen = interfacePresent && nowUs() - unseenSinceUs > Service.unseenDeviceUs
                if unauthorized {
                    setStatus(.problem, "Tap \"Allow\" on the tablet to trust this Mac")
                } else if unseen {
                    setStatus(.problem, "The tablet is connected, but another program's adb may be holding it")
                    if nowUs() - lastServerRestartUs > Service.serverRestartGapUs {
                        lastServerRestartUs = nowUs()
                        adb.killServer()
                    }
                } else {
                    setStatus(.waiting, "Waiting for the tablet. Connect the USB cable and turn on USB debugging.")
                }
                wait(300)
                continue
            }
            if !devicePresent {
                devicePresent = true
                unseenSinceUs = 0
                mayLaunchApp = true
                logLine("adb: found \(ready.serial) (\(ready.model))")
                let sdk = Int(adb.shell(serial: ready.serial, "getprop ro.build.version.sdk")?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "") ?? 0
                if sdk > 0, sdk < Service.oldestAndroidSdk {
                    logLine("adb: the tablet runs Android SDK \(sdk)")
                    tabletProblem = "This tablet's Android is too old. TDisplay needs Android 11 or newer."
                } else if !appIsCurrent(adb, ready.serial) {
                    setStatus(.waiting, "Installing the app on the tablet")
                    var reason = ""
                    var installed = false
                    for _ in 0..<3 {
                        installed = adb.install(serial: ready.serial, apk: Paths.apk, reason: &reason)
                        // A busy tablet's package service can be missing for a moment, as it was right after an uninstall.
                        guard !installed, reason.contains("Can't find service") else { break }
                        sleepMs(2000)
                    }
                    if !installed { tabletProblem = installProblem(reason) }
                }
            }
            if !tabletProblem.isEmpty {
                releaseDisplayIfIdle()
                setStatus(.problem, tabletProblem)
                wait(500)
                continue
            }
            // Without Screen Recording there's nothing to show, and a display made anyway would only swallow windows that
            // nobody can see. macOS is asked once, which also lists TDisplay in System Settings.
            if !CGPreflightScreenCaptureAccess() {
                if !askedForCapture {
                    askedForCapture = true
                    logLine("capture: no Screen Recording permission")
                    CGRequestScreenCaptureAccess()
                }
                releaseDisplayIfIdle()
                setStatus(.problem, "Allow TDisplay under System Settings > Privacy & Security > Screen & System Audio Recording. If macOS offers to quit and reopen it, do.")
                wait(1000)
                continue
            }
            let serial = ready.serial

            var error = ""
            guard let socket = adb.openAbstract(serial: serial, name: Proto.socketName, error: &error) else {
                if mayLaunchApp {
                    mayLaunchApp = false
                    logLine("adb: starting the tablet app")
                    adb.shell(serial: serial, Service.launchCommand)
                }
                releaseDisplayIfIdle()
                setStatus(.waiting, waitingForApp)
                wait(250)
                continue
            }

            setStatus(.waiting, "Connecting")
            // Keeps the Mac's screens awake, as they would be for a video, while the tablet shows one of them.
            var assertion: IOPMAssertionID = 0
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn),
                                        "TDisplay is showing a screen on the tablet" as CFString, &assertion)
            let started = nowUs()
            let session = Session(socket: socket, adb: adb, serial: serial, display: display, settings: StreamSettings.load(), stop: stopSession) { [self] line in
                setStatus(.streaming, line)
            }
            let end = session.run()
            IOPMAssertionRelease(assertion)
            displayIdleSinceUs = nowUs()

            let seconds = String(format: "%.1f", Double(nowUs() - started) / 1e6)
            switch end {
            case .appHidden:
                // The user stepped away on purpose; don't pull the app back up.
                logLine("session: ended by the tablet after \(seconds) s")
                mayLaunchApp = false
                waitingForApp = "Tablet app closed. Open TDisplay on the tablet to reconnect."
                setStatus(.waiting, waitingForApp)
            case .connectionLost:
                logLine("session: connection lost after \(seconds) s, reconnecting")
                mayLaunchApp = true
                waitingForApp = "Waiting for the TDisplay app on the tablet"
                setStatus(.waiting, "Reconnecting")
            case .setupFailed:
                logLine("session: setup failed after \(seconds) s")
                setStatus(.problem, session.problem.isEmpty ? "Could not start. See the log." : session.problem)
                wait(1500)
            case .notReady:
                wait(250)
            case .stopped:
                // Quitting, the Mac going to sleep or a new arrangement; the loop's next turn sees which.
                break
            }
        }

        display.deactivate()
    }
}
