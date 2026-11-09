# TDisplay for Mac: the plan

This was written on the Windows PC on 2026-11-08, to be carried out on the MacBook, by a Claude session running there with the
tablet plugged into the Mac. All five steps are done (see `feasibility/RESULTS.md` and `app/README.md`): TDisplay is installed
in /Applications on the MacBook, streams to the tablet, draws with the pen, copes with unplugging and sleep, and starts at login.

The MacBook has an Apple M5. That's the best case here: its media engine encodes H.264 and HEVC in hardware, and the private
display classes below were tested on Apple silicon. It's a MacBook Air with macOS 27.2, newer than the 26.6.2 those display classes
were last reported working on, and step 1 found them working there too.

## In short

The tablet app stays as it is. It only talks to whatever is on the other end of adb, and since protocol version 4 it decodes H.264
as well as HEVC. A Mac version is a new laptop program for macOS that does what `host/` does on Windows: add a monitor, capture it,
encode it, send it over adb, and turn pen samples from the tablet into pen input. It has to be built and tested on the Mac itself.

## Getting the Mac ready (the owner's part)

1. Done: the chip is an Apple M5. Any macOS from 13 Ventura on keeps things simple (12.3 is the hard minimum, for
   ScreenCaptureKit), and an M5 has a newer one than that.
2. Install Apple's free command-line developer tools: open Terminal and run `xcode-select --install`, then click Install. The full
   Xcode app isn't needed to start.
3. Install Claude Code on the Mac: the Claude desktop app's Code tab, or the `claude` command in Terminal.
4. Copy `TDisplay-mac-start.zip` from the PC's `dist` folder to the Mac (USB stick, cloud drive or email) and unzip it into a
   folder in your home folder, such as `~/TDisplay`.
5. Plug the tablet into the Mac with the USB cable and tap **Allow** when it asks about USB debugging. The Mac is a new computer
   to the tablet.
6. Start a Claude session in that folder and ask it to carry out `mac/PLAN.md`, starting with step 1.

## Each Windows piece and its Mac counterpart

| Job | Windows (`host/`) | Mac |
|---|---|---|
| The extra monitor | Virtual Display Driver, `display.cpp` | `CGVirtualDisplay`: undocumented CoreGraphics classes, no driver to install |
| Capture | Desktop Duplication, `capture.cpp` | ScreenCaptureKit `SCStream` of that display, asked for 4:2:0 YUV (`420v`) with the BT.709 matrix, so there's no conversion step, and a `minimumFrameInterval` of half a refresh |
| Encode | NVENC or Media Foundation | VideoToolbox `VTCompressionSession`: HEVC on the hardware encoder with real-time mode off. Step 1 found H.264 and low-latency rate control too slow at 144 Hz |
| Connection | `adb.cpp`, a private adb server on port 15037 | The same adb protocol over TCP, with Google's platform-tools `adb` for macOS inside the app |
| Wire format | `protocol.h`, version 4 | Unchanged, ported to Swift |
| Session | `stream.cpp`: pacing, frames in flight, pings, nudges, acks, statistics | Ported as it is, with the same statistics line so numbers compare directly |
| Pen | Windows Ink injection, `pen.cpp` | `CGEvent` tablet events: proximity, then tablet-point mouse events with pressure, tilt, eraser and barrel button; the same pressure curve and minimum |
| Tray icon | `main.cpp` | A menu bar item (`NSStatusItem`) with the same states and menu |
| Start with Windows | The Run key | A login item (`SMAppService`, macOS 13+) |
| Setup | `TDisplaySetup.exe` | A signed `.app` in a zip or disk image. There's no driver to install |

Swift suits all of these frameworks. Build with Swift Package Manager from the command-line tools, and make the `.app` bundle with a
small script. Log to `~/Library/Logs/TDisplay/host.log` in the same format as Windows.

## Steps, riskiest first

### 1. Feasibility test (one session, no tablet needed): done

A small command-line tool that answers the questions everything else depends on:

- Create a 2944×1840 virtual display with HiDPI modes at 144 Hz, and also try 120 and 60. Check the rate macOS actually runs it at.
- Capture it with ScreenCaptureKit while something moves on it (`minimumFrameInterval` of one refresh, a small `queueDepth`).
  Measure the gap between frames, and how long a change takes to arrive: draw a counter and compare timestamps.
- Encode those frames with VideoToolbox at 80 Mbps, H.264 with low-latency rate control and HEVC in real-time mode, and measure the
  time per frame.

Then decide the refresh rate and codec, and whether the delay is good enough to go on. For comparison, on the Windows laptop NVENC
encodes in 3.3 ms, and the Intel route in 13–15 ms.

Done on 2026-11-08; the tool and its numbers are in `feasibility/`. The answer is to go on, at 144 Hz with HEVC:

- The virtual display works on macOS 27.2 at exactly 60, 120, 144 or 240 Hz. Give it the 1472×920 mode (or that and 2944×1840)
  with `hiDPI` on, and it starts at 1472×920 points on 2944×1840 pixels. Given only 2944×1840, it starts at 1×.
