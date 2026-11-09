// One connected session with the tablet, ported from host/src/stream.cpp: capture, encode and send on the calling
// thread, receive from the tablet on a second one, and anything slow on a third.
import CoreGraphics
import CoreVideo
import Foundation

enum StreamEnd: Int {
    case connectionLost  // The cable, adb or the app went away unexpectedly.
    case appHidden  // The user left the app on the tablet or turned its screen off.
    case stopped  // The program is quitting.
    case notReady  // The tablet app was closing or starting as we connected. Just try again.
    case setupFailed  // Could not start; the log says why.
}

private struct Average {
    var sum: UInt64 = 0
    var max: UInt64 = 0
    var count: UInt64 = 0

    mutating func add(_ value: UInt64) {
        sum += value
        max = Swift.max(max, value)
        count += 1
    }

    var meanMs: Double { count > 0 ? Double(sum) / Double(count) / 1000 : 0 }
    var maxMs: Double { Double(max) / 1000 }
}

final class Session: @unchecked Sendable {
    private static let pingIntervalUs: UInt64 = 100_000
    // The USB link to this tablet occasionally stops delivering until something new travels toward the tablet. Replies
    // this late mean it has happened, and opening a stream puts a packet on the link no matter what any other stream is
    // waiting for. Normal replies take about 20 ms.
    private static let nudgeAfterUs: UInt64 = 45_000
    private static let nudgeSocket = "tdisplay-nudge"  // Nothing listens here, which is the cheapest answer.
    private static let housekeepingMs = 1000
    // With pings every 100 ms, this much silence means the link or the app is stuck. Reconnecting only takes a fraction
    // of a second, so giving up early makes for a much shorter freeze.
    private static let silenceLimitUs: UInt64 = 2_000_000
    // Frames on their way with no acknowledgement for this long means the tablet's decoder is stuck. Pings still get
    // answered in that state, so the silence check above wouldn't catch it.
    private static let ackLimitUs: UInt64 = 1_500_000
    // macOS is asked for the Accessibility permission once per run, not on every reconnect.
    nonisolated(unsafe) private static var askedForPen = false
    // Longest a pen injection should take before it's worth a line in the log.
    private static let slowInjectUs: UInt64 = 50_000
    private static let socketTimeoutMs = 4000
    private static let statsIntervalUs: UInt64 = 10_000_000
    // After the screen stops changing, a few more encodes of the same image let it sharpen fully.
    private static let refineFrames = 6
    private static let spinUs: UInt64 = 1200
    private static let frameRing = 256
    // A still screen for this long counts as a pause in the statistics.
    private static let pauseUs: UInt64 = 500_000

    private let socket: Socket
    private let adb: Adb
    private let serial: String
    private let display: VirtualDisplay
    private var settings: StreamSettings
    private let externalStop: () -> Bool
    private let onStreaming: (String) -> Void
    private(set) var problem = ""

    private let capture = Capture()
    private let pen = PenInjector()
    // On the input thread, like the pen, and mapped the same way.
    private lazy var gestures = GestureInjector(map: pen.screenPoint)
    private var lastPenUs: UInt64 = 0
    // Fingers count only once the pen has been away this long, so the hand holding it can rest on the screen.
    private static let penQuietUs: UInt64 = 500_000
    private let penAllowed = Shared<Bool>(false)
    private var encoder: VideoEncoder?
    private var firstImage: (CVPixelBuffer, UInt64)?
    private var mirroring = false
    private var width = 0
    private var height = 0
    private var hz = 0
    private var streamFps = 0

