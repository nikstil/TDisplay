#!/bin/bash
# Builds TDisplay for macOS as an app to install: build/package/TDisplay.app, with adb and the tablet app inside, and
# dist/TDisplay-mac.dmg holding it next to a link to Applications. The counterpart of scripts\package.ps1 on Windows.
#
# It's signed with the local certificate from make-signing-identity.sh if there is one, so that macOS keeps its Screen
# Recording and Accessibility permissions from one build to the next, and ad hoc otherwise, which is enough to run on
# this Mac. SIGN_IDENTITY="Developer ID Application: ..." signs it with that certificate instead, with the hardened runtime
# and a timestamp, ready for notarization; see README.md. VERSION sets the version, 1.0 by default.
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
identity="${SIGN_IDENTITY:-}"
localKeychain="$HOME/Library/Application Support/TDisplay/signing/signing.keychain-db"
version="${VERSION:-1.0}"

swift build -c release
out=build/package
app=$out/TDisplay.app
rm -rf "$out"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp .build/release/TDisplay "$app/Contents/MacOS/TDisplay"
# adb goes beside the program, keeping Google's own signature, with its licence notice.
cp ../tools/platform-tools/adb "$app/Contents/MacOS/adb"
cp ../tools/platform-tools/NOTICE.txt "$app/Contents/Resources/adb NOTICE.txt"
cp "$root/dist/TDisplay.apk" "$app/Contents/Resources/TDisplay.apk"

swiftc -O Icon.swift -o "$out/icon"
"$out/icon" "$out/AppIcon.iconset"
iconutil -c icns "$out/AppIcon.iconset" -o "$app/Contents/Resources/AppIcon.icns"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key><string>com.tdisplay.TDisplay</string>
    <key>CFBundleName</key><string>TDisplay</string>
    <key>CFBundleDisplayName</key><string>TDisplay</string>
    <key>CFBundleExecutable</key><string>TDisplay</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
    <key>LSUIElement</key><true/>
</dict>
</plist>
EOF

# Not --deep: adb is already signed, by Google, and keeps that.
if [[ -n "$identity" ]]; then
    codesign --force --options runtime --timestamp --sign "$identity" "$app"
elif [[ -f "$localKeychain" ]]; then
    security unlock-keychain -p "$(cat "$(dirname "$localKeychain")/password")" "$localKeychain"
    codesign --force --keychain "$localKeychain" --sign "TDisplay Local Signing" "$app"
else
    codesign --force --sign - "$app"
fi
codesign --verify --strict --verbose=2 "$app"

staging=$out/dmg
mkdir -p "$staging"
cp -R "$app" "$staging/"
ln -s /Applications "$staging/Applications"
mkdir -p "$root/dist"
hdiutil create -quiet -volname TDisplay -srcfolder "$staging" -format UDZO -ov "$root/dist/TDisplay-mac.dmg"
echo "built $app and $root/dist/TDisplay-mac.dmg"
