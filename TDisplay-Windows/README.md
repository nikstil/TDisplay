# TDisplay

Turns an Android tablet into a second monitor for a Windows laptop, over a USB cable, with pen input and two-finger gestures.

Built on a Lenovo Idea Tab Pro (2944×1840, up to 144 Hz) and a laptop with Intel and NVIDIA graphics. It needs graphics with a
hardware video encoder (NVIDIA with a current driver, Intel or AMD), Windows 10 version 1903 or newer, and Android 11 or newer on
the tablet. See [Video encoders](#video-encoders) for how these compare. The Mac version is a separate
program; its code is in the `TDisplay-Mac` folder next to this one.

## Giving it to someone else

This builds one file, `dist\TDisplaySetup.exe`, with the laptop program, the virtual monitor driver and the tablet app inside it:

```bash
powershell -ExecutionPolicy Bypass -File scripts\package.ps1
```

They run it and approve the administrator prompt, and there's nothing to choose. It first checks that the PC has graphics with a
video encoder TDisplay can use and refuses politely if not, and on a PC with two kinds of graphics it puts the virtual monitor on
the one it chose. Then it installs into `C:\Program Files\TDisplay`, starts the program and tells them how to turn on
USB debugging, which is the one step only the tablet's owner can do. The program installs the app on each tablet by itself, adds
that tablet's screen size to the driver, and starts with Windows from then on. It's removed from Settings → Apps.

The setup isn't code-signed, so Windows SmartScreen warns about it until they click **More info → Run anyway**. Signing it needs
a paid certificate. `setup\Getting started.txt` is the help that gets installed with it. `release\TDisplaySetup.exe` is the last build
that passed a true first-time install, using `scripts\make-pc-look-new.cmd` to put the PC and tablet back to new first.

## Using it

1. Start `dist\tdisplay-host.exe`. A small tablet icon appears in the system tray.
2. Plug in the tablet. The app installs and opens by itself, and the tablet becomes an extra monitor.

The tray icon is grey while waiting, green when connected and orange if something needs attention. Hover over it for the
details, or click it for the menu, where you can turn on **Start with Windows**.

The pen works as a Windows pen. Two fingers act as two fingers on a Windows touchscreen, so programs scroll, zoom and rotate
with them as they would on a touch laptop. As in the Mac version, the laptop first decides what the fingers are doing:
moving together (pan, after about 3 mm), apart or together (zoom, about 3 mm) or twisting (rotate, about 4 mm and 10
degrees). The first past its threshold owns the gesture until the fingers lift, and Windows only gets that movement: a twist
arrives with the fingers exactly as far apart as they started. Photoshop treats two fingers as zoom first, so without this a
twist usually zoomed instead of rotating. Nothing reaches Windows before a gesture is decided, so a quick two-finger tap
(under 0.3 s, moving under 3 mm) is sent as **Ctrl+Z** (undo) instead, and a quick three-finger tap as redo: **Ctrl+Shift+Z**,
or **Ctrl+Y** when Clip Studio Paint, Paint, Paint.NET, SAI, Notepad or Office is in front (Ctrl+Y in Photoshop toggles
proof colors, so it isn't the default). Lenovo's tablets keep three-finger gestures for themselves, so the three-finger tap
only reaches TDisplay once those are turned off in the tablet's settings. Nothing else gets through: one finger, three or more (beyond the tap), palms, and any finger while
the pen is touching, hovering or was there in the last half second do nothing, so a hand resting on the screen while drawing
can't move, click or draw. **Two-finger gestures** in the tray menu turns the fingers off entirely. The tablet app sends the
same finger reports as the Mac version's, which turns them into trackpad gestures instead.

The tablet has a menu of its own: tap the small bar at the top of the screen with a finger. The pen goes straight through
the bar to Windows. (Three-finger gestures were the first idea, but Lenovo's tablet keeps those for itself, for split screen
and screenshots.) Its switches are remembered on the tablet:
- **Instant cursor** (on): the tablet draws a small ring under the pen tip itself, the moment the pen moves, so it leads the
  laptop's picture by the stream's delay. A dot in the middle means the pen is touching. It's drawn in a layer of its own above
  the video and measured no change in how fast frames reach the screen.
- **Palm filter** (on): a contact wider than 20 mm, or any finger that lands while the pen is near or within half a second of
  it leaving, never counts as a gesture finger, for as long as it stays down. A resting hand can't start or join a gesture.
  Three or more fingers end any gesture as canceled, so the laptop never mistakes them for two.
- **Edge lock** (on): pins the app with Android's app pinning, the only way an app can stop the swipe up from the bottom
  edge from leaving it. Back gestures from the sides are ignored too. Android may show a "pin this app?" prompt first, and on
  some tablets App pinning has to be turned on in Settings; on the Lenovo Idea Tab Pro it pinned straight away. Leave with
  **Leave TDisplay** in the menu, which unpins first, or the tablet's unpin gesture (usually swipe up and hold). On a tablet
  with a PIN or pattern, Android locks the screen whenever pinning ends, including when an update replaces the app; turning
  off "Ask for PIN before unpinning" under App pinning stops that. Because Android refuses to start apps for a moment while
  pinning ends, the laptop asks the app to open up to four times, three seconds apart. Android also won't pin an app over
  the lock screen, so after a lock the app pins itself again as soon as the tablet is unlocked. And it won't uninstall a
  pinned app (`DELETE_FAILED_APP_PINNED`), so `make-pc-look-new` unpins it first.

While it waits for the app, the laptop checks every five seconds that the app is still installed and current, and
reinstalls it if not, the same as on a replug. A failed reinstall is tried up to three times before the tray shows it as a
problem, since Android can still be finishing a removal.
- **Statistics** (off): frame rate, data rate, decoding time, refresh rate, pen samples, the size of each touch, battery and
  temperature, in the top-left corner. The touch sizes are what the palm filter goes by.

The tablet app built here is **TDisplay PC** (`com.tdisplay.windows`, socket `tdisplay-windows`), and the Mac version's is
**TDisplay** (`com.tdisplay`, socket `tdisplay`), so a tablet used with both keeps both installed. The app's version code is
`kVersion * 100 + kAppRevision` from `host/src/protocol.h`; raise `kAppRevision` whenever the tablet app changes without the
protocol changing, or laptops will keep the older app.

Leave the app (**Leave TDisplay** in the tablet menu, or the home gesture) or turn the tablet's screen off and streaming stops;
open TDisplay PC on the tablet to resume. Unplug the cable and
the monitor goes away after 20 seconds, moving its windows back to your other screens. A replug within that time carries on
as if nothing happened.

Windows can put the virtual monitor back by itself, for example when the laptop wakes from sleep. TDisplay removes it again
within seconds, but only while it's running. If windows ever vanish onto an invisible screen, start TDisplay, or use Settings →
System → Display to disconnect that display. Turning on **Start with Windows** avoids this.

## For the best results

- **Turn off Battery Saver on the tablet.** While it's on, the tablet limits the screen to 60 Hz. It's easy to miss, because
  while the tablet is charging Android reports Battery Saver as off even though the limit still applies. The tray icon says
  when this is happening. To clear it without unplugging, run this on the laptop (the tablet may turn it back on at 15% battery):

  ```bash
  K:/TDisplay/dist/adb/adb.exe -P 15037 shell settings put global low_power 0
  ```
- **Use a USB 3 cable in a USB 3 port.** Many cables, including those boxed with phones, are USB 2 only. USB 2 is enough for the
  video, but USB 3 moves large frames about four times faster. A laptop data port only charges this tablet at about 2.5 W with
  either kind of cable, so with the screen changing constantly the battery still drains, by about 12% an hour in testing.
  `scripts\cable-test.ps1` tells you what a cable and port really do:

  ```bash
  powershell -ExecutionPolicy Bypass -File scripts\cable-test.ps1
  ```
- **Set the display scale in Windows.** Settings → System → Display, select the tablet's screen. Around 200% suits a 12.7"
  screen at this resolution.
- **Pen settings.** If press-and-hold gets in the way of drawing, turn off the right-click gesture in Settings → Bluetooth &
  devices → Pen & Windows Ink. If the tablet's own floating pen menu gets in the way, turn it off in the tablet's pen settings.
  If strokes come out too faint or too heavy, change `pen_pressure_curve` under [Settings](#settings).
- **Drawing apps.** Pressure and tilt arrive through Windows Ink. Most apps use it by default; in Krita choose *Windows 8+
  Pointer Input* under Settings → Tablet settings.

Pen input doesn't reach windows running as administrator (Task Manager, for example), because Windows blocks that from a
normal program. The tablet's panel also drops from 144 Hz to 120 Hz while the pen is in use, which is the tablet's own behavior.

## Settings

Create `%LOCALAPPDATA%\TDisplay\config.ini`. Every line is optional, and a change takes effect on the next connection.

```ini
[stream]
refresh=120              ; Limits the refresh rate in Hz. By default it's the tablet's highest.
stream_fps=0             ; Frames per second to send. 0 means the same as the refresh rate.
bitrate_mbps=80          ; Higher is sharper during motion, up to about 150 on a USB 2 link.
encoder=auto             ; nvenc or mf (Windows' Media Foundation encoders) to choose one. Auto uses NVENC wherever it can.
codec=auto               ; hevc or h264. H.264 only comes from the Media Foundation encoders. Auto prefers HEVC.
preset=1                 ; NVENC preset 1-4. Higher looks better and adds encoding time. NVENC only.
min_qp=22                ; Quality past which no more bits are spent. 0 turns it off.
max_frames_in_flight=0   ; Frames allowed on their way to the tablet. 0 chooses from the refresh rate.
pen_pressure_curve=50    ; Lower makes light strokes register more strongly. 100 is a straight line.
pen_pressure_minimum=6   ; What the lightest pen contact registers as, in percent.
```

## What's in the box

| Part | Where | What it does |
|---|---|---|
| Laptop program | `host/`, built to `dist\tdisplay-host.exe` | Captures the screen, encodes it on the GPU, sends it over USB, injects pen input |
| Tablet app | `android/`, built to `dist\TDisplay.apk` | Decodes and shows the video, sends pen input back |
| Monitor driver | `driver/` | Makes Windows see a real extra monitor |
| Setup | `setup/`, built to `dist\TDisplaySetup.exe` | Puts all of the above on someone else's PC |

The monitor driver is the open-source [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) (MIT
license, signed through SignPath), unmodified. Windows only loads trusted, signed drivers, so this avoids having to weaken
Windows security to load a home-built one. Its modes are set in `driver\vdd_settings.xml`.

The connection is adb over USB, using a private adb server on its own port so that other software on the laptop that bundles
adb can't disturb it. The tablet app has no network code and only accepts connections that arrive through adb.

On this tablet the USB link occasionally stops delivering until something new travels toward the tablet. When replies run more
than 45 ms late, the laptop program sends a tiny extra packet, which frees it. Each one is counted as a "nudge" in the log.

On MediaTek tablets the app draws each decoded picture with the tablet's GPU instead of handing it straight to the screen. This
display hardware can't show the decoder's YUV pictures itself: they go through its MML processor, which puts the whole screen on
a schedule about 15 ms longer, whichever way up the tablet is. If the GPU ever fails, the app goes back to the direct route until
it next restarts. While the tablet is in use (the pen is near, or something changed in the last 3 seconds) the app also redraws at
least every 40 ms, because with nothing reaching the display for about 50 ms the tablet drops into a deep idle and the first frames
after a pause take up to 30 ms longer. That costs about 1 W, and only while in use.

## Video encoders

The picture is encoded on the GPU that the virtual monitor lives on. On NVIDIA graphics that's NVIDIA's own encoder, NVENC. On
anything else, or where NVENC won't start (an NVIDIA driver too old for it, say), it's whichever hardware encoder Windows offers for
that GPU through Media Foundation: Intel Quick Sync, AMD's, or NVIDIA's own again. Those want their input in NV12, so the program
converts each picture on the GPU first, to BT.709 limited range like NVENC. They're asked for HEVC first, and for H.264 if a GPU
can't do HEVC, which takes the tablet app's protocol version 4. The log says which one is in use.

Measured on this laptop (Intel Core Ultra 7 255HX with its Intel graphics, and an RTX 5070), with the desktop test pattern at
2944×1840 and 144 Hz:

| Encoder | Encode time | Capture to decoded on the tablet | Frames per second |
|---|---|---|---|
| NVENC on the RTX 5070 | 3.3–3.5 ms | 17.2–17.5 ms | about 137 |
| Intel, HEVC | 13–15 ms | 29–31 ms | 57–73 |
| Intel, H.264 | 15–16 ms | 28–30 ms | 51–66 |

The Intel graphics were also running the laptop's own screen, a live wallpaper and Discord at the time, which kept their 3D engine
about 80% busy. About half of the Intel encode time is the encoder itself, and the rest is waiting for that engine. Colors and text
came out the same as with NVENC. AMD's encoder hasn't been tried, because no AMD graphics were at hand.

`build\tdisplay-enctest.exe` measures any encoder on any GPU on its own, with no virtual monitor or tablet involved. For example:

```bash
build\tdisplay-enctest.exe Intel mf auto 2944 1840 144 10 80 1 desktop
```

## Logs

`%LOCALAPPDATA%\TDisplay\host.log` records every connection and disconnection, with a line of statistics every 10 seconds.
The tray menu has a shortcut to the folder.

## Building

```bash
powershell -ExecutionPolicy Bypass -File scripts\setup-toolchain.ps1
```

```bash
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

The first downloads the build tools into `tools\` and changes nothing else on the system. The second produces everything in
`dist\`.

## Installing and removing the driver

Installing asks for administrator approval and may ask you to confirm the publisher (SignPath Foundation):

```bash
powershell -ExecutionPolicy Bypass -File scripts\install-driver.ps1
```

To remove it:

```bash
powershell -ExecutionPolicy Bypass -File scripts\install-driver.ps1 -Uninstall
```

To remove TDisplay completely, also delete this folder and `%LOCALAPPDATA%\TDisplay`, turn off **Start with Windows** first if
you enabled it, and uninstall the app from the tablet.

## Running alongside SuperDisplay

Only one program can hold the tablet's USB connection. SuperDisplay's service restarts with Windows and takes it, in which case
the tray icon turns orange and says so. Stop SuperDisplay's service, or uninstall it from Settings → Apps once you're happy with
TDisplay.
