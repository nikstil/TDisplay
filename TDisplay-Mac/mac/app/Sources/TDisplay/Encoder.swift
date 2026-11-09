// The Mac's hardware video encoder through VideoToolbox, tuned like the Windows encoders for lowest latency: no
// B-frames, no lookahead, one frame in and one frame out, and a keyframe only when asked for one. Measured in step 1
// (mac/feasibility/RESULTS.md): HEVC takes about 6.3 ms a frame at 2944x1840 on an M5, H.264 about 9.7 ms and no more
// than about 111 frames a second at that size.
import CoreMedia
import Foundation
import VideoToolbox

final class VideoEncoder {
    struct Params {
        var width: Int
        var height: Int
        var fps: Int
        var bitrate: Int
        var minQp: Int  // Quality past which more bits are pointless. 0 turns this off.
    }

    let codec: UInt32  // Proto.codecHevc or Proto.codecH264.
    // VPS, SPS and PPS for the decoder (H.264 has no VPS), each after a start code.
    private(set) var codecConfig = Data()
    private let session: VTCompressionSession
    private let fps: Int

    // Nil if the hardware encoder wouldn't start for this codec.
    init?(_ params: Params, codec: UInt32, firstImage: CVPixelBuffer) {
        self.codec = codec
        fps = params.fps
        let name = codec == Proto.codecHevc ? "HEVC" : "H.264"
        let specification = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true] as CFDictionary
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey: params.width,
            kCVPixelBufferHeightKey: params.height,
        ]
        var created: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(params.width), height: Int32(params.height),
            codecType: codec == Proto.codecHevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
            encoderSpecification: specification, imageBufferAttributes: attributes as CFDictionary, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &created)
        guard status == noErr, let created else {
            logLine("encoder: no \(name) hardware encoder (\(status))")
            return nil
        }
        session = created

        var refused: [String] = []
        let set = { (key: CFString, value: CFTypeRef) in
            let status = VTSessionSetProperty(created, key: key, value: value)
            if status != noErr { refused.append("\(key) (\(status))") }
        }
        // In real-time mode the encoder runs flat out for a few seconds, then slows down until each frame takes about
        // three times as long, since that still keeps up. Out of it, it stays fast and still holds no frames back.
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, params.fps as CFNumber)
        set(kVTCompressionPropertyKey_ProfileLevel, codec == Proto.codecHevc ? kVTProfileLevel_HEVC_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel)
        set(kVTCompressionPropertyKey_AverageBitRate, params.bitrate as CFNumber)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, Int(Int32.max) as CFNumber)
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        // BT.709, limited range, which is what the tablet is told to expect.
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2)
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2)
        set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2)
        if params.minQp > 0 { set(kVTCompressionPropertyKey_MinAllowedFrameQP, params.minQp as CFNumber) }
        VTCompressionSessionPrepareToEncodeFrames(created)
        if !refused.isEmpty { logLine("encoder: it refused \(refused.joined(separator: ", "))") }

        // The parameter sets only exist once there's a picture, so make one. The stream proper starts on a keyframe of
        // its own.
        guard let first = encode(firstImage, frameId: 0, forceKeyframe: true), !codecConfig.isEmpty else {
            logLine("encoder: it could not make a first picture")
            VTCompressionSessionInvalidate(created)
            return nil
        }
        _ = first
        logLine("encoder: \(name) \(params.width)x\(params.height)@\(params.fps), \(params.bitrate / 1_000_000) Mbps, on the hardware encoder")
    }

    // Encodes one picture and waits for it. The bitstream is Annex B, and keyframe says whether it is one.
    func encode(_ image: CVPixelBuffer, frameId: UInt32, forceKeyframe: Bool) -> (bitstream: Data, keyframe: Bool)? {
        let done = DispatchSemaphore(value: 0)
        nonisolated(unsafe) var result: (Data, Bool)?
        guard submit(image, frameId: frameId, forceKeyframe: forceKeyframe, done: { output in
            result = output
            done.signal()
        }) else { return nil }
        guard done.wait(timeout: .now() + 2) == .success else {
            logLine("encoder: no picture after 2 s")
            return nil
        }
        return result
    }

    // Hands a picture to the encoder and returns at once. done gets the Annex B bitstream and whether it's a keyframe, or
    // nil if the encoder dropped the picture, on VideoToolbox's thread and in the order the pictures went in. False if the
    // encoder wouldn't take it.
    func submit(_ image: CVPixelBuffer, frameId: UInt32, forceKeyframe: Bool, done: @escaping ((bitstream: Data, keyframe: Bool)?) -> Void) -> Bool {
        let properties = forceKeyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue] as CFDictionary : nil
        let status = VTCompressionSessionEncodeFrame(
            session, imageBuffer: image, presentationTimeStamp: CMTime(value: CMTimeValue(frameId), timescale: CMTimeScale(fps)),
            duration: .invalid, frameProperties: properties, infoFlagsOut: nil
        ) { [self] status, _, sample in
            done(status == noErr ? sample.map(convert) : nil)
        }
        if status != noErr { logLine("encoder: could not encode (\(status))") }
        return status == noErr
    }

    private func convert(_ sample: CMSampleBuffer) -> (bitstream: Data, keyframe: Bool) {
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
        let keyframe = !((attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
        if keyframe, codecConfig.isEmpty, let format = CMSampleBufferGetFormatDescription(sample) {
            for set in parameterSets(format) { codecConfig.append(contentsOf: [0, 0, 0, 1] + set) }
        }
        return (annexB(sample), keyframe)
    }

    private func parameterSets(_ format: CMFormatDescription) -> [[UInt8]] {
        var sets: [[UInt8]] = []
        var count = 0
        var index = 0
        repeat {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            let status = codec == Proto.codecHevc
                ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
                : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            guard status == noErr, let pointer else { break }
            sets.append(Array(UnsafeBufferPointer(start: pointer, count: size)))
            index += 1
        } while index < count
        return sets
    }

    // VideoToolbox writes each unit with a 4-byte length in front; the tablet wants Annex B start codes.
    private func annexB(_ sample: CMSampleBuffer) -> Data {
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return Data() }
        var out = Data(count: CMBlockBufferGetDataLength(block))
        let length = out.count
        out.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: $0.baseAddress!) }
        // The start codes take exactly the place of the lengths, so it's done in place.
        out.withUnsafeMutableBytes { bytes in
            var offset = 0
            while offset + 4 <= length {
                let size = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 | Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
                bytes[offset] = 0
                bytes[offset + 1] = 0
                bytes[offset + 2] = 0
                bytes[offset + 3] = 1
                offset += 4 + size
            }
        }
        return out
    }

    func close() {
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
        VTCompressionSessionInvalidate(session)
    }
}
