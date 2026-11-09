#!/bin/bash
# Makes a self-signed code-signing certificate for building TDisplay on this Mac. macOS ties the Screen Recording and
# Accessibility permissions to the app's signature, and an ad hoc signature is different for every build, so without
# this each new build would have to be given both again. package.sh signs with it whenever it exists.
#
# It lives in a keychain file of its own under ~/Library/Application Support/TDisplay/signing, with a random password
# beside it, apart from the login keychain and out of the keychain search list. Nothing trusts it: it only tells macOS
# that later builds made here are the same app. To get rid of it, delete that folder.
set -euo pipefail
dir="$HOME/Library/Application Support/TDisplay/signing"
keychain="$dir/signing.keychain-db"
name="TDisplay Local Signing"
if [[ -f "$keychain" ]]; then
    echo "already made: $keychain"
    exit 0
fi
mkdir -p "$dir"
chmod 700 "$dir"
password=$(openssl rand -hex 24)
(umask 077 && printf '%s' "$password" > "$dir/password")

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
openssl req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes -keyout "$work/key.pem" -out "$work/cert.pem" -subj "/CN=$name" \
    -addext "basicConstraints=critical,CA:false" -addext "keyUsage=critical,digitalSignature" -addext "extendedKeyUsage=critical,codeSigning" 2>/dev/null
# The keychain tools only read the older PKCS#12 encryption.
openssl pkcs12 -export -legacy -inkey "$work/key.pem" -in "$work/cert.pem" -name "$name" -out "$work/identity.p12" -passout "pass:$password"

searchList=$(security list-keychains -d user | tr -d '"')
security create-keychain -p "$password" "$keychain"
# create-keychain adds it to the search list; put the list back as it was.
security list-keychains -d user -s $searchList
security set-keychain-settings "$keychain"
security unlock-keychain -p "$password" "$keychain"
security import "$work/identity.p12" -k "$keychain" -P "$password" -T /usr/bin/codesign >/dev/null
# Lets codesign use the key without asking.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$keychain" >/dev/null
echo "made $name in $keychain"
