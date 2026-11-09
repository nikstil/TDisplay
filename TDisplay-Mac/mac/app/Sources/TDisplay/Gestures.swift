// Turns two fingers on the tablet into what a Mac trackpad sends: scrolling when they move together, which pans a canvas,
// magnifying when they pinch, and rotating when they twist. As on a trackpad, the first of those to pass its threshold
// owns the gesture until the fingers lift. One finger does nothing, and fingers are ignored while the pen is near, so a
// resting hand doesn't scroll or zoom.
//
// Scrolling is an ordinary continuous scroll event with phases. Magnifying and rotating have no public way in; a real
// trackpad's events, recorded with `tdisplay-feasibility gestures`, are gesture events (type 29) with the kind in field 110
// (8 zoom, 5 rotation), the amount in field 113 and the phase in field 132 (1 began, 2 changed, 4 ended), which is what's
// posted here.
import CoreGraphics
import Foundation
import VirtualDisplayShim

enum TouchGestures {
    private static let key = "touchGestures"

    // On unless switched off in the menu.
    static var enabled: Bool {
        get { UserDefaults.standard.object(forKey: key) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: key) }
    }
}

final class GestureInjector {
    private enum Mode {
        case idle, undecided, scroll, magnify, rotate
    }

    // How far, in Mac points, the fingers must move together, apart or past each other before that becomes the gesture.
    // Twisting also has to turn at least rotateMinimum degrees, since pinches rarely keep their angle exactly.
    private static let scrollStart = 8.0
    private static let magnifyStart = 10.0
    private static let rotateStart = 12.0
    private static let rotateMinimum = 10.0
    private static let debug = ProcessInfo.processInfo.environment["TDISPLAY_DEBUG"] != nil
    // Gesture events: type, and the fields a recorded trackpad gesture uses.
    private static let gestureType: UInt32 = 29
    private static let kindField: UInt32 = 110
    private static let valueField: UInt32 = 113
    private static let phaseField: UInt32 = 132
    private static let zoomKind: Int64 = 8
    private static let rotateKind: Int64 = 5

    private let source = CGEventSource(stateID: .hidSystemState)
    private let map: (Float, Float) -> CGPoint
    private var mode = Mode.idle
    private var pair: (UInt32, UInt32) = (0, 0)
    private var start = (center: CGPoint.zero, distance: 0.0, angle: 0.0)
    private var startFingers = (CGPoint.zero, CGPoint.zero)
    private var last = (center: CGPoint.zero, distance: 0.0, angle: 0.0)
    private var location = CGPoint.zero
    private var carry = (x: 0.0, y: 0.0)  // Scrolling below a whole point, saved for the next event.

    // map turns a point on the tablet into one on the Mac: the pen's mapping, so both land in the same place.
    init(map: @escaping (Float, Float) -> CGPoint) {
        self.map = map
    }

