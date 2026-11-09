# TDisplay

Use an Android tablet as a second screen for your Windows PC or Mac, over a USB cable, with as little delay as possible.
If the tablet has a pen, it works as a real pen on the computer, with pressure and tilt. Two fingers scroll, zoom and rotate.

## Download

Get the latest version from the [Releases page](../../releases/latest):

- **Windows:** `TDisplay-Windows.zip`. Unzip it and run `TDisplaySetup.exe`.
- **Mac:** `TDisplay-Mac.zip`. Unzip it and open `TDisplay.dmg`.

Each zip has a `README.txt` with the full instructions. Everything the program needs is inside the installer, including the
tablet app.

## What you need

| | Windows | Mac |
|---|---|---|
| Computer | 64-bit Windows 10 (1903 or newer) or Windows 11, Intel or AMD processor | Apple silicon (M1 or newer), macOS 15 or newer |
| Graphics | NVIDIA, Intel or AMD with hardware video encoding | Built in |
| Tablet | Android 11 or newer | Android 11 or newer |
| Cable | USB, with data (USB 3 is best) | USB, with data (USB 3 is best) |

## Getting started

1. On the tablet, turn on **USB debugging**: in Settings > About tablet, tap "Build number" seven times, then turn on
   USB debugging in Developer options.
2. Install TDisplay on your computer.
3. Plug the tablet in and tap **Allow** on it. The tablet app installs and opens by itself, and the tablet becomes a second
   screen.

The installers aren't signed with paid certificates, so the first time Windows says "Windows protected your PC" (click
**More info**, then **Run anyway**) and macOS won't open the app until you click **Open Anyway** in System Settings >
Privacy & Security. The README in each zip walks through it.

## Features

| | Windows | Mac |
|---|---|---|
| Pen with pressure and tilt | Yes | Yes |
| Two fingers: scroll, pinch to zoom, twist to rotate | Yes | Yes |
| Two-finger tap to undo, three-finger tap to redo | Yes | No |
| Tablet menu: instant cursor, palm filter, edge lock, statistics | Yes | No |
| Tablet on any side of the main screen | Yes, in Windows' display settings | Yes, from the menu |
| Mirroring the main screen | Not tried | Yes, from the menu |
| Starts by itself | With Windows | At login, if switched on |

One tablet can be used with both: the Windows and Mac versions each install their own tablet app (**TDisplay PC** and
**TDisplay**).

## Tested on

So far TDisplay has been tried on one Windows laptop (NVIDIA and Intel graphics), one MacBook (M5, macOS 27) and a
Lenovo Idea Tab Pro. Other tablets, AMD graphics and older versions of macOS should work but haven't been tried yet.
Reports of how it does elsewhere are welcome in the Issues tab.

## Source code

The two versions are separate programs that share the same idea and the same tablet code:

- [`TDisplay-Windows`](TDisplay-Windows): the Windows program, its tablet app (**TDisplay PC**) and the setup. Building it
  only needs Windows: `scripts\setup-toolchain.ps1` downloads the compilers, the Android build tools and the virtual
  display driver into the project, and `scripts\package.ps1` builds `dist\TDisplaySetup.exe`. Its README explains how it
  works and how it was measured.
- [`TDisplay-Mac`](TDisplay-Mac): the Mac menu bar app and its tablet app (**TDisplay**), built on a Mac with the
  command-line developer tools. See its README.

Each build signs its tablet app with a key made on that computer the first time, so a tablet that has the app from the
downloads needs it uninstalled before a self-built one can go on.

## License

TDisplay is released under the [MIT license](LICENSE).

## Credits

- The extra monitor on Windows is the open-source [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
  (MIT license), used unmodified.
- Both versions include Google's `adb` from the Android SDK platform-tools to talk to the tablet over USB. Its license notice
  is in each zip's Licenses folder.
