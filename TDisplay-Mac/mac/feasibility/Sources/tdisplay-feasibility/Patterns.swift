import Accelerate
import AppKit
import CoreVideo

// The Windows test patterns (host/tools/enctest.cpp), drawn straight into 4:2:0 BT.709 limited-range pictures, which is
// what ScreenCaptureKit hands over: desktop (a still background with a few things moving), text (a page of small text
// scrolling 4 pixels a frame, then still), colors (a gray ramp and color bars) and noise.
enum Pattern: String, CaseIterable {
    case desktop, text, colors, noise
}

struct YCbCr {
    var y: UInt8
    var cb: UInt8
    var cr: UInt8

    init(_ r: Double, _ g: Double, _ b: Double) {
        let luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
        y = UInt8((16 + 219 * luma).rounded())
        cb = UInt8((128 + 224 * (b - luma) / 1.8556).rounded())
        cr = UInt8((128 + 224 * (r - luma) / 1.5748).rounded())
    }
}

final class PatternSource {
    static let stillFrames = 30

    let pattern: Pattern
    let width: Int
    let height: Int
    // A page twice as tall as the picture for text and noise to scroll through.
    private var pageY: [UInt8] = []
    private var pageCbCr: [UInt8] = []

    init(_ pattern: Pattern, width: Int, height: Int) {
        self.pattern = pattern
        self.width = width
        self.height = height
        if pattern == .text { drawTextPage() }
        if pattern == .noise {
            pageY = [UInt8](repeating: 0, count: width * height * 2)
            pageCbCr = [UInt8](repeating: 0, count: width * height)
            pageY.withUnsafeMutableBytes { arc4random_buf($0.baseAddress!, $0.count) }
            pageCbCr.withUnsafeMutableBytes { arc4random_buf($0.baseAddress!, $0.count) }
        }
    }

