# TDisplay for Mac

The menu bar app, the Mac's counterpart of `host/` on Windows. All five steps of `../PLAN.md` are done: it streams to the tablet,
the pen draws with pressure and tilt, it copes with unplugging, sleep and the tablet app closing, and it installs as an app.

## Installing it

```bash
./package.sh
```

That makes `dist/TDisplay-mac.dmg` (and `build/package/TDisplay.app`): the app, with adb (still signed by Google, with its licence
notice) and the tablet app inside, next to a link to Applications. Drag TDisplay to Applications and open it. It lives in the menu
bar. The first time, it asks for Screen Recording, then for Accessibility (for the pen); switch **TDisplay** on for both under System
Settings → Privacy & Security. Accessibility takes effect at once; Screen Recording may need TDisplay quit and reopened, which macOS
offers. **Start at login** in its menu makes it start by itself.

It's built for Apple silicon (arm64) and declares macOS 15 as its minimum, but has only been tried on an M5 with macOS 27.2.

**Signing.** `package.sh` signs with a local self-signed certificate if `make-signing-identity.sh` has made one, which keeps the
permissions from one build to the next: macOS sees any build with this bundle ID and certificate as the same app. The certificate
lives in its own keychain file under `~/Library/Application Support/TDisplay/signing`, out of the login keychain and trusted by
nothing; delete that folder to get rid of it. Without it, the app is signed ad hoc, and every new build has to be given both
permissions again. Switching an installed app from one to the other means clearing its old entries once:

```bash
tccutil reset All com.tdisplay.TDisplay
```

**Giving it to someone else.** Their Mac will refuse an app signed either way when it comes from another computer, until they
allow it under System Settings → Privacy & Security → Open Anyway. To avoid that it needs a Developer ID certificate, from Apple's
paid developer membership:

```bash
SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" ./package.sh
```

That adds the hardened runtime and a timestamp. Then notarize the disk image and staple the ticket (`xcrun notarytool submit
../../dist/TDisplay-mac.dmg --keychain-profile <profile> --wait`, then `xcrun stapler staple` on it); none of this has been tried
yet. Whether Google's platform-tools may be passed on inside another app is the same open question as on Windows.

## Working on it

It needs the command-line developer tools, and Google's platform-tools for macOS unpacked into `mac/tools/platform-tools` (the
app finds `adb` there, and `TDisplay.apk` in `dist/`).

**The tablet app** is built here too, by `../build-tablet-app.sh`, into `dist/TDisplay.apk`, with Temurin JDK 21 in `mac/tools/jdk`
and Android build-tools 35 and platform 35 in `mac/tools/android-sdk` (no Gradle). It's signed with a key made on this Mac, kept with
its password in `~/Library/Application Support/TDisplay/signing`, not the Windows PC's key: a tablet with the PC's build has to have
the app uninstalled before this one goes on, and the other way round. The PC's build is kept as `dist/TDisplay-windows-signed.apk`.
Its version code stays 4, which is all the Windows program checks, so the Windows program works with this build; its version name,
1.5, tells the Mac program it's the one that reports fingers.

```bash
./build.sh --app
```

```bash
open build/TDisplay.app
```

`build/TDisplay.app` is a development launcher that starts `.build/release/TDisplay`, so that the permissions, which macOS ties to
the app's signature, survive rebuilds. It's listed as **TDisplay Development** under System Settings → Privacy & Security, in Screen
& System Audio Recording (for the picture) and in Accessibility (for the pen; add it there with **+**, since macOS won't list it by
itself). After the first time, `./build.sh` alone rebuilds the program and leaves the launcher and its permissions alone. Quitting
from the menu takes the virtual display away.

The log is `~/Library/Logs/TDisplay/host.log`, in the same format as Windows; the menu has a shortcut to its folder. Starting it with
`open build/TDisplay.app --env TDISPLAY_CONSOLE=1 --stdout /tmp/tdisplay.txt` copies the log to a file as well. Adding
`--env TDISPLAY_DEBUG=1` adds a line after each statistics line saying where frames went: how many ScreenCaptureKit delivered, how
many a newer one overtook before the session took them, and how often it waited for the tablet's acknowledgements.
`~/Library/Application Support/TDisplay/config.ini` takes the same `[stream]` keys as Windows, read at each connection.

## Everyday behaviour

- The menu bar icon is grey while waiting, green when connected and orange when something needs attention. The menu's first
  line says what's going on, including when Battery Saver is limiting the tablet to 60 Hz and when the pen lacks its permission.
