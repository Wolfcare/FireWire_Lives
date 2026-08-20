#!/bin/bash
#
# build-dext.sh - build FireWireLives.dext from source.
#
# Deliberately not an .xcodeproj: the pipeline is iig -> clang -> ld -> bundle
# -> codesign, and spelling it out here keeps the build reproducible from a
# terminal and reviewable in a diff.

set -euo pipefail

XCODE=/Applications/Xcode.app/Contents/Developer
DK="$XCODE/Platforms/DriverKit.platform/Developer/SDKs/DriverKit.sdk"
IIG="$XCODE/Toolchains/XcodeDefault.xctoolchain/usr/bin/iig"
TARGET=arm64-apple-driverkit25.5

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/dext"
BUNDLE="$ROOT/build/FireWireLives.dext"
NAME=FireWireLives

[ -d "$DK" ] || { echo "DriverKit SDK not found at $DK - is Xcode installed?"; exit 1; }

mkdir -p "$OUT"
cd "$ROOT"

echo "==> iig"
"$IIG" --def src/dext/FireWireLivesOHCI.iig \
       --header "$OUT/FireWireLivesOHCI.h" \
       --impl "$OUT/FireWireLivesOHCI.iig.cpp" \
       --deployment-target 25.5 \
       -- -isysroot "$DK" -x c++ -std=gnu++17 -D__IIG=1 \
          -F"$DK/System/DriverKit/System/Library/Frameworks" \
          -I"$DK/System/DriverKit/usr/include"

CXXFLAGS="-x c++ -std=gnu++17 -target $TARGET -isysroot $DK
          -Iinclude -I$OUT -Isrc/dext
          -fno-exceptions -fno-rtti -fno-common -Wall -Wextra"
CFLAGS="-x c -std=c11 -target $TARGET -isysroot $DK -Iinclude
        -fno-common -Wall -Wextra"

echo "==> compiling generated glue"
clang++ $CXXFLAGS -c "$OUT/FireWireLivesOHCI.iig.cpp" -o "$OUT/iigglue.o"

echo "==> compiling driver"
clang++ $CXXFLAGS -c src/dext/FireWireLivesOHCI.cpp -o "$OUT/driver.o"

echo "==> compiling portable core"
CORE_OBJS=""
for f in fw_crc fw_selfid fw_configrom fw_packet fw_transaction fw_sbp2; do
    clang $CFLAGS -c "src/core/$f.c" -o "$OUT/$f.o"
    CORE_OBJS="$CORE_OBJS $OUT/$f.o"
done

echo "==> linking"
clang++ -target $TARGET -isysroot "$DK" \
    -framework DriverKit -framework PCIDriverKit \
    "$OUT/driver.o" "$OUT/iigglue.o" $CORE_OBJS \
    -o "$OUT/$NAME"

echo "==> assembling the bundle"
# A dext is a flat bundle: the executable and Info.plist sit at the top level,
# unlike an app's Contents/MacOS layout.
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp "$OUT/$NAME" "$BUNDLE/$NAME"
cp src/dext/Info.plist "$BUNDLE/Info.plist"

echo "==> signing"
# Ad-hoc signing is enough for local development once SIP is disabled and
# `systemextensionsctl developer on` is set. Distribution needs a Developer ID
# plus Apple granting com.apple.developer.driverkit.transport.pci.
codesign --force --sign - \
         --entitlements src/dext/FireWireLives.entitlements \
         --timestamp=none \
         "$BUNDLE"

echo
echo "Built $BUNDLE"
codesign -d --entitlements - "$BUNDLE" 2>&1 | grep -E "driverkit" | sed 's/^/  entitlement: /' || true
echo "  arch: $(lipo -archs "$BUNDLE/$NAME")"
echo "  size: $(stat -f%z "$BUNDLE/$NAME") bytes"
