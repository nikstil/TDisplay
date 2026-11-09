import AppKit
import CoreVideo

// tdisplay-feasibility pattern seconds [desktop|text|colors|noise]
// The counterpart of tdisplay-test.exe on Windows: covers the screen the TDisplay app made with a test pattern that
// changes on every refresh, so the stream can be measured with something on it. desktop is a still background with a
// few things moving, text a page of small text scrolling 4 pixels a frame, colors a still gray ramp and color bars, and
// noise random pixels sliding sideways.
final class PatternWindow: NSObject {
    let seconds: Double
    let pattern: Pattern

    private var window: NSWindow?
    private var link: CVDisplayLink?
    private var scale: CGFloat = 2
    private var pixelWidth = 0
    private var pixelHeight = 0
    private var frame = 0
    private let bar = CALayer()
    private var boxes: [CALayer] = []
    private var sliding: [CALayer] = []

    init(seconds: Double, pattern: Pattern) {
        self.seconds = seconds
        self.pattern = pattern
    }

    func start() {
        guard let screen = NSScreen.screens.first(where: { $0.localizedName == "TDisplay" }),
              let id = (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
        else {
            log("no TDisplay screen; start the TDisplay app with the tablet connected first")
            exit(1)
        }
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
        let root = CALayer()
        root.frame = CGRect(origin: .zero, size: screen.frame.size)
        root.isGeometryFlipped = true
        root.backgroundColor = CGColor(srgbRed: 0.11, green: 0.12, blue: 0.14, alpha: 1)
        host.addSublayer(root)

        switch pattern {
        case .desktop:
            bar.backgroundColor = CGColor(srgbRed: 0.9, green: 0.9, blue: 0.92, alpha: 1)
            root.addSublayer(bar)
            for _ in 0..<12 {
                let box = CALayer()
                box.backgroundColor = CGColor(srgbRed: 0.2, green: 0.6, blue: 1.0, alpha: 1)
                root.addSublayer(box)
                boxes.append(box)
            }
        case .colors:
            let steps = 16
            for i in 0..<steps {
                let v = Double(i) / Double(steps - 1)
                let layer = CALayer()
                layer.backgroundColor = CGColor(srgbRed: v, green: v, blue: v, alpha: 1)
                layer.frame = points(i * pixelWidth / steps, 0, (i + 1) * pixelWidth / steps - i * pixelWidth / steps, pixelHeight / 2)
                root.addSublayer(layer)
            }
            let bars: [(Double, Double, Double)] = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (1, 0, 1)]
            for (i, (r, g, b)) in bars.enumerated() {
                let layer = CALayer()
                layer.backgroundColor = CGColor(srgbRed: r, green: g, blue: b, alpha: 1)
                layer.frame = points(i * pixelWidth / 6, pixelHeight / 2, (i + 1) * pixelWidth / 6 - i * pixelWidth / 6, pixelHeight - pixelHeight / 2)
                root.addSublayer(layer)
            }
        case .text, .noise:
            // Two copies one after the other, so the page can slide past without a gap.
            let image = pattern == .text ? textPage() : noisePage()
            for _ in 0..<2 {
                let layer = CALayer()
                layer.contents = image
                layer.contentsScale = scale
                layer.magnificationFilter = .nearest
                root.addSublayer(layer)
                sliding.append(layer)
            }
        }
        draw()
        window.orderFrontRegardless()
        self.window = window

        var link: CVDisplayLink?
        CVDisplayLinkCreateWithCGDisplay(id, &link)
        guard let link else { exit(1) }
        CVDisplayLinkSetOutputHandler(link) { [weak self] _, _, _, _, _ in
            DispatchQueue.main.async { self?.draw() }
            return kCVReturnSuccess
        }
        CVDisplayLinkStart(link)
        self.link = link
        log("showing the \(pattern.rawValue) pattern on TDisplay (\(pixelWidth)x\(pixelHeight)) for \(Int(seconds)) s")
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
            CVDisplayLinkStop(link)
            window.orderOut(nil)
            log("drew \(self.frame) frames")
            exit(0)
        }
    }

    private func points(_ x: Int, _ y: Int, _ width: Int, _ height: Int) -> CGRect {
        CGRect(x: CGFloat(x) / scale, y: CGFloat(y) / scale, width: CGFloat(width) / scale, height: CGFloat(height) / scale)
    }

    private func draw() {
        frame += 1
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        switch pattern {
        case .desktop:
            bar.frame = points(frame * 6 % (pixelWidth - 160), 0, 160, pixelHeight)
            for (i, box) in boxes.enumerated() {
                box.frame = points((frame * (i + 2) * 3 + i * 211) % (pixelWidth - 300), 80 + i * (pixelHeight - 240) / 12, 300, 90)
            }
        case .text:
            // Down a page twice the screen's height, 4 pixels a frame.
            let top = frame * 4 % (pixelHeight * 2)
            sliding[0].frame = points(0, -top, pixelWidth, pixelHeight * 2)
            sliding[1].frame = points(0, pixelHeight * 2 - top, pixelWidth, pixelHeight * 2)
        case .noise:
            let left = frame * 7 % pixelWidth
            sliding[0].frame = points(-left, 0, pixelWidth, pixelHeight)
            sliding[1].frame = points(pixelWidth - left, 0, pixelWidth, pixelHeight)
        case .colors:
            break
        }
        CATransaction.commit()
    }

    private func bitmap(height: Int, draw: (CGContext) -> Void) -> CGImage? {
        guard let context = CGContext(data: nil, width: pixelWidth, height: height, bitsPerComponent: 8, bytesPerRow: pixelWidth * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)
        else { return nil }
        draw(context)
        return context.makeImage()
    }

    // The Windows text page: four quadrants of small text, dark on light and light on dark, twice the screen's height.
    private func textPage() -> CGImage? {
        bitmap(height: pixelHeight * 2) { context in
            let height = pixelHeight * 2
            let backgrounds: [(Double, Double, Double)] = [(1, 1, 1), (30 / 255, 30 / 255, 30 / 255), (30 / 255, 30 / 255, 30 / 255), (1, 1, 1)]
            let inks: [(Double, Double, Double)] = [(0, 0, 0), (235 / 255, 235 / 255, 235 / 255), (1, 80 / 255, 80 / 255), (0, 0, 200 / 255)]
            let font = CTFontCreateUIFontForLanguage(.system, 18 * scale / 2, nil)!
            let text = "The quick brown fox jumps over the lazy dog 0123456789 (TDisplay sharpness check) iIlL1 oO0 rnm"
            for q in 0..<4 {
                let area = CGRect(x: q % 2 * pixelWidth / 2, y: q / 2 * height / 2, width: pixelWidth / 2, height: height / 2)
                let (br, bg, bb) = backgrounds[q]
                context.setFillColor(CGColor(srgbRed: br, green: bg, blue: bb, alpha: 1))
                context.fill(area)
                let (ir, ig, ib) = inks[q]
                let attributes: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: CGColor(srgbRed: ir, green: ig, blue: ib, alpha: 1)]
                let line = CTLineCreateWithAttributedString(NSAttributedString(string: text, attributes: attributes))
                var y = area.maxY - 28
                while y > area.minY + 10 {
                    context.textPosition = CGPoint(x: area.minX + 20, y: y)
                    CTLineDraw(line, context)
                    y -= 28
                }
            }
        }
    }

    private func noisePage() -> CGImage? {
        bitmap(height: pixelHeight) { context in
            if let data = context.data { arc4random_buf(data, context.bytesPerRow * context.height) }
        }
    }
}
