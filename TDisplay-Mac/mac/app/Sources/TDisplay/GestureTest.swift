// TDisplay --gesture-test: like --pen-test, for gestures. Opens a window in the middle of the main screen, makes two
// scripted fingers scroll, pinch and twist over it with the real GestureInjector, and prints what AppKit hands the
// program, so the events can be checked without the tablet. Needs the same Accessibility permission as the pen.
import AppKit
import VirtualDisplayShim

final class GestureTest: NSObject {
    private var window: NSWindow?
    private var monitor: Any?
    private var screen = CGRect.zero
    private lazy var gestures = GestureInjector { [screen] x, y in
        CGPoint(x: screen.minX + CGFloat(x) * screen.width, y: screen.minY + CGFloat(y) * screen.height)
    }

    func start() {
        guard PenInjector.allowed else {
            print("no Accessibility permission: allow TDisplay under System Settings > Privacy & Security > Accessibility, then run it again")
            exit(3)
        }
        let main = NSScreen.main!
        let frame = NSRect(x: main.frame.midX - 400, y: main.frame.midY - 250, width: 800, height: 500)
        let window = NSWindow(contentRect: frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.title = "TDisplay gesture test"
        window.level = .floating
        window.contentView = NSView()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
        screen = CGDisplayBounds(CGMainDisplayID())
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.scrollWheel, .magnify, .rotate, .gesture, .beginGesture, .endGesture]) { event in
            GestureTest.describe(event)
            return event
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { Thread { self.script() }.start() }
    }

    private static func describe(_ event: NSEvent) {
        var text = "\(event.type) phase \(event.phase.rawValue)"
        switch event.type {
        case .scrollWheel: text += String(format: " scroll %.1f,%.1f precise %d", event.scrollingDeltaX, event.scrollingDeltaY, event.hasPreciseScrollingDeltas ? 1 : 0)
        case .magnify: text += String(format: " magnification %.4f", event.magnification)
        case .rotate: text += String(format: " rotation %.2f", event.rotation)
        default: break
        }
        if let cg = event.cgEvent, cg.type.rawValue == 29 {
            text += String(format: " | kind %lld value %.4f phase %lld", TDEventInteger(cg, 110), TDEventDouble(cg, 113), TDEventInteger(cg, 132))
        }
        text += String(format: " at %.0f,%.0f", event.locationInWindow.x, event.locationInWindow.y)
        print(text)
        fflush(stdout)
    }

    // Two fingers either side of (x, y) on the main screen, in points from its top left, `apart` points apart at `angle`.
    private func touch(_ x: Double, _ y: Double, apart: Double, angle: Double) {
        let dx = cos(angle * .pi / 180) * apart / 2
        let dy = sin(angle * .pi / 180) * apart / 2
        var bytes = Writer()
        bytes.u32(2)
        for (id, px, py) in [(UInt32(0), x - dx, y - dy), (UInt32(1), x + dx, y + dy)] {
            bytes.u32(id)
            bytes.u32(Float(px / screen.width).bitPattern)
            bytes.u32(Float(py / screen.height).bitPattern)
        }
        gestures.apply(TouchFrame(bytes.bytes)!)
        usleep(8000)
    }

    // Two fingers at these points on the main screen.
    private func touchTwo(_ x1: Double, _ y1: Double, _ x2: Double, _ y2: Double) {
        var bytes = Writer()
        bytes.u32(2)
        for (id, px, py) in [(UInt32(0), x1, y1), (UInt32(1), x2, y2)] {
            bytes.u32(id)
            bytes.u32(Float(px / screen.width).bitPattern)
            bytes.u32(Float(py / screen.height).bitPattern)
        }
        gestures.apply(TouchFrame(bytes.bytes)!)
        usleep(8000)
    }

    private func script() {
        let x = screen.midX
        let y = screen.midY
        print("-- two fingers move 100 points right and 60 down: scrolling")
        for i in 0...20 { touch(x + Double(i) * 5, y + Double(i) * 3, apart: 120, angle: 0) }
        gestures.end()
        usleep(200_000)
        print("-- fingers spread from 120 to 240 points apart: zooming in")
        for i in 0...20 { touch(x, y, apart: 120 + Double(i) * 6, angle: 0) }
        gestures.end()
        usleep(200_000)
        print("-- fingers turn 40 degrees clockwise on the screen")
        for i in 0...20 { touch(x, y, apart: 160, angle: Double(i) * 2) }
        gestures.end()
        usleep(200_000)
        print("-- one finger still, the other moves 100 points away from it: zooming in")
        for i in 0...20 { touchTwo(x - 60, y, x + 60 + Double(i) * 5, y) }
        gestures.end()
        usleep(200_000)
        print("-- one finger still, the other turns 40 degrees around it, clockwise on the screen")
        for i in 0...20 {
            let a = Double(i) * 2 * .pi / 180
            touchTwo(x - 80, y, x - 80 + 160 * cos(a), y + 160 * sin(a))
        }
        gestures.end()
        usleep(300_000)
        usleep(200_000)
        print("-- fingers turn 30 degrees anticlockwise while drifting 25 points down: still rotating")
        for i in 0...20 { touch(x, y + Double(i) * 1.25, apart: 200, angle: -Double(i) * 1.5) }
        gestures.end()
        usleep(300_000)
        print("-- done")
        DispatchQueue.main.async { exit(0) }
    }
}
