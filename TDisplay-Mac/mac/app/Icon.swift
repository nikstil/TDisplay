// Draws the app icon into an .iconset folder for iconutil: a tablet with a pen stroke on its screen, on the usual rounded
// square. Run by package.sh.
import AppKit

let folder = URL(fileURLWithPath: CommandLine.arguments[1])
try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)

func draw(_ pixels: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(pixels) / 1024
    // The rounded square macOS icons sit in, 824 of 1024 across.
    let plate = NSBezierPath(roundedRect: NSRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s), xRadius: 185 * s, yRadius: 185 * s)
    NSGradient(starting: NSColor(srgbRed: 0.24, green: 0.27, blue: 0.32, alpha: 1), ending: NSColor(srgbRed: 0.11, green: 0.12, blue: 0.15, alpha: 1))!
        .draw(in: plate, angle: -90)
    // The tablet, in landscape.
    NSColor(srgbRed: 0.90, green: 0.91, blue: 0.93, alpha: 1).setFill()
    NSBezierPath(roundedRect: NSRect(x: 190 * s, y: 300 * s, width: 644 * s, height: 424 * s), xRadius: 48 * s, yRadius: 48 * s).fill()
    let screen = NSRect(x: 222 * s, y: 332 * s, width: 580 * s, height: 360 * s)
    NSGradient(starting: NSColor(srgbRed: 0.13, green: 0.55, blue: 0.62, alpha: 1), ending: NSColor(srgbRed: 0.16, green: 0.30, blue: 0.62, alpha: 1))!
        .draw(in: NSBezierPath(roundedRect: screen, xRadius: 18 * s, yRadius: 18 * s), angle: -35)
    // A pen stroke across the screen, the green of a connected menu bar icon.
    let stroke = NSBezierPath()
    stroke.move(to: NSPoint(x: 280 * s, y: 420 * s))
    stroke.curve(to: NSPoint(x: 740 * s, y: 600 * s), controlPoint1: NSPoint(x: 420 * s, y: 700 * s), controlPoint2: NSPoint(x: 560 * s, y: 330 * s))
    stroke.lineWidth = 34 * s
    stroke.lineCapStyle = .round
    NSColor(srgbRed: 52 / 255, green: 199 / 255, blue: 89 / 255, alpha: 1).setStroke()
    stroke.stroke()
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for size in [16, 32, 128, 256, 512] {
    try! draw(size).write(to: folder.appendingPathComponent("icon_\(size)x\(size).png"))
    try! draw(size * 2).write(to: folder.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
