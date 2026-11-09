import CoreGraphics
import Foundation

// tdisplay-feasibility arrange left|right|above|below [session|app|permanent|child]
// Makes the tablet-sized display, moves it to that side of the main display with CGConfigureDisplayOrigin, and checks it
// can still be removed: the go-macos/virtualdisplay notes say a display changed through a configuration transaction
// can't be, until the process exits. child has another process do the moving.
func runArrangeTest(_ side: String, how: String) throws {
    let display = try makeVirtualDisplay(width: Tablet.width, height: Tablet.height, hz: 144, style: .both, productID: testProductID(.both))
    let id = display.displayID
    guard waitForDisplay(id, online: true) != nil else { throw FeasibilityError("display never came online") }
    usleep(500_000)
    let main = CGDisplayBounds(CGMainDisplayID())
    let before = CGDisplayBounds(id)
    let origin: CGPoint
    switch side {
    case "left": origin = CGPoint(x: main.minX - before.width, y: main.midY - before.height / 2)
    case "above": origin = CGPoint(x: main.midX - before.width / 2, y: main.minY - before.height)
    case "below": origin = CGPoint(x: main.midX - before.width / 2, y: main.maxY)
    default: origin = CGPoint(x: main.maxX, y: main.midY - before.height / 2)
    }
    var result = ""
    if how == "child" {
        let process = Process()
        process.executableURL = Bundle.main.executableURL
        process.arguments = ["place", String(id), String(Int(origin.x.rounded())), String(Int(origin.y.rounded()))]
        try process.run()
        process.waitUntilExit()
        result = "child exited \(process.terminationStatus)"
    } else {
        let option: CGConfigureOption = how == "app" ? .forAppOnly : how == "permanent" ? .permanently : .forSession
        result = "result \(place(id, origin, option).rawValue)"
    }
    usleep(500_000)
    log("\(how): main \(main), virtual before \(before), asked for \(origin), \(result), now \(CGDisplayBounds(id))")
    let removing = now()
    display.remove()
    if let gone = waitForDisplay(id, online: false) {
        log(String(format: "removed after %.0f ms", gone * 1000))
    } else {
        log(String(format: "still there %.1f s after removal", now() - removing))
    }
}

func place(_ id: CGDirectDisplayID, _ origin: CGPoint, _ option: CGConfigureOption) -> CGError {
    var config: CGDisplayConfigRef?
    CGBeginDisplayConfiguration(&config)
    CGConfigureDisplayOrigin(config, id, Int32(origin.x.rounded()), Int32(origin.y.rounded()))
    return CGCompleteDisplayConfiguration(config, option)
}
