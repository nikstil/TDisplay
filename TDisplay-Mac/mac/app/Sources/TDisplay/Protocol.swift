// Wire format between the laptop and the tablet, carried over one adb stream.
// Keep in sync with host/src/protocol.h and android/java/com/tdisplay/Protocol.java.
//
// Every message is a little-endian u32 header, (type << 24) | payloadLength, followed by the payload. All integers and
// floats are little-endian.
import Foundation

enum Proto {
    static let version: UInt32 = 4
    static let socketName = "tdisplay"
    static let maxPayload: UInt32 = 0x00FF_FFFF

    // Laptop to tablet.
    static let config: UInt8 = 0x01  // Config, followed by the codec config bytes (VPS/SPS/PPS, or SPS/PPS for H.264).
    static let frame: UInt8 = 0x02  // FrameHeader, followed by the encoded picture.
    static let ping: UInt8 = 0x03  // Ping.
    static let bye: UInt8 = 0x04  // Empty.

    // Tablet to laptop.
    static let hello: UInt8 = 0x10
    static let pen: UInt8 = 0x11
    static let ack: UInt8 = 0x12
    static let pong: UInt8 = 0x13
    static let goodbye: UInt8 = 0x14
    static let status: UInt8 = 0x15
    // Fingers for gestures. Added after version 4 without changing it: the Windows program skips it.
    static let touch: UInt8 = 0x16

    // The tablet app's version name. Its version code is the protocol version, which is all the Windows program checks;
    // the name says whether it's the build that sends touches. Keep in step with mac/build-tablet-app.sh.
    static let appVersionName = "1.5"

    static let codecHevc: UInt32 = 0
    static let codecH264: UInt32 = 1

    static let penContact: UInt32 = 1 << 0
    static let penInRange: UInt32 = 1 << 1
    static let penBarrel: UInt32 = 1 << 2
    static let penEraser: UInt32 = 1 << 3
    static let penCanceled: UInt32 = 1 << 4

    static let touchMaxFingers = 4
    static let touchCanceled: UInt32 = 1 << 8

    static let helloSize = 20
    static let penSampleSize = 32
    static let ackSize = 12
    static let pongSize = 16
    static let goodbyeSize = 4
    static let statusSize = 12
    static let frameHeaderSize = 16

    static func header(_ type: UInt8, _ length: Int) -> UInt32 { UInt32(type) << 24 | (UInt32(length) & maxPayload) }
}

// Little-endian message building.
struct Writer {
    var bytes: [UInt8] = []

    mutating func u32(_ value: UInt32) { withUnsafeBytes(of: value.littleEndian) { bytes.append(contentsOf: $0) } }
    mutating func u64(_ value: UInt64) { withUnsafeBytes(of: value.littleEndian) { bytes.append(contentsOf: $0) } }
    mutating func append(_ data: Data) { bytes.append(contentsOf: data) }
}

// Little-endian reads from a received payload.
extension Array where Element == UInt8 {
    func u32(_ offset: Int) -> UInt32 { withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) } }
    func u64(_ offset: Int) -> UInt64 { withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt64.self)) } }
    func f32(_ offset: Int) -> Float { Float(bitPattern: u32(offset)) }
}

struct Hello {
    var version: UInt32
    var width: UInt32
    var height: UInt32
    var maxRefreshHz: Float
    var sdk: UInt32

    init(_ p: [UInt8]) {
        version = p.u32(0)
        width = p.u32(4)
        height = p.u32(8)
        maxRefreshHz = p.f32(12)
        sdk = p.u32(16)
    }
}

struct PenSample {
    var flags: UInt32
    var x: Float  // 0..1 across the tablet's view.
    var y: Float  // 0..1 down the tablet's view.
    var pressure: Float  // 0..1.
    var tiltX: Float  // Degrees, -90..90, positive leaning right.
    var tiltY: Float  // Degrees, -90..90, positive leaning toward the user.
    var tabletNs: UInt64

    init(_ p: [UInt8]) {
        flags = p.u32(0)
        x = p.f32(4)
        y = p.f32(8)
        pressure = p.f32(12)
        tiltX = p.f32(16)
        tiltY = p.f32(20)
        tabletNs = p.u64(24)
    }
}

// Where the fingers are: up to four, with the ids Android gives them, x and y 0..1 across the tablet's view. No fingers and
// canceled means Android decided the touch was a palm, or took it away.
struct TouchFrame {
    var canceled: Bool
    var fingers: [(id: UInt32, x: Float, y: Float)]

    // Nil if the payload doesn't hold what it says.
    init?(_ p: [UInt8]) {
        guard p.count >= 4 else { return nil }
        let info = p.u32(0)
        let count = Int(info & 0xFF)
        guard count <= Proto.touchMaxFingers, p.count >= 4 + count * 12 else { return nil }
        canceled = info & Proto.touchCanceled != 0
        fingers = (0..<count).map { (p.u32(4 + $0 * 12), p.f32(8 + $0 * 12), p.f32(12 + $0 * 12)) }
    }
}

struct Ack {
    var frameId: UInt32  // Newest frame the decoder has handed to the display.
    var decodeUs: UInt32  // Time from queueing into the decoder to its output.
    var droppedFrames: UInt32  // Decoded frames skipped because a newer one was ready.

    init(_ p: [UInt8]) {
        frameId = p.u32(0)
        decodeUs = p.u32(4)
        droppedFrames = p.u32(8)
    }
}

struct TabletStatus {
    var powerSave: UInt32 = 0  // Android Battery Saver on (caps rendering at 60 Hz).
    var thermal: UInt32 = 0  // PowerManager thermal status, 0 none .. 6 shutdown.
    var refreshHz: Float = 0  // Refresh rate the panel is running at now.

    init() {}

    init(_ p: [UInt8]) {
        powerSave = p.u32(0)
        thermal = p.u32(4)
        refreshHz = p.f32(8)
    }
}
