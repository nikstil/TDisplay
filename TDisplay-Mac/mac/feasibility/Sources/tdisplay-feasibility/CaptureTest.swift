import AppKit
import CoreMedia
import CoreVideo
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers
import VirtualDisplayShim

// tdisplay-feasibility capture hz seconds [h264|h264-ll|hevc|hevc-ll|none] [mbps] [pixels|points|both] [queue-depth]
// Makes the virtual display, covers it with a window that changes on every vsync, captures it with ScreenCaptureKit
// and, unless told none, encodes each captured frame as the real program would. Every drawn frame carries its number
// as black and white blocks, so each captured frame can be matched to the moment it was committed.
// Drawing follows a CVDisplayLink on the display. TD_LINK=ca uses the screen's CADisplayLink instead, which on macOS
// 27.2 ticks at 60 Hz on a 144 Hz virtual display. TD_FRAME=path.png saves the first captured frame's luma.
// TD_INTERVAL_HZ overrides the capture's minimum frame interval, as a rate.
final class CaptureTest: NSObject, SCStreamOutput, SCStreamDelegate {
    static let bits = 20
    static let block = 32  // pixels
    static let origin = 64  // pixels from the top left
    static let slots = 1 << bits

    let hz: Int
    let seconds: Double
    let codec: Codec?
    let mbps: Int
    let style: ModeStyle
    let queueDepth: Int

    private var display: TDVirtualDisplay?
    private var window: NSWindow?
    private var link: CVDisplayLink?
    private var caLink: CADisplayLink?
    private var stream: SCStream?
    private var encoder: Encoder?
    private let queue = DispatchQueue(label: "capture", qos: .userInteractive)

    // Used on the main thread.
    private var frame = 0
    private var scale: CGFloat = 1
    private var pixelWidth = 0
    private var pixelHeight = 0
    private let bar = CALayer()
    private var boxes: [CALayer] = []
    private var bitLayers: [CALayer] = []
    private var inverseLayers: [CALayer] = []
    private var drawing = false
    private var drawn = 0
    private var lastTick = 0.0
    private var tickGaps: [Double] = []
    private var commitMs: [Double] = []
    // Written on the main thread when a frame is committed, read on the capture queue once that frame comes back.
    private let commitTimes = UnsafeMutablePointer<Double>.allocate(capacity: CaptureTest.slots)

    // Used on the capture queue.
    private var measuring = false
    private var measureStart = 0.0
    private var measureEnd = 0.0
    private var arrivals: [Double] = []
    private var latency: [Double] = []
    private var toDisplay: [Double] = []
    private var fromDisplay: [Double] = []
    private var idle = 0
    private var otherStatus = 0
    private var unreadable = 0
    private var repeats = 0
    private var skipped = 0
    private var lastCounter = -1
    private var encoded = 0
    private var reportedUnreadable = false

    init(hz: Int, seconds: Double, codec: Codec?, mbps: Int, style: ModeStyle, queueDepth: Int) {
        self.hz = hz
        self.seconds = seconds
        self.codec = codec
        self.mbps = mbps
        self.style = style
        self.queueDepth = queueDepth
        commitTimes.initialize(repeating: 0, count: CaptureTest.slots)
    }