    private let ended = Shared<Bool>(false)
    private let endReason = Shared<Int>(StreamEnd.connectionLost.rawValue)
    private let lastReceiveUs = Shared<UInt64>(0)
    private let lastAckUs = Shared<UInt64>(0)
    private let lastPongUs = Shared<UInt64>(0)
    private let lastInputLoopUs = Shared<UInt64>(0)
    private let lastMessageType = Shared<Int>(0)
    private let ackedFrame = Shared<UInt32>(0)
    private let sentFrame = Shared<UInt32>(0)
    private let firstAfterPause = Shared<UInt32>(0)
    private let ackEvent = Event()
    private let nudgeEvent = Event()
    private let ringLock = NSLock()
    private var sentAtUs = [UInt64](repeating: 0, count: Session.frameRing)
    private var lastNudgeUs: UInt64 = 0
    private var nudges: UInt64 = 0
    private var firstUnackedUs: UInt64 = 0  // When the current run of unacknowledged frames began.
    private var pauseJustEnded = false
    private var lastPingUs: UInt64 = 0
    private var tabletNotReady = false
    private var statusLine = ""
    private var lastStatusLineUs: UInt64 = 0

    // Statistics. Encode and send times belong to the video thread, the rest are shared under statsLock.
    private var encodeTime = Average()
    private var sendTime = Average()
    private var bytesSent: UInt64 = 0
    private var framesSent: UInt64 = 0
    private var lastStatsUs: UInt64 = 0
    private let statsLock = NSLock()
    private var roundTrip = Average()
    private var decodeTime = Average()
    private var linkRoundTrip = Average()
    private var afterPause = Average()
    private var capturedFrames: UInt64 = 0
    private var droppedFrames: UInt64 = 0
    private var penSamples: UInt64 = 0
    private var penStrokes: UInt64 = 0
    private var penTouching = false
    private var contactSamples: UInt64 = 0
    private var pressureSum: Double = 0
    private var pressureMax: Float = 0
    private var status = TabletStatus()
    // TDISPLAY_DEBUG in the environment adds a line after each statistics line saying where frames went.
    private static let debug = ProcessInfo.processInfo.environment["TDISPLAY_DEBUG"] != nil
    private var ackWaits = 0
    private var pendingReplaced = 0
    private var lastDelivered = 0
    private var lastOvertaken = 0

    init(socket: Socket, adb: Adb, serial: String, display: VirtualDisplay, settings: StreamSettings, stop: @escaping () -> Bool,
         onStreaming: @escaping (String) -> Void) {
        self.socket = socket
        self.adb = adb
        self.serial = serial
        self.display = display
        self.settings = settings
        externalStop = stop
        self.onStreaming = onStreaming
    }

    private var shouldStop: Bool { ended.get() || externalStop() }

    private func endWith(_ reason: StreamEnd) {
        guard !ended.exchange(true) else { return }
        endReason.put(reason.rawValue)
        ackEvent.set()
        // Wakes the other thread out of any blocking socket call.
        shutdown(socket, SHUT_RDWR)
    }

    private func sendMessage(_ type: UInt8, _ payload: [UInt8]) -> Bool {
        var message = Writer()
        message.u32(Proto.header(type, payload.count))
        message.bytes += payload
        return sendAll(socket, message.bytes)
    }

