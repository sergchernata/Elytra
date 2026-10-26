#!/bin/bash
# CI: signs a release's zipped Ladybird.app with Elytra's certificate (see elytra-codesign-app.sh for why), and zips it
# again in place. Runs on a throwaway macOS runner: it trusts the certificate for code signing system-wide there.
#
# Usage: Meta/elytra-codesign-release.sh <Ladybird-....zip>
# Environment: ARC_CODESIGN_P12 (the certificate and its private key, PKCS#12, base64) and ARC_CODESIGN_PASSWORD.
# The certificate itself is Meta/elytra-codesign.pem; the key must be its.
set -euo pipefail
ZIP="$1"
HERE="$(cd "$(dirname "$0")" && pwd)"
CERTIFICATE="$HERE/elytra-codesign.pem"

if [[ -z "${ARC_CODESIGN_P12:-}" || -z "${ARC_CODESIGN_PASSWORD:-}" ]]; then
    echo "::error::The ARC_CODESIGN_P12 and ARC_CODESIGN_PASSWORD secrets aren't set; refusing to publish an app macOS can't recognize across updates."
    exit 1
fi

WORK="$(mktemp -d)"
KEYCHAIN="$WORK/signing.keychain-db"
KEYCHAIN_PASSWORD="$(openssl rand -hex 24)"
cleanup() {
    security delete-keychain "$KEYCHAIN" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

(umask 077; printf '%s' "$ARC_CODESIGN_P12" | base64 --decode > "$WORK/identity.p12")
security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security set-keychain-settings "$KEYCHAIN" # No auto-lock.
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN"
security import "$WORK/identity.p12" -k "$KEYCHAIN" -P "$ARC_CODESIGN_PASSWORD" -T /usr/bin/codesign >/dev/null
rm -f "$WORK/identity.p12"
security set-key-partition-list -S apple-tool:,apple: -s -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN" >/dev/null
security list-keychains -d user -s "$KEYCHAIN" $(security list-keychains -d user | tr -d '"')

# codesign uses only certificates trusted for code signing. Setting trust needs an authorization no one is there to
# give on a runner, so allow it first.
sudo security authorizationdb write com.apple.trust-settings.admin allow >/dev/null
sudo security add-trusted-cert -d -r trustRoot -p codeSign -k "$KEYCHAIN" "$CERTIFICATE"

# The identity is the committed certificate's, or nothing is signed.
FINGERPRINT="$(openssl x509 -in "$CERTIFICATE" -noout -fingerprint -sha1 | cut -d= -f2 | tr -d ':')"
if ! security find-identity -v -p codesigning "$KEYCHAIN" | grep -q "$FINGERPRINT"; then
    echo "::error::The ARC_CODESIGN_P12 secret isn't the key of Meta/elytra-codesign.pem."
    security find-identity -p codesigning "$KEYCHAIN"
    exit 1
fi

ditto -x -k "$ZIP" "$WORK/unpacked"
APP="$WORK/unpacked/Ladybird.app"
test -d "$APP"
"$HERE/elytra-codesign-app.sh" "$APP" "$FINGERPRINT" "$KEYCHAIN"

# macOS now knows the app by the certificate (so by every release signed with it), not by this build's hash.
REQUIREMENT="$(codesign -d -r- "$APP" 2>&1 | sed -n 's/^designated => //p')"
echo "Designated requirement: $REQUIREMENT"
LOWER_FINGERPRINT="$(echo "$FINGERPRINT" | tr 'A-F' 'a-f')"
if [[ "$REQUIREMENT" != *"$FINGERPRINT"* && "$REQUIREMENT" != *"$LOWER_FINGERPRINT"* ]] || [[ "$REQUIREMENT" == *cdhash* ]]; then
    echo "::error::The signed app's designated requirement doesn't name the certificate."
    exit 1
fi

rm -f "$ZIP"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"
shasum -a 256 "$ZIP" > "$ZIP.sha256"
