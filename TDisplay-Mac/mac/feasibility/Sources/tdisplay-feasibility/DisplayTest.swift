import AppKit
import CoreVideo

// tdisplay-feasibility display hz [pixels|points|both] [seconds]
// Makes the tablet-sized virtual display, reports the modes macOS gives it and the rate its vsync really runs at, then
// removes it and times how long that takes.
func runDisplayTest(hz: Double, style: ModeStyle, seconds: Double) throws {
    let start = now()
    let display = try makeVirtualDisplay(width: Tablet.width, height: Tablet.height, hz: hz, style: style, productID: testProductID(style))
    let id = display.displayID
    log(String(format: "display %u made in %.1f ms, %@ modes at %.0f Hz", id, (now() - start) * 1000, style.rawValue, hz))
    guard let online = waitForDisplay(id, online: true) else { throw FeasibilityError("display \(id) never came online") }
    log(String(format: "online after %.1f ms", online * 1000))
    // The mode settles a moment after the display appears.
    usleep(500_000)
    log(describeModesInChild(id), terminator: "")

    let vsync = measureVsync(id, seconds: seconds)
    log(vsync)

    let removeStart = now()
    display.remove()
    if let gone = waitForDisplay(id, online: false) {
        log(String(format: "removed after %.1f ms", gone * 1000))
    } else {
        log(String(format: "still there %.1f s after removal", now() - removeStart))
    }
}

private func log(_ text: String, terminator: String) {
    print(text, terminator: terminator)
    fflush(stdout)
}

// Vsync times from a CVDisplayLink on the display, which follows its real refresh rather than the mode's label.
private func measureVsync(_ id: CGDirectDisplayID, seconds: Double) -> String {
    var link: CVDisplayLink?
    guard CVDisplayLinkCreateWithCGDisplay(id, &link) == kCVReturnSuccess, let link else { return "vsync: no display link" }
    let lock = NSLock()
    var times: [Double] = []
    CVDisplayLinkSetOutputHandler(link) { _, _, outputTime, _, _ in
        let t = machSeconds(outputTime.pointee.hostTime)
        lock.lock()
        times.append(t)
        lock.unlock()
        return kCVReturnSuccess
    }
    CVDisplayLinkStart(link)
    Thread.sleep(forTimeInterval: seconds)
    CVDisplayLinkStop(link)
    let nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link)
    let actual = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link)
    lock.lock()
    let gaps = zip(times.dropFirst(), times).map { ($0 - $1) * 1000 }
    lock.unlock()
    let nominalHz = nominal.timeValue > 0 ? Double(nominal.timeScale) / Double(nominal.timeValue) : 0
    let median = gaps.sorted().dropFirst(gaps.count / 2).first ?? 0
    return String(format: "vsync: nominal %.2f Hz, actual %.2f Hz, measured %.2f Hz over %d ticks; gap %@",
                  nominalHz, actual > 0 ? 1 / actual : 0, median > 0 ? 1000 / median : 0, gaps.count, percentiles(gaps))
}

// tdisplay-feasibility modes display-id
// Run by the display test in a process of its own.
func runModes(_ id: CGDirectDisplayID) {
    if let current = CGDisplayCopyDisplayMode(id) {
        print("current mode: \(describe(current))")
    } else {
        print("current mode: none")
    }
    let options = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
    let modes = (CGDisplayCopyAllDisplayModes(id, options) as? [CGDisplayMode]) ?? []
    for mode in modes { print("  offers \(describe(mode))") }
    let bounds = CGDisplayBounds(id)
    let size = CGDisplayScreenSize(id)
    print(String(format: "bounds %.0f,%.0f %.0fx%.0f points, %.0fx%.0f mm", bounds.origin.x, bounds.origin.y, bounds.width, bounds.height, size.width, size.height))
    if let screen = NSScreen.screens.first(where: { ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == id }) {
        print(String(format: "NSScreen \"%@\": %.0fx%.0f points at %.0fx, up to %d fps, refresh interval %.3f-%.3f ms",
                     screen.localizedName, screen.frame.width, screen.frame.height, screen.backingScaleFactor, screen.maximumFramesPerSecond,
                     screen.minimumRefreshInterval * 1000, screen.maximumRefreshInterval * 1000))
    } else {
        print("NSScreen: not found")
    }
}
