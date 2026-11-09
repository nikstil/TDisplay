import CoreMedia
import Foundation
import VideoToolbox

// tdisplay-feasibility decode h264|hevc stream
// Reads back an Annex B stream written by the encode test, as the tablet would get it, and decodes every picture with
// VideoToolbox. Checks the start codes and parameter sets without needing ffmpeg.
func runDecodeTest(path: String, hevc: Bool) throws {
    let data = try Data(contentsOf: URL(fileURLWithPath: path))
    let bytes = [UInt8](data)
    var units: [ArraySlice<UInt8>] = []
    var start = -1
    var i = 0
    while i + 3 <= bytes.count {
        if bytes[i] == 0, bytes[i + 1] == 0, bytes[i + 2] == 1 {
            if start >= 0 {
                var end = i
                if end > start, bytes[end - 1] == 0 { end -= 1 }
                units.append(bytes[start..<end])
            }
            start = i + 3
            i += 3
        } else {
            i += 1
        }
    }
    if start >= 0 { units.append(bytes[start..<bytes.count]) }

    let type = { (unit: ArraySlice<UInt8>) -> Int in hevc ? Int(unit.first! >> 1) & 0x3F : Int(unit.first!) & 0x1F }
    let parameterTypes: Set<Int> = hevc ? [32, 33, 34] : [7, 8]
    let isPicture = { (t: Int) -> Bool in hevc ? t < 32 : (1...5).contains(t) }
    var parameterSets: [Int: [UInt8]] = [:]
    for unit in units where parameterTypes.contains(type(unit)) && parameterSets[type(unit)] == nil { parameterSets[type(unit)] = Array(unit) }
    guard parameterSets.count == parameterTypes.count else { throw FeasibilityError("parameter sets missing: found types \(parameterSets.keys.sorted())") }

    let ordered = parameterTypes.sorted().map { parameterSets[$0]! }
    var format: CMFormatDescription?
    let status: OSStatus = ordered[0].withUnsafeBufferPointer { a in
        ordered[1].withUnsafeBufferPointer { b in
            if hevc {
                return ordered[2].withUnsafeBufferPointer { c in
                    let pointers = [a.baseAddress!, b.baseAddress!, c.baseAddress!]
                    let sizes = ordered.map(\.count)
                    return CMVideoFormatDescriptionCreateFromHEVCParameterSets(allocator: nil, parameterSetCount: 3, parameterSetPointers: pointers, parameterSetSizes: sizes,
                                                                               nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &format)
                }
            }
            let pointers = [a.baseAddress!, b.baseAddress!]
            return CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: nil, parameterSetCount: 2, parameterSetPointers: pointers, parameterSetSizes: ordered.map(\.count),
                                                                       nalUnitHeaderLength: 4, formatDescriptionOut: &format)
        }
    }
    guard status == noErr, let format else { throw FeasibilityError("the parameter sets don't make a format (\(status))") }
    let dimensions = CMVideoFormatDescriptionGetDimensions(format)

    var session: VTDecompressionSession?
    let attributes = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange] as CFDictionary
    guard VTDecompressionSessionCreate(allocator: nil, formatDescription: format, decoderSpecification: nil, imageBufferAttributes: attributes,
                                       outputCallback: nil, decompressionSessionOut: &session) == noErr, let session
    else { throw FeasibilityError("no decoder") }

    var decoded = 0
    var failed: [OSStatus: Int] = [:]
    var times: [Double] = []
    var size = ""
    for unit in units where isPicture(type(unit)) {
        var sample = [UInt8](repeating: 0, count: 4)
        let length = UInt32(unit.count).bigEndian
        withUnsafeBytes(of: length) { sample.replaceSubrange(0..<4, with: $0) }
        sample.append(contentsOf: unit)
        var block: CMBlockBuffer?
        CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: sample.count, blockAllocator: nil, customBlockSource: nil,
                                           offsetToData: 0, dataLength: sample.count, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block)
        guard let block else { continue }
        sample.withUnsafeBytes { _ = CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: sample.count) }
        var buffer: CMSampleBuffer?
        var sampleSize = sample.count
        CMSampleBufferCreateReady(allocator: nil, dataBuffer: block, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil,
                                  sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize, sampleBufferOut: &buffer)
        guard let buffer else { continue }
        let begin = now()
        let result = VTDecompressionSessionDecodeFrame(session, sampleBuffer: buffer, flags: [], infoFlagsOut: nil) { status, _, image, _, _ in
            if status == noErr, let image {
                decoded += 1
                size = "\(CVPixelBufferGetWidth(image))x\(CVPixelBufferGetHeight(image))"
            } else {
                failed[status, default: 0] += 1
            }
        }
        times.append((now() - begin) * 1000)
        if result != noErr { failed[result, default: 0] += 1 }
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session)
    VTDecompressionSessionInvalidate(session)
    log("\(units.count) units, stream says \(dimensions.width)x\(dimensions.height); decoded \(decoded) pictures of \(size)")
    if !failed.isEmpty { log("  failed: \(failed.map { "\($0.value) with \($0.key)" }.joined(separator: ", "))") }
    log("  decode call \(percentiles(times))")
}
