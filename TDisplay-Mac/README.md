# TDisplay for Mac

The Mac version of TDisplay: a menu bar app that makes a virtual display, streams it to the tablet, and turns the pen and
two-finger gestures into Mac input. Apple silicon, macOS 15 or newer.

- `mac/app/`: the menu bar app (Swift). Its [README](mac/app/README.md) covers building, packaging, signing and how each part
  works.
- `mac/build-tablet-app.sh`: builds the tablet app from `android/` into `dist/TDisplay.apk`.
- `mac/feasibility/`: the measurements made before the app was written, with their results.
- `mac/PLAN.md`: how the Windows version's design carried over to the Mac.
- `android/`: the tablet app as built for the Mac (**TDisplay**, `com.tdisplay`). It reports fingers for the Mac's gestures.

## Building

You need the command-line developer tools (`xcode-select --install`). These go into `mac/tools/`, which isn't in the
repository:
- Google's platform-tools for macOS, unpacked into `mac/tools/platform-tools`.
- Temurin JDK 21 in `mac/tools/jdk`.
- Android build-tools 35 and platform 35 in `mac/tools/android-sdk`.

Then build the tablet app, then package the Mac app into `dist/TDisplay-mac.dmg`:

```bash
mac/build-tablet-app.sh
```

```bash
mac/app/package.sh
```
