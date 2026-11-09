// Wire format between the laptop and the tablet, carried over one adb stream.
// Keep in sync with android/java/com/tdisplay/Protocol.java.
//
// Every message is a little-endian u32 header, (type << 24) | payloadLength,
// followed by the payload. All integers and floats are little-endian.
#pragma once
#include <cstdint>

namespace proto {

constexpr uint32_t kVersion = 4;
// Goes up when the tablet app changes without the protocol changing, so the laptop still replaces an older app.
// The app's version code is kVersion * 100 + kAppRevision. 1: reports fingers for two-finger gestures. 2: instant
// cursor, palm filter, edge lock, statistics and the tablet menu. 3: menu tab, edge lock through app pinning. 4: reports
// three and four fingers, for the three-finger tap. 5: pins once the tablet is unlocked, if it was locked.
constexpr uint32_t kAppRevision = 5;
constexpr uint32_t kAppVersionCode = kVersion * 100 + kAppRevision;
// The Mac version's app uses "tdisplay"; a name of our own keeps the two apart when both are on one tablet.
constexpr const char* kSocketName = "tdisplay-windows";
// The app's id on the tablet. The Mac version's app is com.tdisplay, so both can be installed at once.
constexpr const char* kAppId = "com.tdisplay.windows";
constexpr uint32_t kMaxPayload = 0x00FFFFFF;

enum Type : uint8_t {
    // Laptop to tablet.
    kConfig = 0x01,  // Config, followed by the codec config bytes (VPS/SPS/PPS, or SPS/PPS for H.264).
    kFrame = 0x02,   // FrameHeader, followed by the encoded picture.
    kPing = 0x03,    // Ping.
    kBye = 0x04,     // Empty.

    // Tablet to laptop.
    kHello = 0x10,   // Hello.
    kPen = 0x11,     // PenSample.
    kAck = 0x12,     // Ack.
    kPong = 0x13,    // Pong.
    kGoodbye = 0x14, // Goodbye.
    kStatus = 0x15,  // Status.
    kTouch = 0x16,   // Touch: u32 (count | flags), then count TouchFinger. Added after version 4 without changing
                     // it, so a laptop program that doesn't know it skips it. The Mac version sends the same.
};

constexpr uint32_t kTouchMaxFingers = 4;
constexpr uint32_t kTouchCanceled = 1u << 8;  // Android dropped the touch, for example as a palm.

enum Codec : uint32_t {
    kCodecHevc = 0,
    kCodecH264 = 1,
};

#pragma pack(push, 1)

struct Config {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate;
    uint32_t codec;  // Codec.
};

struct FrameHeader {
    uint32_t frameId;
    uint64_t captureUs;  // Laptop clock, for latency statistics only.
    uint32_t keyframe;
};

struct Ping {
    uint64_t laptopUs;
};

struct Hello {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    float maxRefreshHz;
    uint32_t sdk;
};

enum PenFlag : uint32_t {
    kPenContact = 1u << 0,
    kPenInRange = 1u << 1,
    kPenBarrel = 1u << 2,
    kPenEraser = 1u << 3,
    kPenCanceled = 1u << 4,
};

struct PenSample {
    uint32_t flags;
    float x;  // 0..1 across the tablet's view.
    float y;  // 0..1 down the tablet's view.
    float pressure;  // 0..1.
    float tiltX;  // Degrees, -90..90, positive leaning right.
    float tiltY;  // Degrees, -90..90, positive leaning toward the user.
    uint64_t tabletNs;
};

struct TouchFinger {
    uint32_t id;  // Stays the same while the finger is down.
    float x;      // 0..1 across the tablet's view, as for the pen.
    float y;
};

struct Ack {
    uint32_t frameId;      // Newest frame the decoder has handed to the display.
    uint32_t decodeUs;      // Time from queueing into the decoder to its output.
    uint32_t droppedFrames; // Decoded frames skipped because a newer one was ready.
};

struct Pong {
    uint64_t laptopUs;  // Echo of Ping::laptopUs.
    uint64_t tabletNs;
};

enum GoodbyeReason : uint32_t {
    kGoodbyeAppHidden = 1,  // The user left the app or turned the screen off.
};

struct Goodbye {
    uint32_t reason;
};

struct Status {
    uint32_t powerSave;  // Android Battery Saver on (caps rendering at 60 Hz).
    uint32_t thermal;    // PowerManager thermal status, 0 none .. 6 shutdown.
    float refreshHz;     // Refresh rate the panel is running at now.
};

#pragma pack(pop)

inline uint32_t makeHeader(Type type, uint32_t length) {
    return (static_cast<uint32_t>(type) << 24) | (length & kMaxPayload);
}

}  // namespace proto
