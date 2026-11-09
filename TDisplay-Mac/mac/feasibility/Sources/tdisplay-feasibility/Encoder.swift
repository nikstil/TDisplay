import CoreMedia
import Foundation
import VideoToolbox

// The encoder variants under test, each codec in plain real-time mode and with low-latency rate control, which Apple
// recommends for this kind of use.
enum Codec: String, CaseIterable {
    case h264 = "h264-ll"
    case h264RealTime = "h264"
    case hevc
    case hevcLowLatency = "hevc-ll"

    var isH264: Bool { self == .h264 || self == .h264RealTime }
    var codecType: CMVideoCodecType { isH264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC }
    var lowLatency: Bool { self == .h264 || self == .hevcLowLatency }
    var label: String {
        switch self {
        case .h264: return "H.264, low-latency rate control"
        case .h264RealTime: return "H.264"
        case .hevc: return "HEVC"
        case .hevcLowLatency: return "HEVC, low-latency rate control"
        }
    }
}

// A VideoToolbox session set up like the Windows encoders: hardware only, no B-frames, nothing held back, a keyframe
// only when asked for one, BT.709 limited range. Times each frame from submission to its output.
final class Encoder {
    let codec: Codec
    private let session: VTCompressionSession
    private let fps: Int
    private let lock = NSLock()
    private(set) var refused: [String] = []
    private(set) var hardware = false
    private(set) var encoderID = "?"
    private(set) var presets: [String] = []
    private var encodeMs: [Double] = []
    private var submitMs: [Double] = []
    private var sizes: [Double] = []
    private var keyframes: [Int] = []
    private var failures: [OSStatus: Int] = [:]
    private var dropped = 0
    private var bytes = 0
    private var parameterSets: [Data] = []
    private var output: FileHandle?

    init(codec: Codec, width: Int, height: Int, fps: Int, bitrate: Int, minQP: Int, outputPath: String? = nil) throws {
        self.codec = codec
        self.fps = fps
        var specification: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
        if codec.lowLatency { specification[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true }
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
        ]
        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height), codecType: codec.codecType,
            encoderSpecification: specification as CFDictionary, imageBufferAttributes: attributes as CFDictionary,
            compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        guard status == noErr, let created else { throw FeasibilityError("\(codec.label): VTCompressionSessionCreate failed (\(status))") }
        session = created

