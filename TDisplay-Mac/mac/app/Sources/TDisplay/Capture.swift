// ScreenCaptureKit on the virtual display (Desktop Duplication on Windows): 4:2:0 BT.709 limited range at the tablet's
// own size, which is what the encoder wants, so nothing is converted. Keeps the newest frame for the video loop.
import CoreMedia
import Foundation
import ScreenCaptureKit

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    enum Result {
        case newFrame(CVPixelBuffer, UInt64)
        case timeout
        case lost
    }

    private var stream: SCStream?
    private let queue = DispatchQueue(label: "tdisplay.capture", qos: .userInteractive)
    private let condition = NSCondition()
    private var latest: CVPixelBuffer?
    private var latestUs: UInt64 = 0
    private var fresh = false
    // Counts for TDISPLAY_DEBUG: every complete frame, and those replaced by a newer one before the loop took them.
    private(set) var delivered = 0
    private(set) var overtaken = 0
    private var lost = false

    // Starts capturing. Fails if the display never appears to ScreenCaptureKit or there's no permission.
    func open(displayID: CGDirectDisplayID, width: Int, height: Int, hz: Int) -> Bool {
        // A newly made display can take a moment to be listed.
        var target: SCDisplay?
        let start = nowUs()
        while target == nil, nowUs() - start < 3_000_000 {
            let done = DispatchSemaphore(value: 0)
            nonisolated(unsafe) var found: SCDisplay?
            nonisolated(unsafe) var failure: Error?
            SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, error in
                found = content?.displays.first { $0.displayID == displayID }
                failure = error
                done.signal()
            }
            done.wait()
            if let failure {
                logLine("capture: ScreenCaptureKit refused to list displays: \(failure.localizedDescription)")
                return false
            }
            target = found
            if target == nil { sleepMs(100) }
        }
        guard let target else {
            logLine("capture: ScreenCaptureKit doesn't list display \(displayID)")
            return false
        }

        let config = SCStreamConfiguration()
        config.width = width
        config.height = height
        // Half a refresh: at exactly one, ScreenCaptureKit skips a frame that comes a little early, about one in six.
        config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(hz * 2))
        // The capture's own buffers are encoded as they are, and the video loop holds up to two, so a few more than
        // the minimum keeps ScreenCaptureKit from running dry.
        config.queueDepth = 5
        config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        config.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
        config.showsCursor = true
        config.captureResolution = .best
        let stream = SCStream(filter: SCContentFilter(display: target, excludingWindows: []), configuration: config, delegate: self)
        do {
            try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        } catch {
            logLine("capture: \(error.localizedDescription)")
            return false
        }
        let started = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var failure: Error?
        stream.startCapture { error in
            failure = error
            started.signal()
        }
        started.wait()
        if let failure {
            logLine("capture: could not start: \(failure.localizedDescription)")
            return false
        }
        self.stream = stream
        logLine("capture: \(target.width)x\(target.height) points as \(width)x\(height) pixels")
        return true
    }

    // Waits up to timeoutMs for a frame newer than the last one taken.
    func next(timeoutMs: Int) -> Result {
        condition.lock()
        defer { condition.unlock() }
        let deadline = Date(timeIntervalSinceNow: Double(timeoutMs) / 1000)
        while !fresh, !lost {
            if !condition.wait(until: deadline) { break }
        }
        if lost { return .lost }
        guard fresh, let latest else { return .timeout }
        fresh = false
        return .newFrame(latest, latestUs)
    }

    func close() {
        if let stream {
            let stopped = DispatchSemaphore(value: 0)
            stream.stopCapture { _ in stopped.signal() }
            _ = stopped.wait(timeout: .now() + 2)
        }
        stream = nil
        condition.lock()
        latest = nil
        condition.unlock()
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        let arrived = nowUs()
        guard type == .screen,
              let infos = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = infos.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
              let image = CMSampleBufferGetImageBuffer(sample)
        else { return }
        condition.lock()
        delivered += 1
        if fresh { overtaken += 1 }
        latest = image
        latestUs = arrived
        fresh = true
        condition.signal()
        condition.unlock()
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        logLine("capture: stopped: \(error.localizedDescription)")
        condition.lock()
        lost = true
        condition.signal()
        condition.unlock()
    }
}
