package com.tdisplay;

/** Wire format shared with the laptop. Keep in sync with host/src/protocol.h. */
final class Protocol {
    static final int VERSION = 4;
    static final String SOCKET_NAME = "tdisplay";

    // Laptop to tablet.
    static final int CONFIG = 0x01;
    static final int FRAME = 0x02;
    static final int PING = 0x03;
    static final int BYE = 0x04;

    // Tablet to laptop.
    static final int HELLO = 0x10;
    static final int PEN = 0x11;
    static final int ACK = 0x12;
    static final int PONG = 0x13;
    static final int GOODBYE = 0x14;
    static final int STATUS = 0x15;
    /** Where the fingers are. Optional: added after version 4 without changing it, and skipped by laptops that don't know it. */
    static final int TOUCH = 0x16;

    static final int PEN_CONTACT = 1;
    static final int PEN_IN_RANGE = 1 << 1;
    static final int PEN_BARREL = 1 << 2;
    static final int PEN_ERASER = 1 << 3;
    static final int PEN_CANCELED = 1 << 4;

    static final int GOODBYE_APP_HIDDEN = 1;

    /** Touch: u32 (count | flags), then up to TOUCH_MAX_FINGERS of (u32 id, f32 x, f32 y), x and y 0..1 across the view. */
    static final int TOUCH_MAX_FINGERS = 4;
    static final int TOUCH_CANCELED = 1 << 8;

    static final int CODEC_HEVC = 0;
    static final int CODEC_H264 = 1;

    static final int CONFIG_SIZE = 20;
    static final int FRAME_HEADER_SIZE = 16;
    static final int PING_SIZE = 8;

    /** adbd runs as this user, so a connection from it arrived over the cable. */
    static final int SHELL_UID = 2000;

    /** With a ping every 100 ms, this much silence means the laptop is gone. */
    static final int READ_TIMEOUT_MS = 3000;

    private Protocol() {}
}
