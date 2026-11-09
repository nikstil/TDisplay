# Step 1 results: the feasibility test

Run on 2026-11-08 on a MacBook Air (Mac17,3, Apple M5, 16 GB) with macOS 27.2 (build 26B123), with only its own 2560×1664 screen
attached and no tablet. The tool is in this folder; `./build.sh` builds it.

## The decision

- **Go on to step 2.** Every part works on macOS 27.2, and the delay is between NVENC's and the Intel route's on Windows.
- **144 Hz**, the tablet's highest. The virtual display, capture and encoder all hold it.
- **HEVC** on the hardware encoder, with real-time mode off. H.264 can't keep up at this size and rate, and low-latency rate control
  is slower than either (details below).

| At 2944×1840, 144 Hz | Mac (M5) | Windows, NVENC | Windows, Intel |
|---|---|---|---|
| Encode | 6.2–6.4 ms | 3.3–3.5 ms | 13–15 ms |
| A change drawn on the display until ScreenCaptureKit hands it over | 5.4 ms (p95 6.8) | not measured | not measured |
| Frames per second captured and encoded | 143.7 | about 137 at the tablet | 57–73 at the tablet |

The Mac encodes about 3 ms slower than NVENC, so if the rest behaves as on Windows, capture to ack should come to about 20 ms,
against 17.2–17.5 with NVENC and 29–31 with Intel. Step 2 measures it properly.

## The virtual display

`CGVirtualDisplay` and its three helper classes are all there on 27.2 (`tdisplay-feasibility classes` lists them). Two differences
from the old class dumps: `CGVirtualDisplayMode`'s width and height are 32-bit, and there's a new
`initWithWidth:height:refreshRate:transferFunction:`. The settings also gained `rotation`, `refreshDeadline` and `isReference`.

| Modes given, with `hiDPI` on | What it starts in |
|---|---|
| 2944×1840 only | 1472×920 at **1×**: big, blurry |
| 1472×920 only | 1472×920 points at 2944×1840 pixels (2×) |
| Both | 1472×920 points at 2944×1840 pixels (2×), and 2944×1840 at 1× is on offer for more space |

So give it the half size, or both. With the size in millimetres set to the real 12.7" panel (274×171 mm), macOS also offers the usual
scaled sizes from 800×500 to 1920×1200.

At 60, 120, 144 and even 240 Hz, the display runs at exactly the rate asked for: a `CVDisplayLink` on it ticked at 6.944 ms for
144 Hz with no spread at all. It's made in 80–300 ms, is online 45–72 ms later, and is gone 12–42 ms after it's released. In the
capture test it was only removed once the stream and the window on it had been let go of first.

`NSScreen.displayLink` (a `CADisplayLink`) ticked at 60 Hz on the 144 Hz display, whatever frame rate it was asked for. The test draws
from a `CVDisplayLink` instead. It doesn't matter to TDisplay itself, which only captures.

Each display identity left a 3 KB colour profile in `/Library/ColorSync/Profiles/Displays`, owned by root: three from these tests,
named `TDisplay-…`, one for each test identity (vendor 0x5444, products 0x0F00–0x0F02). The real program should use product 1.

## Capture

ScreenCaptureKit captured the display at full 2944×1840 as `420v` with the BT.709 matrix. The test covered the display with a window
that moved on every refresh and carried its frame number in black and white blocks, then read the number back from each captured
frame (`capture 144 60 hevc`, run through `build/TDisplayCaptureTest.app`, see below):

| Rate | Captured | Drawn frames missed | Commit to arrival | Gap between frames |
|---|---|---|---|---|
| 144 Hz, 60 s | 143.7 fps | 20 of 9072 | median 5.4 ms, p95 6.8, p99 7.4 | median 6.95 ms, p95 8.4 |
| 120 Hz, 10 s | 119.9 fps | 1 of 1260 | median 5.9 ms, p95 6.4 | median 8.33 ms, p95 8.7 |
| 60 Hz, 10 s | 60.0 fps | 0 of 630 | median 10.7 ms, p95 13.3 | median 16.7 ms |

- **`minimumFrameInterval` has to be shorter than one refresh.** At exactly one refresh, ScreenCaptureKit dropped any frame that
  came a little sooner than that after the previous one: 117–125 fps captured at 144 Hz, and 7–8 ms commit to arrival. Half a
  refresh, or none at all, captured them all. The plan said one refresh; use half.