    private func drawTextPage() {
        let pageHeight = height * 2
        guard let context = CGContext(data: nil, width: width, height: pageHeight, bitsPerComponent: 8, bytesPerRow: width * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Big.rawValue) else { return }
        let backgrounds: [(Double, Double, Double)] = [(1, 1, 1), (30 / 255, 30 / 255, 30 / 255), (30 / 255, 30 / 255, 30 / 255), (1, 1, 1)]
        let inks: [(Double, Double, Double)] = [(0, 0, 0), (235 / 255, 235 / 255, 235 / 255), (1, 80 / 255, 80 / 255), (0, 0, 200 / 255)]
        let font = CTFontCreateUIFontForLanguage(.system, 18, nil)!
        let text = "The quick brown fox jumps over the lazy dog 0123456789 (TDisplay sharpness check) iIlL1 oO0 rnm"
        for q in 0..<4 {
            let area = CGRect(x: q % 2 * width / 2, y: q / 2 * pageHeight / 2, width: width / 2, height: pageHeight / 2)
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
        pageY = [UInt8](repeating: 0, count: width * pageHeight)
        pageCbCr = [UInt8](repeating: 0, count: width * pageHeight / 2)
        var range = vImage_YpCbCrPixelRange(Yp_bias: 16, CbCr_bias: 128, YpRangeMax: 235, CbCrRangeMax: 240, YpMax: 235, YpMin: 16, CbCrMax: 240, CbCrMin: 16)
        var conversion = vImage_ARGBToYpCbCr()
        vImageConvert_ARGBToYpCbCr_GenerateConversion(kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2, &range, &conversion, kvImageARGB8888, kvImage420Yp8_CbCr8, vImage_Flags(kvImageNoFlags))
        var source = vImage_Buffer(data: context.data, height: vImagePixelCount(pageHeight), width: vImagePixelCount(width), rowBytes: context.bytesPerRow)
        pageY.withUnsafeMutableBytes { y in
            pageCbCr.withUnsafeMutableBytes { cbcr in
                var destY = vImage_Buffer(data: y.baseAddress, height: vImagePixelCount(pageHeight), width: vImagePixelCount(width), rowBytes: width)
                var destCbCr = vImage_Buffer(data: cbcr.baseAddress, height: vImagePixelCount(pageHeight / 2), width: vImagePixelCount(width / 2), rowBytes: width)
                let permute: [UInt8] = [0, 1, 2, 3]
                _ = vImageConvert_ARGB8888To420Yp8_CbCr8(&source, &destY, &destCbCr, &conversion, permute, vImage_Flags(kvImageNoFlags))
            }
        }
    }

    func draw(into image: CVPixelBuffer, frame: Int, total: Int) {
        CVPixelBufferLockBaseAddress(image, [])
        defer { CVPixelBufferUnlockBaseAddress(image, []) }
        let planes = Planes(image)
        switch pattern {
        case .text, .noise:
            let moving = pattern == .text ? max(0, min(frame, total - PatternSource.stillFrames)) : frame
            let top = moving * 4 % height
            for row in 0..<height {
                _ = pageY.withUnsafeBytes { memcpy(planes.y + row * planes.yStride, $0.baseAddress! + (top + row) * width, width) }
            }
            for row in 0..<height / 2 {
                _ = pageCbCr.withUnsafeBytes { memcpy(planes.cbcr + row * planes.cbcrStride, $0.baseAddress! + (top / 2 + row) * width, width) }
            }
        case .colors:
            let steps = 16
            for i in 0..<steps {
                let v = Double(i) / Double(steps - 1)
                planes.fill(x: i * width / steps, y: 0, width: (i + 1) * width / steps - i * width / steps, height: height / 2, YCbCr(v, v, v))
            }
            let bars: [(Double, Double, Double)] = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (1, 0, 1)]
            for (i, (r, g, b)) in bars.enumerated() {
                planes.fill(x: i * width / 6, y: height / 2, width: (i + 1) * width / 6 - i * width / 6, height: height - height / 2, YCbCr(r, g, b))
            }
        case .desktop:
            planes.fill(x: 0, y: 0, width: width, height: height, YCbCr(0.11, 0.12, 0.14))
            planes.fill(x: frame * 6 % (width - 160), y: 0, width: 160, height: height, YCbCr(0.9, 0.9, 0.92))
            for i in 0..<12 {
                let x = (frame * (i + 2) * 3 + i * 211) % (width - 300)
                let y = 80 + i * (height - 240) / 12
                planes.fill(x: x, y: y, width: 300, height: 90, YCbCr(0.2, 0.6, 1.0))
            }
        }
    }
}

// The two planes of a locked 4:2:0 biplanar picture.
struct Planes {
    let y: UnsafeMutablePointer<UInt8>
    let yStride: Int
    let cbcr: UnsafeMutablePointer<UInt8>
    let cbcrStride: Int
    let width: Int
    let height: Int

    init(_ image: CVPixelBuffer) {
        y = CVPixelBufferGetBaseAddressOfPlane(image, 0)!.assumingMemoryBound(to: UInt8.self)
        yStride = CVPixelBufferGetBytesPerRowOfPlane(image, 0)
        cbcr = CVPixelBufferGetBaseAddressOfPlane(image, 1)!.assumingMemoryBound(to: UInt8.self)
        cbcrStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1)
        width = CVPixelBufferGetWidth(image)
        height = CVPixelBufferGetHeight(image)
    }

    func fill(x: Int, y top: Int, width w: Int, height h: Int, _ color: YCbCr) {
        let x0 = max(0, x), x1 = min(width, x + w), y0 = max(0, top), y1 = min(height, top + h)
        guard x1 > x0, y1 > y0 else { return }
        for row in y0..<y1 { memset(y + row * yStride + x0, Int32(color.y), x1 - x0) }
        var pair: [UInt8] = [color.cb, color.cr, color.cb, color.cr]
        for row in y0 / 2..<(y1 + 1) / 2 {
            memset_pattern4(cbcr + row * cbcrStride + x0 / 2 * 2, &pair, ((x1 + 1) / 2 - x0 / 2) * 2)
        }
    }
}