- **Touch gestures**, on unless unticked in the menu, turn two fingers on the tablet into what a Mac trackpad sends. Moving them
  together scrolls, which pans a canvas in drawing apps; pinching zooms; twisting rotates (in Photoshop, rotating the view needs
  Settings → Tools → Enable Gestures). The pointer goes to where the fingers are. One finger does nothing, and fingers are
  ignored while the pen is near or has been in the last half second, so the hand holding it can rest on the screen.
- **Tablet screen** in the menu chooses how the tablet relates to the Mac's main screen: extending it to the right (the default),
  left, above or below, or mirroring it. A new choice takes effect within about half a second, reconnecting if need be.
  - Extending puts the virtual display against that side of the main screen, centred along it. macOS remembers where each display
    went, so the tablet comes back in the same place. Moving it by hand in System Settings → Displays works too, until the next
    connection or menu choice puts it back.
  - Mirroring makes no virtual display. The tablet shows the main screen itself, scaled to fit with bars where the shapes differ,
    and the pen points on the main screen. It doesn't use macOS's own mirroring, which can change the main screen's resolution
    to suit both.
  - macOS keeps a resolution for the built-in screen for each combination of screens. With the tablet extending, the built-in
    screen may switch to its default resolution; setting the one you want while the tablet is connected should make macOS keep it
    for that combination (not tried).
- Leaving the app on the tablet or turning its screen off ends the session, and the menu says how to come back. Unplugging the
  cable does the same. Either way the virtual display stays for 20 seconds, so coming back within that time leaves windows
  where they were; after that it goes, and macOS moves its windows to the other screens.
- A dropped connection reconnects by itself, starting the tablet app again if it isn't open. If the tablet's USB connection is
  there but adb hasn't listed it for 1.5 seconds, the adb server is restarted, as on Windows. Those 1.5 seconds count from when
  the connection appears, so plugging the tablet back in doesn't restart the server straight away.
- As the Mac goes to sleep the session ends, and when it wakes, it starts again. A dark wake, with the lid closed or the screens
  off, doesn't count as waking.
- **Start at login** in the menu registers the app with macOS (`SMAppService`). It only works for a real app bundle, so in this
  development build the item says it's for the packaged app; step 5 makes that bundle.

To measure it, `../feasibility/.build/release/tdisplay-feasibility pattern 60 desktop` covers the tablet's screen with one of the
Windows test patterns (`desktop`, `text`, `colors` or `noise`) for a minute.

`open -W --stdout /tmp/pen.txt build/TDisplay.app --args --pen-test` checks the pen without a tablet: it opens a window in the middle
of the main screen, draws strokes inside it with the real injector (a pressure ramp with the tilt sweeping across, the eraser, the
barrel button, a double tap) and prints what AppKit received. It moves the pointer for about two seconds.

## How the pieces map

| File | Windows | What it does |
|---|---|---|
| `Adb.swift` | `adb.cpp` | Talks to a private adb server on port 15037 |
| `Protocol.swift` | `protocol.h` | Wire format version 4, unchanged |
| `Display.swift` | `display.cpp` | Makes the virtual display with `CGVirtualDisplay`, kept between sessions for the 20-second grace |
| `Capture.swift` | `capture.cpp` | ScreenCaptureKit, 4:2:0 BT.709 at the tablet's size |
| `Encoder.swift` | `encoder.cpp`, `mfencoder.cpp` | HEVC on the hardware encoder, H.264 if HEVC won't start |
| `Session.swift` | `stream.cpp` | Pacing, frames in flight, pings, nudges, acks and the statistics line, ported as they are |
| `Service.swift` | `service.cpp` | Finds the tablet, installs and starts the app, runs sessions and reconnects |
| `Pen.swift` | `pen.cpp` | Turns pen samples into tablet proximity and tablet-point events |
| `PenTest.swift` | `tools/pentest.cpp` | The `--pen-test` mode |
| `Gestures.swift` | (new on the Mac) | Tells scrolling, pinching and twisting apart, and posts them as trackpad events |
| `GestureTest.swift` | (new on the Mac) | The `--gesture-test` mode: scripted two-finger gestures, and what AppKit got |
| `Arrangement.swift` | (new on the Mac) | The Tablet screen choice: which side to extend to, or mirroring |
| `LoginItem.swift` | the Run key in `main.cpp` | Start at login |
| `main.swift` | `main.cpp` | The menu bar item: grey waiting, green connected, orange when something needs attention |

## Step 2 results