    func start() {
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            log("no Screen Recording permission: turn on TDisplay Capture Test under System Settings > Privacy & Security > Screen & System Audio Recording, then run it again")
            exit(3)
        }
        do {
            display = try makeVirtualDisplay(width: Tablet.width, height: Tablet.height, hz: Double(hz), style: style, productID: testProductID(style))
        } catch {
            fail("no virtual display: \(error)")
        }
        let id = display!.displayID
        log("display \(id), \(style.rawValue) modes at \(hz) Hz")
        let asked = now()
        Timer.scheduledTimer(withTimeInterval: 0.02, repeats: true) { [self] timer in
            if let screen = NSScreen.screens.first(where: { ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == id }) {
                timer.invalidate()
                log(String(format: "NSScreen appeared after %.0f ms", (now() - asked) * 1000))
                setUp(screen)
            } else if now() - asked > 10 {
                fail("the display never showed up as an NSScreen")
            }
        }
    }

    private func setUp(_ screen: NSScreen) {
        usleep(300_000)
        log(describeModesInChild(display!.displayID).trimmingCharacters(in: .newlines))
        scale = screen.backingScaleFactor
        pixelWidth = Int(screen.frame.width * scale)
        pixelHeight = Int(screen.frame.height * scale)

        let window = NSWindow(contentRect: screen.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.setFrame(screen.frame, display: false)
        window.level = .screenSaver
        window.isOpaque = true
        window.backgroundColor = .black
        window.ignoresMouseEvents = true
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
        let view = NSView(frame: CGRect(origin: .zero, size: screen.frame.size))
        let host = CALayer()
        view.layer = host
        view.wantsLayer = true
        window.contentView = view
        // AppKit manages the hosting layer's geometry, so the flipped coordinates go on a layer of its own.
        let root = CALayer()
        root.frame = CGRect(origin: .zero, size: screen.frame.size)
        root.isGeometryFlipped = true
        root.backgroundColor = CGColor(srgbRed: 0.11, green: 0.12, blue: 0.14, alpha: 1)
        host.addSublayer(root)

        bar.backgroundColor = CGColor(srgbRed: 0.9, green: 0.9, blue: 0.92, alpha: 1)
        root.addSublayer(bar)
        for _ in 0..<12 {
            let box = CALayer()
            box.backgroundColor = CGColor(srgbRed: 0.2, green: 0.6, blue: 1.0, alpha: 1)
            root.addSublayer(box)
            boxes.append(box)
        }
        for b in 0..<CaptureTest.bits {
            for (row, list) in [(0, \CaptureTest.bitLayers), (1, \CaptureTest.inverseLayers)] {
                let layer = CALayer()
                layer.frame = points(CaptureTest.origin + b * CaptureTest.block, CaptureTest.origin + row * CaptureTest.block, CaptureTest.block, CaptureTest.block)
                root.addSublayer(layer)
                self[keyPath: list].append(layer)
            }
        }
        window.orderFrontRegardless()
        self.window = window

        if ProcessInfo.processInfo.environment["TD_LINK"] == "ca" {
            let link = screen.displayLink(target: self, selector: #selector(tick))
            link.preferredFrameRateRange = CAFrameRateRange(minimum: Float(hz), maximum: Float(hz), preferred: Float(hz))
            link.add(to: .main, forMode: .common)
            caLink = link
        } else {
            var link: CVDisplayLink?
            guard CVDisplayLinkCreateWithCGDisplay(display!.displayID, &link) == kCVReturnSuccess, let link else { fail("no display link") }
            CVDisplayLinkSetOutputHandler(link) { [weak self] _, _, _, _, _ in
                DispatchQueue.main.async { self?.tick() }
                return kCVReturnSuccess
            }
            CVDisplayLinkStart(link)
            self.link = link
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.startCapture() }
    }

    private func points(_ x: Int, _ y: Int, _ width: Int, _ height: Int) -> CGRect {
        CGRect(x: CGFloat(x) / scale, y: CGFloat(y) / scale, width: CGFloat(width) / scale, height: CGFloat(height) / scale)
    }

    @objc private func tick() {
        let t = now()
        if drawing {
            if lastTick > 0 { tickGaps.append((t - lastTick) * 1000) }
            drawn += 1
        }
        lastTick = t
        frame += 1
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        bar.frame = points(frame * 6 % (pixelWidth - 160), 0, 160, pixelHeight)
        for (i, box) in boxes.enumerated() {
            box.frame = points((frame * (i + 2) * 3 + i * 211) % (pixelWidth - 300), 80 + i * (pixelHeight - 240) / 12, 300, 90)
        }
        for b in 0..<CaptureTest.bits {
            let on = (frame >> b) & 1 == 1
            bitLayers[b].backgroundColor = on ? .white : .black
            inverseLayers[b].backgroundColor = on ? .black : .white
        }
        CATransaction.commit()
        let committed = now()
        commitTimes[frame & (CaptureTest.slots - 1)] = committed
        if drawing { commitMs.append((committed - t) * 1000) }
    }

    private func startCapture() {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, error in
            DispatchQueue.main.async { [self] in
                guard let content else { fail("no shareable content: \(String(describing: error))") }
                guard let target = content.displays.first(where: { $0.displayID == display!.displayID }) else {
                    fail("ScreenCaptureKit doesn't list the virtual display")
                }
                let config = SCStreamConfiguration()
                config.width = pixelWidth
                config.height = pixelHeight
                // Half a refresh. With exactly one, ScreenCaptureKit skipped about one frame in six whenever two arrived a
                // little under a refresh apart. TD_INTERVAL_HZ sets it as a rate instead, 0 for no minimum.
                let intervalHz = ProcessInfo.processInfo.environment["TD_INTERVAL_HZ"].flatMap(Int.init) ?? hz * 2
                config.minimumFrameInterval = intervalHz > 0 ? CMTime(value: 1, timescale: CMTimeScale(intervalHz)) : .zero
                config.queueDepth = queueDepth
                config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                config.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
                config.showsCursor = false
                config.captureResolution = .best
                if let codec {
                    do {
                        encoder = try Encoder(codec: codec, width: pixelWidth, height: pixelHeight, fps: hz, bitrate: mbps * 1_000_000, minQP: 22)
                    } catch {
                        fail("\(error)")
                    }
                }
                let stream = SCStream(filter: SCContentFilter(display: target, excludingWindows: []), configuration: config, delegate: self)
                do {
                    try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
                } catch {
                    fail("addStreamOutput: \(error)")
                }
                self.stream = stream
                log("capturing \(target.width)x\(target.height) points as \(pixelWidth)x\(pixelHeight) pixels, queue depth \(queueDepth), minimum interval \(intervalHz > 0 ? "1/\(intervalHz) s" : "none")")
                stream.startCapture { error in
                    if let error { self.fail("startCapture: \(error)") }
                    // A second to settle, then the measurement proper.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.measure() }
                }
            }
        }
    }

    private func measure() {
        drawing = true
        queue.sync {
            measuring = true
            measureStart = now()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { self.finish() }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        let arrival = now()
        guard type == .screen, measuring else { return }
        guard let infos = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let info = infos.first, let raw = info[.status] as? Int, let status = SCFrameStatus(rawValue: raw)
        else {
            otherStatus += 1
            return
        }
        guard status == .complete, let image = CMSampleBufferGetImageBuffer(sample) else {
            if status == .idle { idle += 1 } else { otherStatus += 1 }
            return
        }
        arrivals.append(arrival)
        if arrivals.count == 1, let path = ProcessInfo.processInfo.environment["TD_FRAME"] { saveLuma(image, to: path) }
        guard let counter = readCounter(image) else {
            unreadable += 1
            return
        }
        if counter == lastCounter {
            repeats += 1
        } else if lastCounter >= 0, counter > lastCounter + 1 {
            skipped += counter - lastCounter - 1
        }
        lastCounter = counter
        let committed = commitTimes[counter & (CaptureTest.slots - 1)]
        latency.append((arrival - committed) * 1000)
        if let ticks = info[.displayTime] as? UInt64 {
            let displayed = machSeconds(ticks)
            toDisplay.append((displayed - committed) * 1000)
            fromDisplay.append((arrival - displayed) * 1000)
        }
        encoder?.encode(image, index: encoded, forceKeyframe: encoded == 0)
        encoded += 1
    }

    // The frame number from the blocks, or nil if a block and its inverse don't disagree as they should.
    private func readCounter(_ image: CVPixelBuffer) -> Int? {
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(image, 0)?.assumingMemoryBound(to: UInt8.self) else { return nil }
        let stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0)
        let ratio = Double(CVPixelBufferGetWidth(image)) / Double(pixelWidth)
        let half = CaptureTest.block / 2
        let row = Int(Double(CaptureTest.origin + half) * ratio)
        let inverseRow = Int(Double(CaptureTest.origin + CaptureTest.block + half) * ratio)
        var value = 0
        for b in 0..<CaptureTest.bits {
            let x = Int(Double(CaptureTest.origin + b * CaptureTest.block + half) * ratio)
            let on = base[row * stride + x] > 128
            if on == (base[inverseRow * stride + x] > 128) {
                if !reportedUnreadable {
                    reportedUnreadable = true
                    let samples = (0..<CaptureTest.bits).map { b -> String in
                        let x = Int(Double(CaptureTest.origin + b * CaptureTest.block + half) * ratio)
                        return "\(base[row * stride + x])/\(base[inverseRow * stride + x])"
                    }
                    log("unreadable frame \(CVPixelBufferGetWidth(image))x\(CVPixelBufferGetHeight(image)), luma of each block and its inverse: \(samples.joined(separator: " "))")
                }
                return nil
            }
            if on { value |= 1 << b }
        }
        return value
    }

    private func saveLuma(_ image: CVPixelBuffer, to path: String) {
        CVPixelBufferLockBaseAddress(image, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(image, .readOnly) }
        let width = CVPixelBufferGetWidthOfPlane(image, 0)
        let height = CVPixelBufferGetHeightOfPlane(image, 0)
        let stride = CVPixelBufferGetBytesPerRowOfPlane(image, 0)
        guard let base = CVPixelBufferGetBaseAddressOfPlane(image, 0),
              let provider = CGDataProvider(data: Data(bytes: base, count: stride * height) as CFData),
              let picture = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: stride, space: CGColorSpaceCreateDeviceGray(),
                                    bitmapInfo: CGBitmapInfo(rawValue: 0), provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(destination, picture, nil)
        CGImageDestinationFinalize(destination)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("capture stopped: \(error)")
    }

    private func finish() {
        drawing = false
        queue.sync {
            measuring = false
            measureEnd = now()
        }
        caLink?.invalidate()
        if let link { CVDisplayLinkStop(link) }
        let stopped = DispatchSemaphore(value: 0)
        stream?.stopCapture { _ in stopped.signal() }
        _ = stopped.wait(timeout: .now() + 2)
        stream = nil
        encoder?.finish()

        queue.sync {
            let elapsed = measureEnd - measureStart
            let gaps = zip(arrivals.dropFirst(), arrivals).map { ($0 - $1) * 1000 }
            log(String(format: "drawn: %d frames in %.2f s = %.1f fps", drawn, elapsed, Double(drawn) / elapsed))
            log("  display link gap \(percentiles(tickGaps))")
            log("  layer commit \(percentiles(commitMs))")
            log(String(format: "captured: %d complete = %.1f fps, %d idle, %d other, %d unreadable; %d repeated, %d drawn frames never captured",
                       arrivals.count, Double(arrivals.count) / elapsed, idle, otherStatus, unreadable, repeats, skipped))
            log("  capture gap \(percentiles(gaps))")
            log("  commit to arrival \(percentiles(latency))")
            log("  commit to display time \(percentiles(toDisplay))")
            log("  display time to arrival \(percentiles(fromDisplay))")
            if let encoder { log(encoder.report(seconds: elapsed)) }
        }

        window?.orderOut(nil)
        window?.close()
        window = nil
        let id = display!.displayID
        let removing = now()
        display?.remove()
        display = nil
        // Waits with the run loop going, in case AppKit has to see the display go before the WindowServer lets it.
        Timer.scheduledTimer(withTimeInterval: 0.005, repeats: true) { _ in
            if !onlineDisplays().contains(id) {
                log(String(format: "display removed after %.0f ms", (now() - removing) * 1000))
                exit(0)
            } else if now() - removing > 10 {
                log(String(format: "display still there %.1f s after removal", now() - removing))
                exit(0)
            }
        }
    }

    private func fail(_ message: String) -> Never {
        log(message)
        display?.remove()
        exit(1)
    }
}
