// Turns pen samples from the tablet into tablet input on the virtual display, ported from host/src/pen.cpp. Where
// Windows has pen pointers, macOS has what a drawing tablet's own driver sends: a proximity event when the pen comes
// into range and when it leaves, and mouse events in between that carry the pen's pressure and tilt. Posting them takes
// the Accessibility permission.
import AppKit
import ApplicationServices
import CoreGraphics

final class PenInjector {
    // Android reports the pen leaving hover just before it touches down. Waiting this long before telling macOS avoids
    // a false out-of-range between hover and contact.
    private static let leaveDelayUs: UInt64 = 60_000
    // A pen lifted straight out of range after touching may never report leaving. Silence this long while hovering is
    // treated as gone; a pen that's really still there comes back on its next sample.
    private static let hoverSilenceUs: UInt64 = 500_000
    // What the pen calls itself to applications. The vendor and tablet are the tablet's own USB IDs.
    private static let deviceID: Int64 = 0x5444
    private static let vendorID: Int64 = 0x17EF
    private static let tabletID: Int64 = 0x201C
    private static let capabilities = Int64(0x0001 | 0x0002 | 0x0004 | 0x0040 | 0x0080 | 0x0100 | 0x0400)  // ID, x, y, buttons, tilt, pressure
    // macOS gives tilt as -1..1, which drawing apps read as up to 60 degrees each way.
    private static let fullTiltDegrees = 60.0

    private let source = CGEventSource(stateID: .hidSystemState)
    private let doubleClickUs = UInt64(NSEvent.doubleClickInterval * 1_000_000)
    private var pressureExponent = 0.5
    private var pressureMinimum = 0.06
    private let rectLock = NSLock()
    private var rect = CGRect.zero
    // The part of the tablet's picture the display fills, as fractions: all of it, unless the main screen is mirrored and
    // fitted to the tablet with bars at the sides or top and bottom.
    private var shown = CGRect(x: 0, y: 0, width: 1, height: 1)
    private var fitted = false
    private var displayID = kCGNullDirectDisplay
    private var tabletWidth = 1
    private var tabletHeight = 1

    private var inRange = false
    private var contact = false
    private var eraser = false
    private var secondary = false  // This stroke is a right click: the barrel button was held as it began.
    private var point = CGPoint.zero
    private var tabletPoint = (x: 0, y: 0)
    private var pressure = 0.0
    private var tiltX = 0.0
    private var tiltY = 0.0
    private var barrel = false
    private var lastSampleUs: UInt64 = 0
    private var leaveAtUs: UInt64 = 0  // Nonzero while a pen-left event is waiting to see if contact follows.
    private var clicks = 0
    private var lastUpUs: UInt64 = 0
    private var lastUpPoint = CGPoint.zero

    // True if macOS lets this program post input. Without it, samples are quietly dropped.
    // CGPreflightPostEventAccess keeps its first answer for the life of the process, so a permission given while running
    // only shows up through AXIsProcessTrusted, which asks again each time. Switching an app on under Accessibility
    // grants both.
    static var allowed: Bool { CGPreflightPostEventAccess() || AXIsProcessTrusted() }

    // The display that pen coordinates map onto, and the tablet's size in pixels. fitted is for a mirrored main screen,
    // which is scaled to fit the tablet rather than filling it.
    func setTarget(_ id: CGDirectDisplayID, width: Int, height: Int, fitted: Bool = false) {
        displayID = id
        tabletWidth = max(width, 1)
        tabletHeight = max(height, 1)
        self.fitted = fitted
        refreshRect()
    }

    // Picks up changes to where the display sits and to its size, for when it's changed in System Settings.
    func refreshRect() {
        let bounds = CGDisplayBounds(displayID)
        let area = fitted ? fittedArea(of: bounds.size, in: CGSize(width: tabletWidth, height: tabletHeight)) : CGRect(x: 0, y: 0, width: 1, height: 1)
        rectLock.lock()
        rect = bounds
        shown = area
        rectLock.unlock()
    }

