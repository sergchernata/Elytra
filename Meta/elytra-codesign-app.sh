#!/bin/sh
# Signs a packaged Ladybird.app with Elytra's own certificate, in place of the ad hoc signatures the packager gave it.
#
# Usage: Meta/elytra-codesign-app.sh <Ladybird.app> <identity> [keychain]
#
# Ad hoc, macOS knows the app only by its binary's hash, which every release changes: permissions given to it (file
# access, and the like) are asked for again after each update. Signed with the same certificate each time, its
# designated requirement names the certificate instead, so they're kept. The identity may be "-" (ad hoc), to try
# this out without a certificate.
#
# Every binary keeps its entitlements and flags (the helpers' hardened runtime); libraries and plugins go first, then
# the helpers, then the bundle, which signs the main executable and reseals everything.
set -eu
APP="$1"
IDENTITY="$2"
KEYCHAIN="${3:-}"
MAIN="$APP/Contents/MacOS/Ladybird"

sign() {
    set -- "$1"
    [ -n "$KEYCHAIN" ] && set -- --keychain "$KEYCHAIN" "$@"
    if ! output="$(codesign --force --sign "$IDENTITY" --timestamp=none --preserve-metadata=entitlements,flags "$@" 2>&1)"; then
        echo "$output" >&2
        exit 1
    fi
}

is_mach_o() {
    file -b "$1" | grep -q '^Mach-O'
}

find "$APP/Contents" -type f ! -path "$APP/Contents/MacOS/*" | sort | while IFS= read -r binary; do
    if is_mach_o "$binary"; then sign "$binary"; fi
done
find "$APP/Contents/MacOS" -type f | sort | while IFS= read -r binary; do
    if [ "$binary" != "$MAIN" ] && is_mach_o "$binary"; then sign "$binary"; fi
done
sign "$APP"

codesign --verify --deep --strict "$APP"
