#!/bin/bash
#
# survey-macos-firewire.sh - reproduce the findings in README.md.
#
# Establishes what FireWire support, if any, the running macOS actually has.
# Every check is paired with a control so that a silently failing command
# cannot be mistaken for evidence of absence.

set -u

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '  \033[32m%s\033[0m\n' "$1"; }
warn() { printf '  \033[33m%s\033[0m\n' "$1"; }

bold "System"
sw_vers | sed 's/^/  /'
printf '  arch: %s\n' "$(uname -m)"

bold $'\nFireWire kext bundles on disk'
found_bundles=0
for k in IOFireWireFamily IOFireWireAVC IOFireWireSBP2 \
         IOFireWireSerialBusProtocolTransport; do
    bundle="/System/Library/Extensions/$k.kext"
    [ -d "$bundle" ] || continue
    found_bundles=$((found_bundles + 1))
    if [ -f "$bundle/Contents/MacOS/$k" ]; then
        ok "$k: bundle present WITH binary"
    else
        warn "$k: bundle present, NO binary (codeless husk)"
    fi
done
[ "$found_bundles" -eq 0 ] && warn "no FireWire kext bundles at all"

bold $'\nKernel collection contents'
# The control matters: if kmutil fails or the flags are wrong, the FireWire
# lookup would return "absent" for the wrong reason.
total=$(kmutil inspect --show-kext-uuids -a arm64e 2>/dev/null | wc -l | tr -d ' ')
if [ "$total" -eq 0 ]; then
    warn "kmutil returned nothing - cannot draw a conclusion from this check"
else
    ok "$total kexts in the loaded collections"
    control=$(kmutil inspect --show-kext-uuids -a arm64e 2>/dev/null \
              | grep -c -E "IOPCIFamily|IOStorageFamily")
    if [ "$control" -eq 0 ]; then
        warn "control check failed - kmutil output is not what we expect"
    else
        ok "control: IOPCIFamily / IOStorageFamily found ($control)"
        fw=$(kmutil inspect --show-kext-uuids -a arm64e 2>/dev/null \
             | grep -c -i -E "firewire|fwohci|1394")
        if [ "$fw" -eq 0 ]; then
            warn "NO FireWire kext in any kernel collection"
        else
            ok "$fw FireWire kext(s) present"
        fi
    fi
fi

bold $'\nSDK headers'
# Several of these SDK entries are symlinks (MacOSX.sdk and MacOSX26.sdk both
# point at MacOSX26.5.sdk). find must be told to follow them with -L, or it
# stats the link and reports zero - which reads exactly like "the headers were
# removed" and is wrong.
for sdk in /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk; do
    [ -e "$sdk" ] || continue
    target=$(readlink "$sdk" || true)
    # Dedupe by inode: the SDK also symlinks Versions/Current -> Versions/A,
    # so a naive count double-reports every header.
    n=$(find -L "$sdk" -ipath "*firewire*" -type f -print0 2>/dev/null \
        | xargs -0 stat -f '%i' 2>/dev/null | sort -u | wc -l | tr -d ' ')
    if [ -n "$target" ]; then
        printf '  %-18s -> %-16s %s FireWire header(s)\n' \
            "$(basename "$sdk")" "$target" "$n"
    else
        printf '  %-18s %-19s %s FireWire header(s)\n' \
            "$(basename "$sdk")" "" "$n"
    fi
done
warn "headers survive; it is the driver binaries that were removed"

bold $'\nRuntime IOKit state'
if ioreg -c IOFireWireController 2>/dev/null | grep -q IOFireWireController; then
    ok "IOFireWireController is registered - a native stack is running"
else
    warn "IOFireWireController absent"
fi

bold $'\nPCIDriverKit viability'
ref=/System/Library/DriverExtensions/com.apple.DriverKit-Apple16X50PCI.dext
if [ -d "$ref" ]; then
    ok "Apple ships a PCIDriverKit dext: $(basename "$ref")"
    bin=$(find "$ref" -type f -perm +111 2>/dev/null | head -1)
    if [ -n "$bin" ]; then
        printf '    archs: %s\n' "$(lipo -archs "$bin" 2>/dev/null)"
        otool -L "$bin" 2>/dev/null | grep -o 'PCIDriverKit' | head -1 \
            | sed 's/^/    links: /'
        codesign -d --entitlements - --xml "$bin" 2>/dev/null \
            | plutil -p - 2>/dev/null | grep driverkit | sed 's/^/    /'
    fi
else
    warn "no reference PCIDriverKit dext found on this system"
fi

bold $'\nSecurity posture (development requirements)'
csrutil status 2>&1 | sed 's/^/  /'
systemextensionsctl developer 2>&1 | head -2 | sed 's/^/  /'

bold $'\nAttached 1394 OHCI controllers'
if [ -x build/fwprobe ]; then
    ./build/fwprobe | sed -n '/PCI devices/,$p' | sed 's/^/  /'
else
    warn "build/fwprobe not built yet - run 'make probe'"
fi
