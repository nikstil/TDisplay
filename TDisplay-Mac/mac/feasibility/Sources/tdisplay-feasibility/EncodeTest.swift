import CoreVideo
import Foundation

// tdisplay-feasibility encode codec width height fps seconds bitrate_mbps pattern [output]
// The counterpart of tdisplay-enctest.exe: one encoder with nothing else involved, fed a test pattern at the frame
// rate. The output, if given, is the Annex B stream as the tablet would get it, for checking with ffmpeg.
func runEncodeTest(codec: Codec, width: Int, height: Int, fps: Int, seconds: Int, mbps: Int, pattern: Pattern, minQP: Int, output: String?) throws {
    let attributes: [CFString: Any] = [
        kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
        kCVPixelBufferWidthKey: width,
        kCVPixelBufferHeightKey: height,
        kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
    ]
    var pool: CVPixelBufferPool?
    guard CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &pool) == kCVReturnSuccess, let pool else { throw FeasibilityError("no picture pool") }
    let source = PatternSource(pattern, width: width, height: height)

    let openStart = now()
    let encoder = try Encoder(codec: codec, width: width, height: height, fps: fps, bitrate: mbps * 1_000_000, minQP: minQP, outputPath: output)
    log(String(format: "encoder opened in %.1f ms", (now() - openStart) * 1000))

    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)
    let total = seconds * fps
    var drawMs: [Double] = []
    let start = now()
    for frame in 0..<total {
        wait(until: start + Double(frame) / Double(fps))
        var image: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &image) == kCVReturnSuccess, let image else { throw FeasibilityError("pool ran dry") }
        let drawStart = now()
        source.draw(into: image, frame: frame, total: total)
        drawMs.append((now() - drawStart) * 1000)
        encoder.encode(image, index: frame, forceKeyframe: frame == 0 || frame == 120)
    }
    encoder.finish()
    let elapsed = now() - start
    log(String(format: "%@ %dx%d, %@ pattern, %d frames in %.2f s = %.1f fps asked for %d", codec.rawValue, width, height, pattern.rawValue,
               total, elapsed, Double(total) / elapsed, fps))
    log(encoder.report(seconds: elapsed))
    log("  drawing the pattern \(percentiles(drawMs))")
}
