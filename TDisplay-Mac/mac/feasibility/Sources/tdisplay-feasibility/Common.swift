import CoreGraphics
import Foundation
import QuartzCore
import VirtualDisplayShim

// The tablet everything is measured for: the Lenovo Idea Tab Pro, 2944x1840 on 12.7 inches.
enum Tablet {
    static let width = 2944
    static let height = 1840
    static let diagonalInches = 12.7
}

// How the modes are handed to the WindowServer. pixels gives the full 2944x1840, points half of it, and both gives
// both, so it shows which one macOS treats as HiDPI and which it starts in.
enum ModeStyle: String, CaseIterable {
    case pixels, points, both
}

struct FeasibilityError: Error, CustomStringConvertible {
    let description: String
    init(_ description: String) { self.description = description }
}

func now() -> Double { CACurrentMediaTime() }

private let timebase: mach_timebase_info_data_t = {
    var info = mach_timebase_info_data_t()
    mach_timebase_info(&info)
    return info
}()

func machSeconds(_ ticks: UInt64) -> Double { Double(ticks) * Double(timebase.numer) / Double(timebase.denom) / 1e9 }

func machTicks(_ seconds: Double) -> UInt64 { UInt64(seconds * 1e9 * Double(timebase.denom) / Double(timebase.numer)) }

// Sleeps until a CACurrentMediaTime() moment, which is on the same clock as mach_absolute_time().
func wait(until moment: Double) {
    let target = machTicks(moment)
    if target > mach_absolute_time() { mach_wait_until(target) }
}

func percentiles(_ values: [Double], unit: String = "ms") -> String {
    guard !values.isEmpty else { return "none" }
    let sorted = values.sorted()
    let at = { (p: Double) in sorted[Int(p * Double(sorted.count - 1))] }
    return String(format: "median %.2f, p95 %.2f, p99 %.2f, max %.2f %@ (n=%d)", at(0.5), at(0.95), at(0.99), at(1), unit, sorted.count)
}

func log(_ text: String) {
    print(text)
    fflush(stdout)
}

func onlineDisplays() -> [CGDirectDisplayID] {
    var count: UInt32 = 0
    CGGetOnlineDisplayList(0, nil, &count)
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    CGGetOnlineDisplayList(count, &ids, &count)
    return Array(ids.prefix(Int(count)))
}

// The serial number comes from the size, so a tablet of another size gets an identity, and remembered mode, of its
// own. The test runs pass their own product IDs so they don't disturb the identity the real program will use.
func makeVirtualDisplay(width: Int, height: Int, hz: Double, style: ModeStyle, productID: UInt32) throws -> TDVirtualDisplay {
    let diagonalPixels = Double(width * width + height * height).squareRoot()
    let mmPerPixel = Tablet.diagonalInches * 25.4 / diagonalPixels
    let full: [NSNumber] = [NSNumber(value: width), NSNumber(value: height), NSNumber(value: hz)]
    let half: [NSNumber] = [NSNumber(value: width / 2), NSNumber(value: height / 2), NSNumber(value: hz)]
    let modes: [[NSNumber]] = style == .pixels ? [full] : style == .points ? [half] : [full, half]
    return try TDVirtualDisplay(
        name: "TDisplay", maxPixelsWide: UInt32(width), maxPixelsHigh: UInt32(height),
        sizeInMillimeters: CGSize(width: Double(width) * mmPerPixel, height: Double(height) * mmPerPixel),
        vendorID: 0x5444, productID: productID, serialNumber: UInt32(width) << 16 | UInt32(height),
        hiDPI: true, modes: modes)
}

func testProductID(_ style: ModeStyle) -> UInt32 { 0x0F00 + UInt32(ModeStyle.allCases.firstIndex(of: style)!) }

// Waits for the display to come or go. Returns the seconds it took, or nil if it didn't happen in time.
func waitForDisplay(_ id: CGDirectDisplayID, online: Bool, timeout: Double = 10) -> Double? {
    let start = now()
    while now() - start < timeout {
        if onlineDisplays().contains(id) == online { return now() - start }
        usleep(5000)
    }
    return nil
}

// Asks a new copy of this program for the display's modes. The process that made a display can't see its modes once
// it has listed the displays, according to the go-macos/virtualdisplay notes, and another process always can.
func describeModesInChild(_ id: CGDirectDisplayID) -> String {
    let process = Process()
    process.executableURL = Bundle.main.executableURL
    process.arguments = ["modes", String(id)]
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    do {
        try process.run()
    } catch {
        return "could not start the mode reader: \(error)"
    }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    return String(decoding: data, as: UTF8.self)
}

func describe(_ mode: CGDisplayMode) -> String {
    let scale = mode.width > 0 ? Double(mode.pixelWidth) / Double(mode.width) : 0
    return String(format: "%dx%d points, %dx%d pixels (%.0fx), %.3f Hz%@", mode.width, mode.height, mode.pixelWidth, mode.pixelHeight, scale,
                  mode.refreshRate, mode.isUsableForDesktopGUI() ? "" : ", not for the desktop")
}