        // In real-time mode the M5's encoder runs flat out for a few seconds, then slows down until each frame takes 16-18
        // ms instead of 6, since that still keeps up with the expected rate; expecting 480 fps only put that off to 10
        // seconds. Out of real-time mode it stays at 6 ms. The low-latency encoders need real-time mode.
        set(kVTCompressionPropertyKey_RealTime, codec.lowLatency ? kCFBooleanTrue : kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, codec.isH264 ? kVTProfileLevel_H264_High_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate as CFNumber)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        // The hardware encoders already hold back no frames, and refuse to be told so.
        if codec.lowLatency { set(kVTCompressionPropertyKey_MaxFrameDelayCount, 0 as CFNumber) }
        // Low-latency rate control already never inserts keyframes of its own.
        if !codec.lowLatency { set(kVTCompressionPropertyKey_MaxKeyFrameInterval, Int(Int32.max) as CFNumber) }
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        if minQP > 0 { set(kVTCompressionPropertyKey_MinAllowedFrameQP, minQP as CFNumber) }
        let presetDictionaries = (copy(kVTCompressionPropertyKey_SupportedPresetDictionaries) as? [String: [CFString: Any]]) ?? [:]
        presets = presetDictionaries.keys.sorted()
        // TD_PRESET applies one of the macOS 26 presets on top, for example HighSpeed or VideoConferencing.
        if let name = ProcessInfo.processInfo.environment["TD_PRESET"] {
            guard let settings = presetDictionaries["CompressionPreset_" + name] ?? presetDictionaries[name] else {
                throw FeasibilityError("\(codec.label): no preset \(name), only \(presets)")
            }
            for (key, value) in settings { set(key, value as CFTypeRef) }
        }
        // TD_LIMIT_FRAMES=n caps each frame at n frames' share of the bitrate, like a VBV buffer of n frames on Windows.
        if let frames = ProcessInfo.processInfo.environment["TD_LIMIT_FRAMES"].flatMap(Double.init) {
            set(kVTCompressionPropertyKey_DataRateLimits, [Double(bitrate) / 8 * frames / Double(fps), 1.0 / Double(fps)] as CFArray)
        }
        // TD_SET=Key=value,... sets any other session property by name, for trying things out: numbers, true or false.
        for item in (ProcessInfo.processInfo.environment["TD_SET"] ?? "").split(separator: ",") {
            let parts = item.split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            let value: CFTypeRef = parts[1] == "true" || parts[1] == "false" ? (parts[1] == "true") as CFBoolean : (Double(parts[1]) ?? 0) as CFNumber
            set(parts[0] as CFString, value)
        }
        if ProcessInfo.processInfo.environment["TD_SHOW"] != nil {
            for (name, settings) in presetDictionaries { print("preset \(name): \(settings)") }
            var supported: CFDictionary?
            VTSessionCopySupportedPropertyDictionary(session, supportedPropertyDictionaryOut: &supported)
            print("supported: \(((supported as? [String: Any]) ?? [:]).keys.sorted().joined(separator: " "))")
            for key in ["SupportedThroughputModes", "ThroughputMode", "LookAheadFrames", "NumberOfCores", "MaxEncoderPixelRate", "MaximumRealTimeFrameRate",
                        "MaxFrameDelayCount", "MaximizePowerEfficiency", "Priority", "EncoderUsage", "InputQueueMaxCount", "RecommendedParallelizationLimit",
                        "PreemptiveLoadBalancing", "NumberOfSlices", "MotionEstimationSearchMode", "SupportedMotionSearchModes"] {
                print("  \(key) = \(copy(key as CFString).map { "\($0)" } ?? "unreadable")")
                if let info = (supported as? [String: Any])?[key] { print("    \(info)") }
            }
        }
        VTCompressionSessionPrepareToEncodeFrames(session)