    private func handshake() -> Bool {
        setTimeouts(socket, ms: Session.socketTimeoutMs)
        setNoDelay(socket)
        // A small buffer keeps frames from queueing out of sight when the cable is busy.
        setSendBuffer(socket, bytes: 512 * 1024)

        guard let header = recvBytes(socket, 4), UInt8(header.u32(0) >> 24) == Proto.hello, Int(header.u32(0) & Proto.maxPayload) == Proto.helloSize,
              let payload = recvBytes(socket, Proto.helloSize)
        else {
            tabletNotReady = true
            return false
        }
        let hello = Hello(payload)
        guard hello.version == Proto.version else {
            logLine("stream: tablet app speaks protocol \(hello.version), this program speaks \(Proto.version). Reinstall the tablet app.")
            problem = "The app on the tablet doesn't match this program. Uninstall it from the tablet, then plug the tablet in again."
            return false
        }
        logLine(String(format: "stream: tablet is %ux%u, up to %.0f Hz, Android SDK %u", hello.width, hello.height, hello.maxRefreshHz, hello.sdk))

        let requestedHz = min(settings.maxRefreshHz, Int(hello.maxRefreshHz + 0.5))
        let arrangement = Arrangement.current
        mirroring = arrangement == .mirror
        let source: CGDirectDisplayID
        if mirroring {
            // No virtual display: the tablet shows the main screen, scaled to fit, and any windows left on the virtual
            // display come back to the other screens.
            display.deactivate()
            source = CGMainDisplayID()
            width = Int(hello.width)
            height = Int(hello.height)
            hz = requestedHz
        } else {
            guard display.activate(width: Int(hello.width), height: Int(hello.height), hz: requestedHz) else {
                problem = "macOS wouldn't make the virtual display. See the log."
                return false
            }
            display.place(arrangement)
            source = display.displayID
            width = display.width
            height = display.height
            hz = display.hz
        }
        pen.setTarget(source, width: width, height: height, fitted: mirroring)
        pen.setPressureCurve(exponent: Double(settings.penCurvePercent) / 100, minimum: Double(settings.penMinimumPercent) / 100)
        // The picture works without it, so a missing Accessibility permission only costs the pen.
        penAllowed.put(PenInjector.allowed)
        if !penAllowed.get(), !Session.askedForPen {
            Session.askedForPen = true
            logLine("pen: no Accessibility permission, so the pen does nothing until it's given")
            CGRequestPostEventAccess()
        }

        // The service checks this before connecting; it can only have gone since.
        guard CGPreflightScreenCaptureAccess() else {
            problem = "Allow TDisplay under System Settings > Privacy & Security > Screen & System Audio Recording."
            return false
        }
        if !capture.open(displayID: source, width: width, height: height, hz: hz) {
            sleepMs(300)
            guard capture.open(displayID: source, width: width, height: height, hz: hz) else {
                problem = "The screen capture wouldn't start. See the log."
                return false
            }
        }
        // Enough to fill the round trip of about 24 ms at this rate without letting a queue build up.
        streamFps = settings.streamFps > 0 ? min(settings.streamFps, hz) : hz
        if settings.maxFramesInFlight <= 0 { settings.maxFramesInFlight = min(max(streamFps * 24 / 1000 + 1, 2), 5) }

        // The encoder makes its first picture from whatever the display shows now, which the stream then starts with.
        if case let .newFrame(image, at) = capture.next(timeoutMs: 1000) { firstImage = (image, at) }
        guard let image = firstImage?.0 ?? blankImage() else { return false }
        let params = VideoEncoder.Params(width: width, height: height, fps: streamFps, bitrate: settings.bitrate, minQp: settings.minQp)
        encoder = VideoEncoder(params, codec: settings.codec ?? Proto.codecHevc, firstImage: image)
        if encoder == nil, settings.codec == nil { encoder = VideoEncoder(params, codec: Proto.codecH264, firstImage: image) }
        guard let encoder else {
            problem = "The video encoder wouldn't start. Close anything else that's recording the screen."
            return false
        }

        var config = Writer()
        config.u32(UInt32(width))
        config.u32(UInt32(height))
        config.u32(UInt32(hz))
        config.u32(UInt32(settings.bitrate))
        config.u32(encoder.codec)
        config.append(encoder.codecConfig)
        return sendMessage(Proto.config, config.bytes)
    }

