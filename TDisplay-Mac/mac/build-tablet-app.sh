#!/bin/bash
# Builds the tablet app from android/ on this Mac, into dist/TDisplay.apk, with the JDK and Android build tools in
# mac/tools (jdk, android-sdk/build-tools/35.0.0, android-sdk/platforms/android-35). No Gradle: it compiles, dexes,
# packages and signs with the plain tools.
#
# It's signed with a key made on this Mac the first time, kept with its password under
# ~/Library/Application Support/TDisplay/signing and never in the project. The Windows PC signs with a key of its own, so a
# tablet with the PC's build must have the app uninstalled before this one goes on, and the other way round.
#
# The version code stays 4, the protocol version, which is all the Windows program checks; the version name says which
# build this is, and the Mac program checks that too.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(pwd)
versionName="1.5"
export JAVA_HOME="$root/mac/tools/jdk/Contents/Home"
tools="$root/mac/tools/android-sdk/build-tools/35.0.0"
platform="$root/mac/tools/android-sdk/platforms/android-35/android.jar"
signing="$HOME/Library/Application Support/TDisplay/signing"
keystore="$signing/android.keystore"
out="$root/mac/build/tablet-app"

if [[ ! -f "$keystore" ]]; then
    mkdir -p "$signing"
    chmod 700 "$signing"
    (umask 077 && openssl rand -hex 24 > "$signing/android.password")
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$keystore" -storetype PKCS12 -alias tdisplay -keyalg RSA -keysize 3072 \
        -validity 10000 -dname "CN=TDisplay" -storepass "$(cat "$signing/android.password")" >/dev/null
    chmod 600 "$keystore"
    echo "made a signing key in $keystore"
fi

rm -rf "$out"
mkdir -p "$out/classes" "$out/dex"
"$tools/aapt2" compile --dir android/res -o "$out/res.zip"
"$tools/aapt2" link -o "$out/unsigned.apk" -I "$platform" --manifest android/AndroidManifest.xml \
    --min-sdk-version 30 --target-sdk-version 35 --version-code 4 --version-name "$versionName" "$out/res.zip"
"$JAVA_HOME/bin/javac" -source 11 -target 11 -Xlint:-options -encoding UTF-8 -classpath "$platform" -d "$out/classes" android/java/com/tdisplay/*.java
"$tools/d8" --release --min-api 30 --lib "$platform" --output "$out/dex" $(find "$out/classes" -name '*.class')
(cd "$out/dex" && zip -q "$out/unsigned.apk" classes.dex)
"$tools/zipalign" -p -f 4 "$out/unsigned.apk" "$out/aligned.apk"
"$tools/apksigner" sign --v4-signing-enabled false --ks "$keystore" --ks-pass "file:$signing/android.password" --out "$root/dist/TDisplay.apk" "$out/aligned.apk"
"$tools/apksigner" verify "$root/dist/TDisplay.apk"
echo "built dist/TDisplay.apk, version $versionName"