Measured on 2026-11-08 on the MacBook Air (M5, macOS 27.2) with the Lenovo Idea Tab Pro over its USB 3 cable, at 2944×1840 and
144 Hz, HEVC at 80 Mbps. Each line is a 10-second statistics line from the log.

| Pattern | Sent | Encode | Capture to ack | Decode on the tablet | Link round trip | Bitrate |
|---|---|---|---|---|---|---|
| Desktop, first minute | 129–133 fps | 6.4–6.6 ms | 21.4–22.9 ms | 9.7–10.0 ms | 2.6–3.1 ms | 2.2 Mbps |
| Desktop, after 2½ minutes | 118–121 fps | 6.9–7.0 ms | 22.5–22.6 ms | 10.0–10.1 ms | 2.9–3.4 ms | 2.0 Mbps |
| Text, scrolling | 130 fps | 6.2 ms | 21.2 ms | 9.9 ms | 2.8 ms | 0.8 Mbps |
| Noise | 123 fps | 6.3 ms | 20.9 ms | 8.8 ms | 3.6 ms | 136 Mbps |

Against Windows at the same size and rate: NVENC measured 3.3–3.5 ms encode, 17.2–17.5 ms capture to ack and about 137 fps; the
Intel route 13–15 ms, 29–31 ms and 57–73 fps. The Mac comes in about 5 ms behind NVENC, nearly all of it the slower encoder, and well
ahead of Intel.

- **The picture is right.** A screenshot of the tablet taken over adb showed the Mac's desktop at full resolution, and with the
  colours pattern, the gray ramp came back within 1 of every step from 0 to 255 and the six colour bars pure. BT.709 limited range
  holds end to end.
- **Reconnecting is quick.** With the tablet app force-stopped, the Mac relaunched it and was streaming again 0.4 s later, on the
  same virtual display. Quitting removes the display within about 20 ms.
- **The frame rate is set by what macOS delivers.** At 144 Hz, TDisplay sent 120–133 fps of the desktop pattern. I first put that
  down to the encoder, which takes 6.4–7.0 ms of a 6.94 ms frame, and the ported loop, which captures and encodes one frame at a
  time. That was wrong. With `TDISPLAY_DEBUG` set, ScreenCaptureKit turned out to deliver only 115–128 frames a second while TDisplay
  streams, and the loop sent all but 1–2% of them. A version that encoded the next frame while the last was still being sent made
  no difference beyond the ±5 fps from run to run (alternating them: 128.5 and 119.5 fps for the old loop, 122.5 and 124.4 for
  the new), so it wasn't kept. A deeper capture queue (8) and more frames in flight (5) didn't help either. One open question:
  the `pattern` tool draws about 151 times a second for a 144 Hz display, so its own uneven timing may cost some of those frames;
  real app motion hasn't been measured this way.
- **On battery, fewer still.** On battery macOS delivers about 120 frames a second. Step 1's own capture test, which got 143.7 fps
  on the charger, got 125.3 on battery and 142.0 on the charger again. Capture to ack and encode times don't change.