        hardware = (copy(kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder) as? Bool) ?? false
        encoderID = (copy(kVTCompressionPropertyKey_EncoderID) as? String) ?? "?"
        if let outputPath {
            FileManager.default.createFile(atPath: outputPath, contents: nil)
            output = FileHandle(forWritingAtPath: outputPath)
        }
    }

    private func copy(_ key: CFString) -> Any? {
        var value: Unmanaged<CFTypeRef>?
        guard VTSessionCopyProperty(session, key: key, allocator: nil, valueOut: &value) == noErr else { return nil }
        return value?.takeRetainedValue()
    }

    private func set(_ key: CFString, _ value: CFTypeRef) {
        let status = VTSessionSetProperty(session, key: key, value: value)
        if status != noErr { refused.append("\(key) (\(status))") }
    }

    func encode(_ image: CVPixelBuffer, index: Int, forceKeyframe: Bool) {
        let properties = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue] as CFDictionary : nil
        let start = now()
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: image, presentationTimeStamp: CMTime(value: CMTimeValue(index), timescale: CMTimeScale(fps)),
            duration: .invalid, frameProperties: properties, infoFlagsOut: nil
        ) { [self] status, flags, sample in
            received(status: status, dropped: flags.contains(.frameDropped), sample: sample, index: index, ms: (now() - start) * 1000)
        }
        let submitted = (now() - start) * 1000
        lock.lock()
        submitMs.append(submitted)
        if status != noErr { failures[status, default: 0] += 1 }
        lock.unlock()
    }

    private func received(status: OSStatus, dropped: Bool, sample: CMSampleBuffer?, index: Int, ms: Double) {
        guard status == noErr, let sample else {
            lock.lock()
            if dropped { self.dropped += 1 } else { failures[status, default: 0] += 1 }
            lock.unlock()
            return
        }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
        let keyframe = !((attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
        let size = CMSampleBufferGetTotalSampleSize(sample)
        var sets: [Data] = []
        if keyframe, let format = CMSampleBufferGetFormatDescription(sample) { sets = Encoder.parameterSets(format, codec: codec) }
        let annexB = output != nil ? Encoder.annexB(sample) : Data()
        lock.lock()
        encodeMs.append(ms)
        sizes.append(Double(size))
        bytes += size
        if keyframe {
            keyframes.append(index)
            if parameterSets.isEmpty { parameterSets = sets }
        }
        if let output {
            if keyframe { for set in sets { output.write(Data([0, 0, 0, 1]) + set) } }
            output.write(annexB)
        }
        lock.unlock()
    }

    // VPS, SPS and PPS for HEVC, SPS and PPS for H.264, as the Config message will carry them.
    static func parameterSets(_ format: CMFormatDescription, codec: Codec) -> [Data] {
        var sets: [Data] = []
        var count = 0
        var index = 0
        repeat {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            let status = codec.isH264
                ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
                : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            guard status == noErr, let pointer else { break }
            sets.append(Data(bytes: pointer, count: size))
            index += 1
        } while index < count
        return sets
    }

    // VideoToolbox writes each unit with a 4-byte length in front; the tablet wants Annex B start codes.
    static func annexB(_ sample: CMSampleBuffer) -> Data {
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return Data() }
        let length = CMBlockBufferGetDataLength(block)
        var units = Data(count: length)
        units.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
        var out = Data(capacity: length + 32)
        var offset = 0
        while offset + 4 <= length {
            let size = Int(units[offset]) << 24 | Int(units[offset + 1]) << 16 | Int(units[offset + 2]) << 8 | Int(units[offset + 3])
            offset += 4
            guard size > 0, offset + size <= length else { break }
            out.append(contentsOf: [0, 0, 0, 1])
            out.append(units[offset..<offset + size])
            offset += size
        }
        return out
    }

    func finish() {
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
        try? output?.close()
    }

    func report(seconds: Double) -> String {
        lock.lock()
        defer { lock.unlock() }
        var lines: [String] = []
        lines.append(String(format: "%@ on %@%@: %d frames out, %.1f Mbps", codec.label, encoderID, hardware ? " (hardware)" : "",
                            encodeMs.count, Double(bytes) * 8 / seconds / 1e6))
        if dropped > 0 { lines.append("  dropped by the encoder: \(dropped)") }
        if !failures.isEmpty { lines.append("  failed: \(failures.map { "\($0.value) with \($0.key)" }.joined(separator: ", "))") }
        if !presets.isEmpty { lines.append("  presets: \(presets.joined(separator: ", "))") }
        if !refused.isEmpty { lines.append("  refused: \(refused.joined(separator: ", "))") }
        lines.append("  encode \(percentiles(encodeMs))")
        // How it changes over the run, a second at a time.
        let perSecond = stride(from: 0, to: encodeMs.count, by: fps).map { percentiles(Array(encodeMs[$0..<min($0 + fps, encodeMs.count)])).split(separator: ",")[0] }
        lines.append("  encode by second: \(perSecond.map { $0.replacingOccurrences(of: "median ", with: "") }.joined(separator: " "))")
        lines.append("  submit call \(percentiles(submitMs))")
        lines.append("  frame size \(percentiles(sizes.map { $0 / 1024 }, unit: "KB"))")
        lines.append("  keyframes (\(keyframes.count)): \(keyframes.prefix(12).map(String.init).joined(separator: " "))")
        lines.append("  parameter sets: \(parameterSets.map { "\($0.count) bytes" }.joined(separator: ", "))")
        return lines.joined(separator: "\n")
    }
}