- A `queueDepth` of 3 was enough, with each frame encoded straight from the capture's own surface. 6 changed nothing.
- The `displayTime` that comes with each frame is about 4.7 ms *after* the frame arrives, so it's the moment the display would
  show it, not when it was captured. Use the arrival time.

## Encoding

Measured from handing a frame to VideoToolbox until it comes back encoded, at 2944×1840 and 80 Mbps, with the Windows test patterns
(`encode hevc 2944 1840 144 60 80 desktop`):

| Encoder | Per frame | Keeps up with 144 fps |
|---|---|---|
| HEVC, hardware (`ave.hevc`), real-time off | 6.15 ms on the patterns (60 s, no drift), 6.35 on captured frames | Yes |
| HEVC, hardware, real-time on | 6.2 ms for 3–10 s, then 16–18 ms for good | Yes, late |
| H.264, hardware (`ave.avc`) | 9.7 ms | No: about 111 fps at most, and then 62 ms behind |
| H.264, low-latency rate control (`h264.rtvc`) | 10 ms, and the call blocks | No: at 144 fps it dropped every frame. Fine at 60 |
| HEVC, low-latency rate control | 10.7 ms, and the call blocks | No: 86 fps |

- **Real-time mode costs latency.** Told it's encoding in real time, the encoder runs at full speed for a few seconds and then slows
  down until each frame takes about three times as long, since that still keeps up. Telling it to expect 480 fps only put that off
  to 10 seconds. With `kVTCompressionPropertyKey_RealTime` off, it stayed at 6.1–6.4 ms through every 30 and 60 second run. With it off,
  it still held no frames back and kept no lookahead. Its `MaxFrameDelayCount` and `LookAheadFrames` read back as 0.
- Low-latency rate control (`kVTVideoEncoderSpecification_EnableLowLatencyRateControl`) picks a different encoder, `rtvc`, that
  isn't the hardware one and is too slow at this size. It's for video calls.
- The macOS 26 presets don't help: `HighSpeed` turns on frame reordering and took 30 ms.
- `PrioritizeEncodingSpeedOverQuality` was accepted, and a smaller motion search changed nothing. The hardware encoder has one core
  and is rated for 498 Mpixel/s; 2944×1840 at 144 Hz is 780 Mpixel/s, and it manages that with time to spare.
- Keyframes come out large without a limit: 919 KB for the text pattern, 5 MB for noise. Step 2 should cap them with
  `DataRateLimits`, as Windows does with a buffer of two frames.

VideoToolbox writes each unit with a length in front. The tool converts that to Annex B start codes and takes VPS, SPS and PPS from
the format description, as step 2 will. Decoding those streams back with VideoToolbox (`decode hevc stream`) gave every picture,
432 of 432 for HEVC text and noise and 180 of 180 for H.264, at 2944×1840.

## The Screen Recording permission

macOS ties it to the app's signature, and an ad-hoc signature changes with every build. Three things learned the hard way:

- The capture test runs through `build/TDisplayCaptureTest.app`, a small launcher (`Launcher.c`) that starts the tool. The tool
  inherits the launcher's permission, so the tool can be rebuilt freely. `./build.sh --app` rebuilds the launcher, after which the
  permission has to be given again.
- An app only appears in the Screen Recording list if it asks for the permission itself. If it asks on another app's behalf,
  macOS won't prompt.
- Adding an app with **+** at the same path as an older entry only changes the older entry, which still holds the old signature, so
  it never works. Give a rebuilt app a new bundle ID and path, or remove the old entry first.

The real app will want a stable signature: the self-signed certificate the plan mentions. Because the project sits in
`~/Documents`, macOS also asked whether each test app could open that folder.

Two entries are left in System Settings → Privacy & Security → Screen & System Audio Recording: "TDisplay Feasibility", which can go,
and "TDisplay Capture Test", which the tool needs for more capture runs.

## Running it again

```bash
./build.sh
```

```bash
.build/release/tdisplay-feasibility display 144 both 3
```

```bash
.build/release/tdisplay-feasibility encode hevc 2944 1840 144 30 80 desktop
```

```bash
open -W --stdout /tmp/capture.txt --stderr /tmp/capture.txt build/TDisplayCaptureTest.app --args capture 144 30 hevc
```

`TD_SET=Key=value,...` sets any encoder property by name, `TD_PRESET=` applies a preset, `TD_INTERVAL_HZ=` changes the capture's
minimum interval, and `TD_FRAME=frame.png` saves the first captured frame.