- Sending 120 fps on purpose (`stream_fps=120`) was no better: 118.5 fps and 23.3 ms.
- **Noise overshoots the bitrate.** VideoToolbox treats the average bitrate as a target, not a limit, so random noise came out at
  136 Mbps for 80 asked. Fine over USB 3; on a USB 2 cable it would be near the limit. `DataRateLimits` was accepted but didn't
  cap anything, keyframes included (5 MB for noise, under the tablet's 8 MB limit). Real desktop content stays far below.
- **adb had to run outside Claude's sandbox** while testing: in it, the server hung before listening. The app itself, started with
  `open`, isn't sandboxed.

## Step 3 results

Tested on 2026-11-08 with the tablet's own pen in Photoshop (Beta): the pointer follows the pen in hover, taps click, and a brush set
to follow pen pressure and pen tilt does both. In 10 seconds of drawing the tablet sent up to 1,725 pen samples, raw pressure reached
0.98, and no injection took long enough to log. While the pen is near, the tablet's panel drops to 120 Hz, which is its own
behaviour, as on Windows.

What macOS receives, from `--pen-test`:

- A tablet proximity event as the pen comes into range and as it leaves, for the pen or the eraser, each with its own unique ID.
  Qt, and so Krita, ignores tablet events from a device it hasn't seen come into proximity.
- Mouse events in between, marked as tablet points, carrying pressure through the same curve as Windows (`pen_pressure_curve`,
  `pen_pressure_minimum`: 0.05 from the pen becomes 0.27) and tilt.
- Tilt as a fraction of 60 degrees each way, with the vertical axis flipped: macOS's leans away from the user where Android's leans
  toward. Qt reads it back as `tilt.y * -60`.
- With the barrel button held, a tap is a right click, as with Windows Ink. A second tap in the same place soon after is a double
  click.

The pen needs the Accessibility permission. Without it the picture still works, the pen does nothing, and the menu says what to
allow. The session checks again every second, so a permission given while connected takes effect without reconnecting. (The
first version of that check never saw a change; step 5 found and fixed it.)

## Step 4 results

Tested on 2026-11-08, each step read back from the log:

| What happened | What TDisplay did |
|---|---|
| The tablet app was left (Home key) | The session ended at once and the menu said how to come back. The display went 20.05 s later; reopening the app reconnected in 0.2 s |
| The cable was out for about 9 s | Noticed at once. The display stayed, so a window on it stayed too, and streaming resumed on it after the replug |
| The cable was out for about 30 s | The display went 20.3 s after the unplug, moving its windows to the Mac's screen. After the replug, adb found the tablet and streaming started 0.3 s later |
| Battery Saver turned on, over adb | The tablet reported it with its panel at 60 Hz, and the menu's first line said so 20 ms later. Back to normal when turned off |
| Sleep from the Apple menu, 18 s asleep | The session ended going to sleep. 0.3 s after waking it was streaming again, on the same display |
| The lid closed overnight, with a few dark wakes | It stayed asleep through the dark wakes, kept the display, and on opening the lid removed the long-idle display and reconnected |

The first replug and the overnight wake each set off an adb restart the moment the tablet reappeared, which cost 3.5 seconds:
the 1.5-second wait counted from when the tablet went missing, as it then did on Windows too. It now counts from when the tablet's
USB connection reappears, and after that change the 30-second replug reconnected without a restart. `host/src/service.cpp` got the
same change afterwards, not yet built or tried on the PC.

Start at login can only be tried in the packaged app (step 5), and the login itself only by logging out and in again.

## Step 5 results

Tested on 2026-11-08, from `dist/TDisplay-mac.dmg` installed in /Applications:

- The app found the tablet with its own adb and asked for both permissions itself. Once they were given, it streamed and the pen
  worked, as in development.
- With the local certificate, a rebuilt and reinstalled app kept both permissions and was streaming 0.5 s after starting.
- With the pen permission switched off, then on again while TDisplay ran, the pen started working 46 ms after it was given.
- Start at login registered: macOS lists `com.tdisplay.TDisplay` at /Applications/TDisplay.app as an enabled, allowed login item.
  Starting at the next login itself hasn't been seen yet.

Three fixes came out of it:

- Without Screen Recording, TDisplay used to make the virtual display anyway, so a window moved onto it vanished from sight. It
  now makes no display until it's allowed, logs that once, and checks again every second.
- The pen permission was checked with `CGPreflightPostEventAccess`, which keeps its first answer for the life of the process, so a
  permission given while running went unnoticed. `AXIsProcessTrusted` asks again each time.
- The pen permission's log line and request now come once per run instead of at every reconnect.

## Touch gestures

Added on 2026-11-08. The tablet app reports up to four fingers with every touch event, as a new message type (0x16) that the
Windows program skips. The Mac program does the rest:

- **Telling the gestures apart.** Two fingers moving together scroll; moving apart or together along the line between them
  zooms; moving past each other across it rotates. Whichever of the three is biggest decides, once it's past its threshold (8
  points together, 10 apart, 12 across and 10 degrees). Until then the gesture waits. A first version that compared each
  movement with its own threshold called most quick twists scrolls, since twisting fingers also drift about 10 points before
  they've turned 10 degrees; comparing them with each other fixed that.
- **What it posts.** Scrolling is an ordinary continuous scroll event with phases, content following the fingers. Zooming and
  rotating have no public way in. A real trackpad, recorded with `tdisplay-feasibility gestures`, sends gesture events (type 29)
  with the kind in field 110 (8 zoom, 5 rotation), the amount in field 113 and the phase in field 132 (1 began, 2 changed, 4
  ended); TDisplay posts the same, and AppKit hands apps genuine magnify and rotate events (`--gesture-test` shows them). They
  need the same Accessibility permission as the pen.
- **Tested** with fingers on the tablet in Photoshop: dragging pans, pinching zooms and twisting rotates straight away, and the
  menu switch turns them off and on. `TDISPLAY_DEBUG` logs which gesture each two-finger touch became, and why.