    // Where a point on the tablet (0..1 across its picture) lands on the Mac, for gestures to use the same mapping.
    func screenPoint(x: Float, y: Float) -> CGPoint {
        rectLock.lock()
        let area = rect
        let part = shown
        rectLock.unlock()
        let u = Double(min(max((CGFloat(x) - part.minX) / part.width, 0), 1))
        let v = Double(min(max((CGFloat(y) - part.minY) / part.height, 0), 1))
        return CGPoint(x: min(area.minX + u * area.width, area.maxX - 0.01), y: min(area.minY + v * area.height, area.maxY - 0.01))
    }

    // How the pen's pressure maps onto what macOS gets: output = minimum + (1 - minimum) * input^exponent. An exponent
    // below 1 lifts light strokes, and the minimum is what the lightest contact registers as.
    func setPressureCurve(exponent: Double, minimum: Double) {
        pressureExponent = min(max(exponent, 0.1), 3)
        pressureMinimum = min(max(minimum, 0), 0.9)
    }

    func apply(_ sample: PenSample, nowUs: UInt64) {
        lastSampleUs = nowUs
        let wantContact = sample.flags & Proto.penContact != 0
        let wantInRange = wantContact || sample.flags & Proto.penInRange != 0

        // A tool change can't happen in place: applications treat pen and eraser as separate devices.
        var wantEraser = sample.flags & Proto.penEraser != 0
        if contact, wantContact { wantEraser = eraser }
        if inRange, wantEraser != eraser, !contact {
            proximity(enter: false)
            inRange = false
        }
        eraser = wantEraser

        rectLock.lock()
        let area = rect
        let part = shown
        rectLock.unlock()
        // On the bars around a fitted picture, the pen stays at the nearest edge of the screen.
        let x = Double(min(max((CGFloat(sample.x) - part.minX) / part.width, 0), 1))
        let y = Double(min(max((CGFloat(sample.y) - part.minY) / part.height, 0), 1))
        // Kept a hair inside the far edges, which belong to the next display over.
        point = CGPoint(x: min(area.minX + x * area.width, area.maxX - 0.01), y: min(area.minY + y * area.height, area.maxY - 0.01))
        tabletPoint = (Int((x * Double(tabletWidth - 1)).rounded()), Int((y * Double(tabletHeight - 1)).rounded()))
        let shaped = pressureMinimum + (1 - pressureMinimum) * pow(Double(min(max(sample.pressure, 0), 1)), pressureExponent)
        pressure = wantContact ? shaped : 0
        // Android's tilt leans right and toward the user; macOS's leans right and away from the user.
        tiltX = min(max(Double(sample.tiltX) / PenInjector.fullTiltDegrees, -1), 1)
        tiltY = min(max(-Double(sample.tiltY) / PenInjector.fullTiltDegrees, -1), 1)
        barrel = sample.flags & Proto.penBarrel != 0

        if sample.flags & Proto.penCanceled != 0 {
            if contact { post(secondary ? .rightMouseUp : .leftMouseUp) }
            contact = false
            if inRange { proximity(enter: false) }
            inRange = false
            leaveAtUs = 0
            return
        }

        if !wantInRange {
            // Hold this back briefly in case contact follows straight away.
            if inRange, leaveAtUs == 0 { leaveAtUs = nowUs + PenInjector.leaveDelayUs }
            if contact {
                post(secondary ? .rightMouseUp : .leftMouseUp)
                contact = false
                noteUp(nowUs)
            }
            return
        }
        leaveAtUs = 0

        if !inRange {
            proximity(enter: true)
            inRange = true
        }
        if wantContact, !contact {
            // As with Windows Ink, touching down with the barrel button held is a right click.
            secondary = barrel
            let near = hypot(point.x - lastUpPoint.x, point.y - lastUpPoint.y) < 4
            clicks = since(nowUs, lastUpUs) < doubleClickUs && near ? clicks + 1 : 1
            post(secondary ? .rightMouseDown : .leftMouseDown)
        } else if wantContact {
            post(secondary ? .rightMouseDragged : .leftMouseDragged)
        } else if contact {
            post(secondary ? .rightMouseUp : .leftMouseUp)
            noteUp(nowUs)
        } else {
            post(.mouseMoved)
        }
        contact = wantContact
    }

