// The virtual monitor, made with CoreGraphics' undocumented CGVirtualDisplay classes (the counterpart of the Virtual
// Display Driver on Windows). It lasts between sessions, so a replug within the grace period finds it as it was.
import CoreGraphics
import Foundation
import VirtualDisplayShim

final class VirtualDisplay {
    private var display: TDVirtualDisplay?
    private(set) var width = 0
    private(set) var height = 0
    private(set) var hz = 0

    var isActive: Bool { display != nil }
    var displayID: CGDirectDisplayID { display?.displayID ?? kCGNullDirectDisplay }

    // Makes the display at this size and rate, or keeps the one there if it matches. A display's mode is never changed
    // once it exists: one whose mode changed can't be removed until the process exits.
    func activate(width: Int, height: Int, hz: Int) -> Bool {
        if display != nil, width == self.width, height == self.height, hz == self.hz { return true }
        deactivate()
        // The serial number comes from the size, so a tablet of another size gets an identity, and remembered mode, of
        // its own. Every identity leaves a colour profile behind in /Library/ColorSync/Profiles/Displays.
        let diagonalPixels = Double(width * width + height * height).squareRoot()
        let mmPerPixel = 12.7 * 25.4 / diagonalPixels  // Read as a 12.7" panel, which only matters to macOS's default scaling.
        // With the half size on offer and HiDPI on, macOS starts it at 2x. Given only the full size, it starts at 1x.
        var modes: [[NSNumber]] = []
        if width % 2 == 0, height % 2 == 0 { modes.append([NSNumber(value: width / 2), NSNumber(value: height / 2), NSNumber(value: hz)]) }
        modes.append([NSNumber(value: width), NSNumber(value: height), NSNumber(value: hz)])
        let created: TDVirtualDisplay
        do {
            created = try TDVirtualDisplay(
                name: "TDisplay", maxPixelsWide: UInt32(width), maxPixelsHigh: UInt32(height),
                sizeInMillimeters: CGSize(width: Double(width) * mmPerPixel, height: Double(height) * mmPerPixel),
                vendorID: 0x5444, productID: 1, serialNumber: UInt32(width) << 16 | UInt32(height), hiDPI: true, modes: modes)
        } catch {
            logLine("display: could not make a \(width)x\(height)@\(hz) display: \(error.localizedDescription)")
            return false
        }
        let id = created.displayID
        let start = nowUs()
        while !onlineDisplays().contains(id) {
            if nowUs() - start > 5_000_000 {
                logLine("display: \(id) never came online")
                created.remove()
                return false
            }
            sleepMs(5)
        }
        display = created
        self.width = width
        self.height = height
        self.hz = hz
        logLine(String(format: "display: %u is %dx%d@%d, online after %.0f ms", id, width, height, hz, Double(nowUs() - start) / 1000))
        return true
    }

    // Puts the display on the chosen side of the main screen. A display moved from this process couldn't be removed again
    // until it exited, so a copy of this program does the moving and exits. macOS remembers the place for this display's
    // identity, so next time it comes back there by itself.
    func place(_ arrangement: Arrangement) {
        guard let current = display else { return }
        let id = current.displayID
        let bounds = CGDisplayBounds(id)
        guard let target = arrangement.origin(for: bounds.size), bounds.width > 0 else { return }
        let x = Int(target.x.rounded())
        let y = Int(target.y.rounded())
        guard Int(bounds.minX) != x || Int(bounds.minY) != y else { return }
        let helper = Process()
        helper.executableURL = Bundle.main.executableURL
        helper.arguments = ["--place", String(id), String(x), String(y)]
        do {
            try helper.run()
        } catch {
            logLine("display: could not start the helper to move it (\(error.localizedDescription))")
            return
        }
        helper.waitUntilExit()
        let now = CGDisplayBounds(id)
        logLine(String(format: "display: %u moved %@ the main screen, to %.0f,%.0f", id, arrangement == .left || arrangement == .right ? "\(arrangement.rawValue) of" : "\(arrangement.rawValue)", now.minX, now.minY))
    }

    // Moves a display, for place(): run as `TDisplay --place id x y` in a process of its own.
    static func move(_ id: CGDirectDisplayID, x: Int32, y: Int32) -> Bool {
        var config: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&config) == .success else { return false }
        CGConfigureDisplayOrigin(config, id, x, y)
        return CGCompleteDisplayConfiguration(config, .forSession) == .success
    }

    // Takes the display away and waits until it's gone, since removal finishes after the call returns.
    func deactivate() {
        guard let current = display else { return }
        let id = current.displayID
        display = nil
        current.remove()
        let start = nowUs()
        while onlineDisplays().contains(id), nowUs() - start < 5_000_000 { sleepMs(5) }
        logLine(String(format: "display: %u removed after %.0f ms", id, Double(nowUs() - start) / 1000))
    }

    private func onlineDisplays() -> [CGDirectDisplayID] {
        var count: UInt32 = 0
        CGGetOnlineDisplayList(0, nil, &count)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        CGGetOnlineDisplayList(count, &ids, &count)
        return Array(ids.prefix(Int(count)))
    }
}
