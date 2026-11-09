import AppKit
import VirtualDisplayShim

// tdisplay-feasibility gestures seconds
// Opens a window and prints every gesture, magnify, rotate and scroll event it gets from the trackpad, with every
// non-zero field of the underlying CGEvent, so synthetic ones can be built the same way.
final class GestureDump: NSObject {
    let seconds: Double
    private var window: NSWindow?
    private var monitor: Any?
    private var global: Any?
    private var last = ""

    init(seconds: Double) { self.seconds = seconds }

    func start() {
        let screen = NSScreen.main!
        let frame = NSRect(x: screen.frame.midX - 400, y: screen.frame.midY - 250, width: 800, height: 500)
        let window = NSWindow(contentRect: frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.title = "Pinch, rotate and scroll with two fingers here"
        window.contentView = NSView()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
        let types: NSEvent.EventTypeMask = [.gesture, .magnify, .rotate, .swipe, .smartMagnify, .beginGesture, .endGesture, .scrollWheel]
        monitor = NSEvent.addLocalMonitorForEvents(matching: types) { [weak self] event in
            self?.dump(event)
            return event
        }
        // Gestures meant for any other app too, so it doesn't matter which window is under the pointer.
        global = NSEvent.addGlobalMonitorForEvents(matching: types) { [weak self] event in self?.dump(event) }
        log("recording for \(Int(seconds)) s")
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { exit(0) }
    }

    private func dump(_ event: NSEvent) {
        guard let cg = event.cgEvent else { return }
        var fields: [String] = []
        for field in UInt32(0)..<256 {
            let integer = TDEventInteger(cg, field)
            let double = TDEventDouble(cg, field)
            if integer == 0, double == 0 { continue }
            fields.append(Double(integer) == double ? "\(field)=\(integer)" : "\(field)=\(integer)/\(String(format: "%.4f", double))")
        }
        var summary = "type \(event.type.rawValue) cg \(cg.type.rawValue) phase \(event.phase.rawValue) momentum \(event.momentumPhase.rawValue)"
        switch event.type {
        case .magnify: summary += String(format: " magnification %.4f", event.magnification)
        case .rotate: summary += String(format: " rotation %.3f", event.rotation)
        case .scrollWheel: summary += String(format: " scroll %.2f,%.2f precise %d", event.scrollingDeltaX, event.scrollingDeltaY, event.hasPreciseScrollingDeltas ? 1 : 0)
        default: break
        }
        let line = summary + " | " + fields.joined(separator: " ")
        // Field 58 is a timestamp-like counter; collapse lines that differ only there.
        if line != last { print(line) }
        last = line
        fflush(stdout)
    }
}