    private func noteUp(_ nowUs: UInt64) {
        lastUpUs = nowUs
        lastUpPoint = point
    }

    // Call often: it finishes any pending pen-left-the-screen.
    func tick(nowUs: UInt64) {
        if leaveAtUs != 0, nowUs >= leaveAtUs {
            leaveAtUs = 0
            proximity(enter: false)
            inRange = false
        } else if inRange, !contact, since(nowUs, lastSampleUs) >= PenInjector.hoverSilenceUs {
            proximity(enter: false)
            inRange = false
        }
    }

    // Time until tick() next has work, for the caller's poll timeout.
    func msUntilTick(nowUs: UInt64) -> Int {
        guard inRange else { return 250 }
        var next = contact ? nowUs + 250_000 : lastSampleUs + PenInjector.hoverSilenceUs
        if leaveAtUs != 0, leaveAtUs < next { next = leaveAtUs }
        return next <= nowUs ? 0 : Int((next - nowUs) / 1000) + 1
    }

    // Lifts the pen. Used when the connection ends so nothing stays pressed.
    func release() {
        if contact {
            post(secondary ? .rightMouseUp : .leftMouseUp)
            contact = false
        }
        if inRange {
            proximity(enter: false)
            inRange = false
        }
        leaveAtUs = 0
    }

    private func proximity(enter: Bool) {
        guard let event = CGEvent(source: source) else { return }
        event.type = .tabletProximity
        event.location = point
        event.setIntegerValueField(.tabletProximityEventVendorID, value: PenInjector.vendorID)
        event.setIntegerValueField(.tabletProximityEventTabletID, value: PenInjector.tabletID)
        event.setIntegerValueField(.tabletProximityEventPointerID, value: eraser ? 2 : 1)
        event.setIntegerValueField(.tabletProximityEventDeviceID, value: PenInjector.deviceID)
        event.setIntegerValueField(.tabletProximityEventSystemTabletID, value: 1)
        event.setIntegerValueField(.tabletProximityEventVendorPointerSerialNumber, value: 1)
        event.setIntegerValueField(.tabletProximityEventVendorUniqueID, value: PenInjector.deviceID << 8 | (eraser ? 2 : 1))
        event.setIntegerValueField(.tabletProximityEventCapabilityMask, value: PenInjector.capabilities)
        event.setIntegerValueField(.tabletProximityEventPointerType, value: eraser ? 3 : 1)  // NX_TABLET_POINTER_ERASER or _PEN.
        event.setIntegerValueField(.tabletProximityEventEnterProximity, value: enter ? 1 : 0)
        event.post(tap: .cghidEventTap)
    }

    private func post(_ type: CGEventType) {
        let button: CGMouseButton = type == .rightMouseDown || type == .rightMouseDragged || type == .rightMouseUp ? .right : .left
        guard let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: button) else { return }
        event.setIntegerValueField(.mouseEventSubtype, value: Int64(CGEventMouseSubtype.tabletPoint.rawValue))
        if type != .mouseMoved { event.setIntegerValueField(.mouseEventClickState, value: Int64(clicks)) }
        event.setDoubleValueField(.mouseEventPressure, value: pressure)
        event.setIntegerValueField(.tabletEventPointX, value: Int64(tabletPoint.x))
        event.setIntegerValueField(.tabletEventPointY, value: Int64(tabletPoint.y))
        // Bit 0 is the tip, bit 1 the barrel button.
        let tip: Int64 = type == .leftMouseDown || type == .leftMouseDragged || type == .rightMouseDown || type == .rightMouseDragged ? 1 : 0
        event.setIntegerValueField(.tabletEventPointButtons, value: tip | (barrel ? 2 : 0))
        event.setDoubleValueField(.tabletEventPointPressure, value: pressure)
        event.setDoubleValueField(.tabletEventTiltX, value: tiltX)
        event.setDoubleValueField(.tabletEventTiltY, value: tiltY)
        event.setIntegerValueField(.tabletEventDeviceID, value: PenInjector.deviceID)
        event.post(tap: .cghidEventTap)
    }
}