    private func blankImage() -> CVPixelBuffer? {
        var image: CVPixelBuffer?
        let attributes = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary] as CFDictionary
        CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attributes, &image)
        guard let image else { return nil }
        CVPixelBufferLockBaseAddress(image, [])
        for plane in 0..<2 {
            memset(CVPixelBufferGetBaseAddressOfPlane(image, plane), plane == 0 ? 16 : 128,
                   CVPixelBufferGetBytesPerRowOfPlane(image, plane) * CVPixelBufferGetHeightOfPlane(image, plane))
        }
        CVPixelBufferUnlockBaseAddress(image, [])
        return image
    }

    private func sendFrame(_ frameId: UInt32, _ captureUs: UInt64, _ keyframe: Bool, _ bitstream: Data) -> Bool {
        let payload = Proto.frameHeaderSize + bitstream.count
        guard payload <= Proto.maxPayload else {
            logLine("stream: frame too large (\(payload) bytes)")
            return false
        }
        var prefix = Writer()
        prefix.u32(Proto.header(Proto.frame, payload))
        prefix.u32(frameId)
        prefix.u64(captureUs)
        prefix.u32(keyframe ? 1 : 0)
        return prefix.bytes.withUnsafeBytes { head in bitstream.withUnsafeBytes { body in sendGather(socket, [head, body]) } }
    }

    private func pingAndCheckLink(_ now: UInt64) -> Bool {
        if since(now, lastReceiveUs.get()) > Session.silenceLimitUs {
            logLine(String(format: "stream: nothing from the tablet for %.1f s (last message type 0x%02x, input thread last ran %.0f ms ago, sent %u, acked %u)",
                           Double(since(now, lastReceiveUs.get())) / 1e6, lastMessageType.get(), Double(since(now, lastInputLoopUs.get())) / 1e3,
                           sentFrame.get(), ackedFrame.get()))
            return false
        }
        let unacked = sentFrame.get() != ackedFrame.get()
        let framesLate = unacked && since(now, lastAckUs.get()) > Session.nudgeAfterUs && since(now, firstUnackedUs) > Session.nudgeAfterUs
        let pongLate = lastPingUs > lastPongUs.get() && since(now, lastPingUs) > Session.nudgeAfterUs
        if framesLate || pongLate, since(now, lastNudgeUs) > Session.nudgeAfterUs {
            lastNudgeUs = now
            nudges += 1
            nudgeEvent.set()
        }
        if unacked, since(now, lastAckUs.get()) > Session.ackLimitUs, since(now, firstUnackedUs) > Session.ackLimitUs {
            logLine(String(format: "stream: no frame decoded for %.1f s (sent %u, acked %u; last message from the tablet %.0f ms ago, type 0x%02x)",
                           Double(since(now, lastAckUs.get())) / 1e6, sentFrame.get(), ackedFrame.get(), Double(since(now, lastReceiveUs.get())) / 1e3,
                           lastMessageType.get()))
            return false
        }
        if now - lastPingUs >= Session.pingIntervalUs {
            lastPingUs = now
            var ping = Writer()
            ping.u64(now)
            guard sendMessage(Proto.ping, ping.bytes) else {
                logLine("stream: ping could not be sent (\(String(cString: strerror(errno))))")
                return false
            }
        }
        return true
    }

    private func encodeAndSend(_ image: CVPixelBuffer, captureUs: UInt64, forceKeyframe: Bool) -> Bool {
        let frameId = sentFrame.get() &+ 1
        let encodeStart = nowUs()
        if sentFrame.get() == ackedFrame.get() { firstUnackedUs = encodeStart }
        if pauseJustEnded {
            pauseJustEnded = false
            firstAfterPause.put(frameId)
        }
        guard let (bitstream, keyframe) = encoder!.encode(image, frameId: frameId, forceKeyframe: forceKeyframe) else { return false }
        let encodeEnd = nowUs()
        ringLock.lock()
        sentAtUs[Int(frameId) % Session.frameRing] = captureUs
        ringLock.unlock()
        sentFrame.put(frameId)
        guard sendFrame(frameId, captureUs, keyframe, bitstream) else {
            logLine("stream: frame could not be sent (\(String(cString: strerror(errno))))")
            return false
        }
        let sendEnd = nowUs()
        encodeTime.add(encodeEnd - encodeStart)
        sendTime.add(sendEnd - encodeEnd)
        bytesSent += UInt64(bitstream.count)
        framesSent += 1
        return true
    }

    private func logStats(_ now: UInt64) {
        guard now - lastStatsUs >= Session.statsIntervalUs else { return }
        let seconds = Double(now - lastStatsUs) / 1e6
        statsLock.lock()
        logLine(String(format: "stream: %.1f fps of %.1f captured, %.1f Mbps | encode %.1f ms (max %.1f), send %.1f (max %.1f) | link rtt %.1f (max %.1f) | capture-to-ack %.1f (max %.1f), decode %.1f (max %.1f) | dropped %llu, pen %llu, nudges %llu | tablet %.0f Hz, saver %u, thermal %u | pen down %llu samples in %llu strokes, raw pressure avg %.3f max %.3f | first frame after a pause %.1f (max %.1f)",
                       Double(framesSent) / seconds, Double(capturedFrames) / seconds, Double(bytesSent) * 8 / seconds / 1e6, encodeTime.meanMs, encodeTime.maxMs,
                       sendTime.meanMs, sendTime.maxMs, linkRoundTrip.meanMs, linkRoundTrip.maxMs, roundTrip.meanMs, roundTrip.maxMs, decodeTime.meanMs,
                       decodeTime.maxMs, droppedFrames, penSamples, nudges, Double(status.refreshHz), status.powerSave, status.thermal, contactSamples,
                       penStrokes, contactSamples > 0 ? pressureSum / Double(contactSamples) : 0, Double(pressureMax), afterPause.meanMs, afterPause.maxMs))
        encodeTime = Average()
        sendTime = Average()
        roundTrip = Average()
        decodeTime = Average()
        linkRoundTrip = Average()
        afterPause = Average()
        capturedFrames = 0
        nudges = 0
        droppedFrames = 0
        penSamples = 0
        penStrokes = 0
        contactSamples = 0
        pressureSum = 0
        pressureMax = 0
        statsLock.unlock()
        bytesSent = 0
        framesSent = 0
        if Session.debug {
            logLine("debug: capture delivered \(capture.delivered - lastDelivered), overtaken in capture \(capture.overtaken - lastOvertaken), replaced while pacing \(pendingReplaced), waits for acks \(ackWaits)")
            lastDelivered = capture.delivered
            lastOvertaken = capture.overtaken
            pendingReplaced = 0
            ackWaits = 0
        }
        lastStatsUs = now
    }

    // Once a second is plenty for something that only changes when the user changes a setting.
    private func updateStatusLine() {
        let now = nowUs()
        guard now - lastStatusLineUs >= 1_000_000 else { return }
        lastStatusLineUs = now
        statsLock.lock()
        let powerSave = status.powerSave != 0
        statsLock.unlock()
        var line = mirroring ? "Connected, mirroring the main screen" : "Connected: \(width)x\(height) at \(hz) Hz"
        if powerSave, hz > 60 { line += ". Battery Saver is limiting the tablet to 60 Hz." }
        if !penAllowed.get() { line += ". For the pen, allow TDisplay under Privacy & Security > Accessibility." }
        if line != statusLine {
            statusLine = line
            onStreaming(line)
        }
    }

    private func videoLoop() {
        var needKeyframe = true
        var haveFrame = false
        var pending = false  // A new image is ready but sending it now would exceed the panel's rate.
        var announced = false
        var refineLeft = 0
        var lastEncodeUs: UInt64 = 0
        var pendingCaptureUs: UInt64 = 0
        var lastNewFrameUs = nowUs()
        var latest: CVPixelBuffer?
        // macOS can compose faster than the panel refreshes when another screen is faster; frames beyond the panel's
        // rate would only cost bandwidth and heat. The margin absorbs clock jitter.
        let minGapUs = 1_000_000 / UInt64(max(streamFps, 1)) - 100
        let refineGapUs = 2_000_000 / UInt64(max(hz, 1))
        let frameMs = max(1, 1000 / max(hz, 1))
        lastStatsUs = nowUs()
        // The picture the encoder started from goes first, so a still screen shows up too.
        if let (image, at) = firstImage {
            latest = image
            haveFrame = true
            pending = true
            pendingCaptureUs = at
            refineLeft = Session.refineFrames
        }
        firstImage = nil

        let send = { (captureUs: UInt64) -> Bool in
            // Pacing is measured start to start, so the time spent encoding doesn't slow the rate.
            lastEncodeUs = nowUs()
            guard let image = latest, self.encodeAndSend(image, captureUs: captureUs, forceKeyframe: needKeyframe) else { return false }
            needKeyframe = false
            if !announced {
                announced = true
                logLine("stream: first frame sent")
            }
            return true
        }

        loop: while !shouldStop {
            let now = nowUs()
            if !pingAndCheckLink(now) { break }
            logStats(now)
            if announced { updateStatusLine() }

            // Don't get ahead of the tablet. Anything newer that appears while waiting simply replaces what the capture
            // is holding, so waiting never adds stale frames.
            if sentFrame.get() &- ackedFrame.get() >= UInt32(settings.maxFramesInFlight) {
                ackWaits += 1
                ackEvent.wait(ms: 20)
                continue
            }

            let sinceEncode = now - lastEncodeUs
            // Waits for the capture only have millisecond precision, so the last stretch is a short spin. It only
            // happens while the screen is changing faster than the panel can show.
            if pending, sinceEncode + Session.spinUs >= minGapUs {
                while nowUs() - lastEncodeUs < minGapUs {}
                pending = false
                if !send(pendingCaptureUs) { break }
                continue
            }

            var timeout = refineLeft > 0 ? frameMs : 20
            if pending { timeout = Int((minGapUs - sinceEncode - Session.spinUs) / 1000 + 1) }
            let result = capture.next(timeoutMs: timeout)
            let captureUs = nowUs()
            switch result {
            case .lost:
                logLine("stream: the capture stopped; restarting session")
                endWith(.connectionLost)
                break loop
            case let .newFrame(image, arrived):
                statsLock.lock()
                capturedFrames += 1
                statsLock.unlock()
                // A frame that came in before the loop started is older than lastNewFrameUs, hence since().
                if since(arrived, lastNewFrameUs) > Session.pauseUs { pauseJustEnded = true }
                lastNewFrameUs = arrived
                if pending { pendingReplaced += 1 }
                latest = image
                haveFrame = true
                pending = true
                pendingCaptureUs = arrived
                refineLeft = Session.refineFrames
            case .timeout:
                if !pending, haveFrame, refineLeft > 0, captureUs - lastEncodeUs >= refineGapUs {
                    refineLeft -= 1
                    if !send(captureUs) { break loop }
                }
            }
        }

        endWith(ended.get() ? StreamEnd(rawValue: endReason.get())! : (externalStop() ? .stopped : .connectionLost))
    }

    // Everything that may block for a while and doesn't need to be quick lives here, away from the threads that carry
    // the stream.
    private func housekeepingLoop() {
        var lastRefreshUs: UInt64 = 0
        while !shouldStop {
            if nudgeEvent.wait(ms: Session.housekeepingMs), !shouldStop {
                var ignored = ""
                if let nudge = adb.openAbstract(serial: serial, name: Session.nudgeSocket, error: &ignored) { close(nudge) }
            }
            let now = nowUs()
            if now - lastRefreshUs >= UInt64(Session.housekeepingMs) * 1000 {
                lastRefreshUs = now
                pen.refreshRect()
                // The permission can be given while connected, and takes effect without reconnecting.
                if !penAllowed.get(), PenInjector.allowed {
                    logLine("pen: Accessibility permission given")
                    penAllowed.put(true)
                }
            }
        }
    }

    private func inputLoop() {
        var poller = pollfd(fd: socket, events: Int16(POLLIN), revents: 0)
        defer {
            gestures.end()
            pen.release()
        }
        while !shouldStop {
            lastInputLoopUs.put(nowUs())
            let ready = poll(&poller, 1, Int32(pen.msUntilTick(nowUs: lastInputLoopUs.get())))
            lastInputLoopUs.put(nowUs())
            if ready < 0 {
                if errno == EINTR { continue }
                logLine("stream: waiting for tablet data failed (\(String(cString: strerror(errno))))")
                endWith(.connectionLost)
                break
            }
            guard ready > 0 else {
                pen.tick(nowUs: nowUs())
                continue
            }
            guard let head = recvBytes(socket, 4) else {
                if !shouldStop { logLine("stream: tablet connection closed (\(String(cString: strerror(errno))))") }
                endWith(.connectionLost)
                break
            }
            let header = head.u32(0)
            let type = UInt8(header >> 24)
            let size = Int(header & Proto.maxPayload)
            guard size <= 256, let payload = size > 0 ? recvBytes(socket, size) : [] else {
                logLine("stream: bad message from the tablet (type \(type), \(size) bytes)")
                endWith(.connectionLost)
                break
            }
            let now = nowUs()
            lastReceiveUs.put(now)
            lastMessageType.put(Int(type))
            switch type {
            case Proto.pen where size == Proto.penSampleSize:
                let sample = PenSample(payload)
                if sample.flags & (Proto.penInRange | Proto.penContact) != 0 {
                    lastPenUs = now
                    gestures.end()
                }
                if penAllowed.get() {
                    pen.apply(sample, nowUs: now)
                    let injectUs = nowUs() - now
                    if injectUs > Session.slowInjectUs { logLine(String(format: "stream: pen injection took %.0f ms", Double(injectUs) / 1e3)) }
                }
                statsLock.lock()
                penSamples += 1
                let touching = sample.flags & Proto.penContact != 0
                if touching {
                    if !penTouching { penStrokes += 1 }
                    contactSamples += 1
                    pressureSum += Double(sample.pressure)
                    pressureMax = max(pressureMax, sample.pressure)
                }
                penTouching = touching
                statsLock.unlock()
            case Proto.touch:
                guard let frame = TouchFrame(payload) else { break }
                if penAllowed.get(), TouchGestures.enabled, since(now, lastPenUs) > Session.penQuietUs {
                    gestures.apply(frame)
                } else {
                    gestures.end()
                }
            case Proto.ack where size == Proto.ackSize:
                let ack = Ack(payload)
                ackedFrame.put(ack.frameId)
                lastAckUs.put(now)
                ackEvent.set()
                ringLock.lock()
                let captured = sentAtUs[Int(ack.frameId) % Session.frameRing]
                ringLock.unlock()
                statsLock.lock()
                if captured > 0 { roundTrip.add(since(now, captured)) }
                // The tablet may skip straight to a newer picture, so the first one it reports counts.
                let first = firstAfterPause.get()
                if first > 0, captured > 0, ack.frameId &- first < 4 {
                    afterPause.add(since(now, captured))
                    firstAfterPause.put(0)
                }
                decodeTime.add(UInt64(ack.decodeUs))
                droppedFrames += UInt64(ack.droppedFrames)
                statsLock.unlock()
            case Proto.status where size == Proto.statusSize:
                let next = TabletStatus(payload)
                statsLock.lock()
                if next.powerSave != status.powerSave || next.thermal != status.thermal {
                    logLine(String(format: "stream: tablet Battery Saver %@, thermal status %u, panel %.0f Hz", next.powerSave != 0 ? "on" : "off", next.thermal,
                                   Double(next.refreshHz)))
                }
                status = next
                statsLock.unlock()
            case Proto.goodbye:
                logLine("stream: tablet app closed the session")
                endWith(.appHidden)
            case Proto.pong where size == Proto.pongSize:
                lastPongUs.put(now)
                statsLock.lock()
                linkRoundTrip.add(since(now, payload.u64(0)))
                statsLock.unlock()
            default:
                break
            }
            pen.tick(nowUs: nowUs())
        }
    }

    // Runs until the session ends. Owns the socket from here on.
    func run() -> StreamEnd {
        defer { close(socket) }
        lastReceiveUs.put(nowUs())
        guard handshake() else {
            capture.close()
            return tabletNotReady ? .notReady : .setupFailed
        }
        lastReceiveUs.put(nowUs())
        let inputDone = DispatchSemaphore(value: 0)
        let housekeepingDone = DispatchSemaphore(value: 0)
        let input = Thread { [self] in
            inputLoop()
            inputDone.signal()
        }
        input.qualityOfService = .userInteractive
        input.name = "tdisplay-input"
        input.start()
        let housekeeping = Thread { [self] in
            housekeepingLoop()
            housekeepingDone.signal()
        }
        housekeeping.qualityOfService = .utility
        housekeeping.name = "tdisplay-housekeeping"
        housekeeping.start()
        videoLoop()
        nudgeEvent.set()
        inputDone.wait()
        housekeepingDone.wait()
        encoder?.close()
        capture.close()
        if externalStop() { return .stopped }
        return StreamEnd(rawValue: endReason.get())!
    }
}