- ScreenCaptureKit captured 143.7 fps, and a change took 5.4 ms (p95 6.8) from being drawn to arriving, but only with a
  `minimumFrameInterval` of half a refresh. At one refresh it skipped about a frame in six.
- HEVC on the hardware encoder took 6.2–6.4 ms a frame, steady over a minute, but only with real-time mode off: in real-time mode
  it slows to 16–18 ms after a few seconds. H.264 on the hardware takes 9.7 ms and tops out around 111 fps at this size, and
  low-latency rate control uses a slower encoder that isn't the hardware one.

### 2. Streaming, without the pen: done

The menu bar app: start the private adb server, install `TDisplay.apk` (version 4, in `dist/`) when the tablet's version differs,
start the app on the tablet, open the `tdisplay` socket, read Hello, create the virtual display at the tablet's size and rate, send
Config with the codec and parameter sets, then frames, with pings, nudges, acks and statistics as `stream.cpp` does.

VideoToolbox writes each picture as length-prefixed units. Convert them to Annex B start codes, and take VPS, SPS and PPS (or SPS
and PPS for H.264) from the format description; `feasibility/Sources/tdisplay-feasibility/Encoder.swift` already does both, and
the result decodes. Cap keyframes with `DataRateLimits`, as Windows does with a buffer of two frames: uncapped, a keyframe of busy
content came to 5 MB. This MacBook Air has no fan, so watch the encode time over a long session. Done when the picture shows on the
tablet and the statistics line can be compared with Windows.

Done on 2026-11-08; the app is in `app/`, and `app/README.md` has the numbers. With the desktop pattern at 2944×1840 and 144 Hz:
6.4–7.0 ms encode, 21.4–22.9 ms capture to ack and 120–133 fps, against NVENC's 3.3 ms, 17.2–17.5 ms and 137 fps. The picture and
its colours are right, a stopped tablet app is back in 0.4 s, and quitting takes the display away. What differs from the above:

- `DataRateLimits` didn't cap keyframes on the hardware encoder, so it isn't set. Busy content can overshoot the bitrate (noise:
  136 Mbps for 80).
- With the desktop pattern at 144 Hz it sends 120–133 fps. That isn't the encoder or the ported loop: ScreenCaptureKit only
  delivers about that many frames while TDisplay streams (about 120 on battery), and the loop sends all but 1–2% of them. Encoding
  the next frame while the last was still being sent was tried afterwards and made no measurable difference, so the loop stays as
  `stream.cpp` has it.

### 3. Pen: done

Map the tablet's 0–1 coordinates onto the virtual display's bounds (`CGDisplayBounds`). Post a proximity event when the pen comes
into range and leaves, and tablet-point events with pressure (through the same curve and minimum as Windows), tilt, eraser and
barrel button. This needs the Accessibility permission. Done when pressure and tilt work in Krita (free) or Photoshop. Fingers are
already ignored on the tablet.

Done on 2026-11-08, in `app/Sources/TDisplay/Pen.swift`: pressure and tilt work in Photoshop with the real pen. Tilt goes to macOS as
a fraction of 60 degrees, with the vertical axis flipped, which is how Qt (and so Krita) reads it back. As with Windows Ink, a tap
with the barrel button held is a right click, and the eraser end is a device of its own. Without the Accessibility permission the
picture still works and the menu says what's missing. `TDisplay --pen-test` draws scripted strokes in a window of its own and prints
what AppKit received, the counterpart of `tdisplay-pentest.exe`.

### 4. Everyday behavior: done

Take the display away when the tablet goes (the same 20-second grace as Windows), reconnect, survive sleep and wake, show the
Battery Saver hint, start at login, and read a config file with the same keys as Windows. The menu shows the status and opens the
log folder.

Done on 2026-11-08; `app/README.md` has what each test showed. The session ends as the Mac goes to sleep and starts again as it
wakes, 0.3 s after waking in the test; a dark wake with the lid closed doesn't count. Two changes from Windows: after the user leaves
the tablet app, the menu keeps saying how to come back rather than switching to a general waiting message, and the adb restart for
a tablet that adb can't see waits 1.5 s from when the tablet's USB connection appears, not from when it went missing. The old way
restarted adb the moment a tablet was plugged back in, costing 3.5 s; `host/src/service.cpp` had the same flaw and has the same fix
now, not yet built or tried on the PC. Start at login uses
`SMAppService`, which needs a real app bundle, so it's tried in step 5.

### 5. Packaging: done

A `.app` with adb and the APK inside, ad-hoc signed for use on this Mac. To give it to other people it needs Developer ID signing and
notarization, which takes Apple's paid developer membership ($99 a year). Without that, macOS blocks it until each person allows it
under System Settings → Privacy & Security. adb carries the same licence question as on Windows.

Done on 2026-11-08: `app/package.sh` makes `dist/TDisplay-mac.dmg`, and the app from it is installed in /Applications, with its
permissions and Start at login working. Rather than ad hoc, it's signed with a local self-signed certificate
(`app/make-signing-identity.sh`), so the permissions survive rebuilds; ad hoc is the fallback where there's no certificate. Developer
ID signing is one variable away (`SIGN_IDENTITY`), and notarizing is described in `app/README.md` but untried. The adb licence
question is still open. Three things the installed app showed:

- Without Screen Recording, a session used to make the virtual display anyway, so a window moved onto it vanished. TDisplay now
  checks first, and makes no display until it's allowed.
- `CGPreflightPostEventAccess` keeps its first answer for the life of the process, so an Accessibility permission given while
  running went unnoticed. `AXIsProcessTrusted` asks again each time; with it, the pen started working 46 ms after being allowed.
- The app's own request for permission to post input adds one "TDisplay" row to the Accessibility list, and adding the app with
  **+** adds another. Either one lets it drive the pen.

## Pitfalls to plan around

- `CGVirtualDisplay` is undocumented, so any macOS update can break it, and apps that use it can't go on the App Store. The
  supported alternative, a DriverKit display driver, needs an entitlement Apple grants on request.
- Never change a virtual display's mode once it exists. According to the go-macos/virtualdisplay notes, a display whose mode was
  changed can't be removed until the process exits. To change size or rate, remove the display and make a new one.
- macOS remembers the last mode for each display identity (vendor, product and serial number). Derive the serial number from the
  size, so a different tablet gets a fresh identity.
- Removal finishes after the call returns. Wait until the display is gone before making a new one. Stop the capture stream and
  close any window on the display first.
- The process that made a display can't read that display's modes with `CGDisplayCopyDisplayMode` once it has listed the
  displays. Read them from another process if they're needed.
- Keep VideoToolbox out of real-time mode (`kVTCompressionPropertyKey_RealTime` false). In real-time mode the encoder slows to
  whatever just keeps up, which nearly triples the delay.
- An `NSScreen.displayLink` on the virtual display ticked at 60 Hz when the display ran at 144. A `CVDisplayLink` follows the
  real rate.
- On battery, macOS composes and captures the virtual display at about 120 frames a second instead of 140 or so. Measure on the
  charger, and say which.
- Moving a virtual display with `CGConfigureDisplayOrigin` is a configuration transaction like a mode change: done in the process
  that made the display, at any scope, the display can't be removed until that process exits. Done from another, short-lived
  process, it's fine (`tdisplay-feasibility arrange left child` shows both). TDisplay runs a copy of itself as `--place id x y`.
- macOS keeps a built-in screen resolution per combination of screens, so adding the virtual display can change the built-in
  screen's resolution, and removing it changes it back.
- It needs a logged-in user session. Run it as a login item, not a background daemon.
- Each new display leaves a color profile in `/Library/ColorSync/Profiles/Displays`. Reuse the same names and sizes.
- The Screen Recording and Accessibility permissions belong to the app's signature. An ad-hoc signature changes with every build, so
  macOS would ask again after each one. A self-signed certificate on the Mac keeps them across builds; switching from ad hoc to it
  needs the old entries cleared once (`tccutil reset All com.tdisplay.TDisplay`). The app only appears in the Screen Recording list
  once it asks for itself, and adding a rebuilt app with **+** at the same path only changes the old entry, which keeps the old
  signature.
- The tablet app needed no change for the plan itself. Touch gestures, added afterwards, did: the tablet app is now built on the
  Mac (`build-tablet-app.sh`) and signed with a key made there, not the Windows PC's (`android/signing.keystore`, left out of the
  zip on purpose), so moving a tablet between a PC-built and a Mac-built app means uninstalling it first. The version code stays 4,
  so the Windows program accepts the Mac build.
- Pinch and rotate have no public API: they're gesture events (type 29) with undocumented fields, learned from a real trackpad
  (see `app/README.md`). A macOS update could change them; `tdisplay-feasibility gestures` records a trackpad's again.

## Measuring it against Windows

Use the same kind of test patterns (a moving desktop, small text, a gray ramp and color bars) and compare the statistics line:
encode time, capture to ack, decode, frames per second. On the Windows laptop at 2944×1840 and 144 Hz, NVENC measured 3.3–3.5 ms
encode, 17.2–17.5 ms capture to ack and about 137 fps. The Intel route measured 13–15 ms, 29–31 ms and 57–73 fps.

## Sources

- [go-macos/virtualdisplay](https://pkg.go.dev/github.com/go-macos/virtualdisplay): the `CGVirtualDisplay` classes, the 1–240 Hz
  range it accepts, and the pitfalls above. It was tested on macOS 26.6.2 on Apple silicon.
- [Apple: kVTVideoEncoderSpecification_EnableLowLatencyRateControl](https://developer.apple.com/documentation/videotoolbox/kvtvideoencoderspecification_enablelowlatencyratecontrol):
  macOS 11.3 and newer, no B-frames, an endless GOP and High profiles.
- [Apple: Encoding video for low-latency conferencing](https://developer.apple.com/documentation/videotoolbox/encoding-video-for-low-latency-conferencing)
- [Apple: SCStreamConfiguration queueDepth](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/queuedepth)