    func apply(_ frame: TouchFrame) {
        guard !frame.canceled, frame.fingers.count == 2 else {
            end()
            return
        }
        let a = frame.fingers[0]
        let b = frame.fingers[1]
        let p = map(a.x, a.y)
        let q = map(b.x, b.y)
        let now = (center: CGPoint(x: (p.x + q.x) / 2, y: (p.y + q.y) / 2), distance: Double(max(hypot(q.x - p.x, q.y - p.y), 1)),
                   angle: Double(atan2(q.y - p.y, q.x - p.x)))
        if mode == .idle || pair != (a.id, b.id) {
            end()
            mode = .undecided
            pair = (a.id, b.id)
            start = now
            startFingers = (p, q)
            last = now
            return
        }
        switch mode {
        case .undecided:
            // How the fingers moved since they came down: together (scrolling), apart or together along the line between
            // them (pinching), or past each other across it (twisting), all in points. The biggest decides, but only once
            // it's past its threshold; until then the gesture waits, so a twist whose fingers also drift a little doesn't
            // turn into a scroll before it has turned far enough.
            let dp = CGPoint(x: p.x - startFingers.0.x, y: p.y - startFingers.0.y)
            let dq = CGPoint(x: q.x - startFingers.1.x, y: q.y - startFingers.1.y)
            let axis = CGPoint(x: startFingers.1.x - startFingers.0.x, y: startFingers.1.y - startFingers.0.y)
            let length = max(hypot(axis.x, axis.y), 1)
            let along = CGPoint(x: axis.x / length, y: axis.y / length)
            let apart = CGPoint(x: dq.x - dp.x, y: dq.y - dp.y)
            let scrolled = Double(hypot((dp.x + dq.x) / 2, (dp.y + dq.y) / 2))
            let pinched = Double(abs(apart.x * along.x + apart.y * along.y))
            let twisted = Double(abs(apart.x * along.y - apart.y * along.x))
            let turned = abs(degrees(now.angle - start.angle))
            let biggest = max(scrolled, pinched, twisted)
            let scroll = scrolled == biggest && scrolled >= GestureInjector.scrollStart
            let magnify = !scroll && pinched == biggest && pinched >= GestureInjector.magnifyStart
            let rotate = !scroll && !magnify && twisted == biggest && twisted >= GestureInjector.rotateStart && turned >= GestureInjector.rotateMinimum
            guard scroll || magnify || rotate else { return }
            if GestureInjector.debug {
                logLine(String(format: "gesture: %@ (together %.0f, apart %.0f, across %.0f points, %.0f degrees)",
                               scroll ? "scroll" : magnify ? "magnify" : "rotate", scrolled, pinched, twisted, turned))
            }
            // The pointer goes to where the fingers are, which is what the gesture acts on.
            location = now.center
            if let move = CGEvent(mouseEventSource: source, mouseType: .mouseMoved, mouseCursorPosition: location, mouseButton: .left) {
                move.post(tap: .cghidEventTap)
            }
            if scroll {
                mode = .scroll
                carry = (0, 0)
                scrollBy(now.center.x - start.center.x, now.center.y - start.center.y, phase: 1)
            } else if magnify {
                mode = .magnify
                gesture(GestureInjector.zoomKind, 0, phase: 1)
                gesture(GestureInjector.zoomKind, now.distance / start.distance - 1, phase: 2)
            } else {
                mode = .rotate
                gesture(GestureInjector.rotateKind, 0, phase: 1)
                gesture(GestureInjector.rotateKind, -degrees(now.angle - start.angle), phase: 2)
            }
        case .scroll:
            scrollBy(now.center.x - last.center.x, now.center.y - last.center.y, phase: 2)
        case .magnify:
            gesture(GestureInjector.zoomKind, now.distance / last.distance - 1, phase: 2)
        case .rotate:
            // macOS counts rotation anticlockwise; the tablet's angles grow clockwise, since its y runs down.
            gesture(GestureInjector.rotateKind, -degrees(now.angle - last.angle), phase: 2)
        case .idle:
            break
        }
        last = now
    }

    // Ends whatever gesture is going on, as lifting the fingers would.
    func end() {
        switch mode {
        case .scroll: scrollBy(0, 0, phase: 4)
        case .magnify: gesture(GestureInjector.zoomKind, 0, phase: 4)
        case .rotate: gesture(GestureInjector.rotateKind, 0, phase: 4)
        case .idle, .undecided: break
        }
        mode = .idle
    }

    private func degrees(_ radians: Double) -> Double {
        var d = radians * 180 / .pi
        while d > 180 { d -= 360 }
        while d < -180 { d += 360 }
        return d
    }

    // Content follows the fingers, as on any touch screen.
    private func scrollBy(_ dx: Double, _ dy: Double, phase: Int64) {
        carry.x += dx
        carry.y += dy
        let x = carry.x.rounded(.towardZero)
        let y = carry.y.rounded(.towardZero)
        carry.x -= x
        carry.y -= y
        guard let event = CGEvent(scrollWheelEvent2Source: source, units: .pixel, wheelCount: 2, wheel1: Int32(y), wheel2: Int32(x), wheel3: 0) else { return }
        event.location = location
        event.setIntegerValueField(.scrollWheelEventIsContinuous, value: 1)
        event.setIntegerValueField(.scrollWheelEventScrollPhase, value: phase)
        event.post(tap: .cghidEventTap)
    }

    private func gesture(_ kind: Int64, _ value: Double, phase: Int64) {
        guard let event = CGEvent(source: source) else { return }
        TDEventSetType(event, GestureInjector.gestureType)
        event.location = location
        TDEventSetInteger(event, GestureInjector.kindField, kind)
        TDEventSetDouble(event, GestureInjector.valueField, value)
        TDEventSetInteger(event, GestureInjector.phaseField, phase)
        event.post(tap: .cghidEventTap)
    }
}
