import AppKit
import VirtualDisplayShim

func usage() -> Never {
    print("""
    usage:
      tdisplay-feasibility classes
      tdisplay-feasibility permission
      tdisplay-feasibility display hz [pixels|points|both] [seconds]
      tdisplay-feasibility capture hz seconds [h264|h264-ll|hevc|hevc-ll|none] [mbps] [pixels|points|both] [queue-depth]
      tdisplay-feasibility encode h264|h264-ll|hevc|hevc-ll width height fps seconds mbps desktop|text|colors|noise [min-qp] [output]
      tdisplay-feasibility decode h264|hevc stream
      tdisplay-feasibility pattern seconds [desktop|text|colors|noise]
      tdisplay-feasibility gestures seconds
      tdisplay-feasibility arrange left|right|above|below [session|app|permanent|child]
      tdisplay-feasibility modes display-id
    TD_PRESET=HighSpeed (or another preset the encoder lists) applies a macOS 26 compression preset on top.
    """)
    exit(2)
}

let args = Array(CommandLine.arguments.dropFirst())
func arg(_ i: Int) -> String? { i < args.count ? args[i] : nil }
func int(_ i: Int, _ fallback: Int? = nil) -> Int {
    guard let value = arg(i).flatMap(Int.init) ?? fallback else { usage() }
    return value
}

do {
    switch arg(0) {
    case "permission":
        print("Screen Recording allowed: \(CGPreflightScreenCaptureAccess())")
    case "classes":
        print(TDVirtualDisplay.describePrivateClasses(), terminator: "")
    case "modes":
        runModes(CGDirectDisplayID(int(1)))
    case "display":
        guard let style = ModeStyle(rawValue: arg(2) ?? "both") else { usage() }
        try runDisplayTest(hz: Double(int(1)), style: style, seconds: Double(int(3, 3)))
    case "capture":
        let codecName = arg(3) ?? "hevc"
        let codec = Codec(rawValue: codecName)
        guard codec != nil || codecName == "none", let style = ModeStyle(rawValue: arg(5) ?? "both") else { usage() }
        let test = CaptureTest(hz: int(1), seconds: Double(int(2)), codec: codec, mbps: int(4, 80), style: style, queueDepth: int(6, 3))
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        DispatchQueue.main.async { test.start() }
        app.run()
    case "encode":
        guard let codec = Codec(rawValue: arg(1) ?? ""), let pattern = Pattern(rawValue: arg(7) ?? "") else { usage() }
        try runEncodeTest(codec: codec, width: int(2), height: int(3), fps: int(4), seconds: int(5), mbps: int(6), pattern: pattern,
                          minQP: int(8, 22), output: arg(9))
    case "pattern":
        guard let pattern = Pattern(rawValue: arg(2) ?? "desktop") else { usage() }
        let window = PatternWindow(seconds: Double(int(1)), pattern: pattern)
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        DispatchQueue.main.async { window.start() }
        app.run()
    case "gestures":
        let dump = GestureDump(seconds: Double(int(1, 30)))
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        DispatchQueue.main.async { dump.start() }
        app.run()
    case "arrange":
        try runArrangeTest(arg(1) ?? "right", how: arg(2) ?? "session")
    case "place":
        let status = place(CGDirectDisplayID(int(1)), CGPoint(x: int(2), y: int(3)), .forSession)
        exit(status == .success ? 0 : 1)
    case "decode":
        guard let codec = arg(1), codec == "h264" || codec == "hevc", let path = arg(2) else { usage() }
        try runDecodeTest(path: path, hevc: codec == "hevc")
    default:
        usage()
    }
} catch {
    print("failed: \(error)")
    exit(1)
}
