#!/bin/bash
# Builds the feasibility tool, and once, build/TDisplayCaptureTest.app: a launcher for it that macOS asks the Screen
# Recording permission for, rather than for whatever started the tool. Run the capture test through it:
#   open -W --stdout out.txt --stderr err.txt build/TDisplayCaptureTest.app --args capture 144 10 hevc
# The launcher is only rebuilt with --app, because the permission has to be given again after that.
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release
app=build/TDisplayCaptureTest.app
if [[ -d "$app" && "${1:-}" != "--app" ]]; then exit 0; fi
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
clang -O2 -framework CoreGraphics -o "$app/Contents/MacOS/launcher" Launcher.c
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>com.tdisplay.capture-test</string>
    <key>CFBundleName</key><string>TDisplay Capture Test</string>
    <key>CFBundleExecutable</key><string>launcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>LSMinimumSystemVersion</key><string>26.0</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF
codesign --force --sign - --identifier com.tdisplay.capture-test "$app"
echo "built $app; give it the Screen Recording permission again"
