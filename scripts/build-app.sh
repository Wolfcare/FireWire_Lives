#!/bin/bash
#
# build-app.sh - build the installer app that activates FireWireLives.dext.
#
# A dext cannot be loaded from the command line; it has to be embedded in an
# app bundle and activated via OSSystemExtensionRequest. This builds that app
# with the dext inside it.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/build/FireWireLivesInstaller.app"
DEXT="$ROOT/build/FireWireLives.dext"
NAME=FireWireLivesInstaller

cd "$ROOT"

echo "==> building the dext, so the app always embeds the current one"
./scripts/build-dext.sh >/dev/null
[ -d "$DEXT" ] || { echo "dext build produced nothing at $DEXT"; exit 1; }

echo "==> compiling the installer"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Library/SystemExtensions"

clang -x objective-c -fobjc-arc -mmacosx-version-min=26.0 \
      -framework Foundation -framework SystemExtensions \
      -Wall -Wextra \
      src/app/main.m -o "$APP/Contents/MacOS/$NAME"

cp src/app/Info.plist "$APP/Contents/Info.plist"

echo "==> embedding the dext"
# The embedded copy is named for its bundle identifier, which is the
# convention macOS expects when it scans Contents/Library/SystemExtensions.
cp -R "$DEXT" "$APP/Contents/Library/SystemExtensions/com.firewirelives.FireWireLives.dext"

echo "==> signing"
# Inside out: the nested dext must be signed before the app that contains it,
# or the app's seal will not cover a valid inner signature.
codesign --force --sign - --timestamp=none \
         --entitlements src/dext/FireWireLives.entitlements \
         "$APP/Contents/Library/SystemExtensions/com.firewirelives.FireWireLives.dext"

codesign --force --sign - --timestamp=none \
         --entitlements src/app/installer.entitlements \
         "$APP"

echo
echo "Built $APP"
codesign --verify --verbose=1 "$APP" 2>&1 | sed 's/^/  /'
echo
echo "To install:   $APP/Contents/MacOS/$NAME"
echo "To remove:    $APP/Contents/MacOS/$NAME deactivate"
