#!/bin/bash
# Builds TDisplay, and once, build/TDisplay.app: a development launcher for it that macOS gives the Screen Recording
# permission to, so the program can be rebuilt without asking again. Start it with:
#   open build/TDisplay.app
# The launcher is only rebuilt with --app, after which the permission has to be given again (remove the old entry in
# System Settings first). Step 5 of mac/PLAN.md replaces this with a real, signed app.
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release
app=build/TDisplay.app
if [[ -d "$app" && "${1:-}" != "--app" ]]; then exit 0; fi
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
clang -O2 -framework CoreGraphics -o "$app/Contents/MacOS/launcher" Launcher.c
cat > "$app/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>com.tdisplay.development</string>
    <key>CFBundleName</key><string>TDisplay Development</string>
    <key>CFBundleExecutable</key><string>launcher</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.2</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF
codesign --force --sign - --identifier com.tdisplay.development "$app"
echo "built $app; give TDisplay Development the Screen Recording permission"
