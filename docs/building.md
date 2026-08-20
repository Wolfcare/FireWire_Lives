# Building

## The core, tests and tools

These need nothing beyond the Command Line Tools, which are already installed:

```
make test     # 119 checks
make asan     # same suite under AddressSanitizer + UBSan
make probe    # survey the machine for a 1394 OHCI controller
```

`make probe` exits 2 when no controller is attached. That is informational, not
an error.

## The dext

**Blocked on Xcode.** The DriverKit SDK ships only inside Xcode; the Command
Line Tools do not include it:

```
$ ls -d /Library/Developer/CommandLineTools/SDKs/DriverKit*
no matches found
```

So `src/dext/` has never been compiled. It is written against the OHCI 1.1
specification and the documented PCIDriverKit API, and every call that could
not be checked against a real SDK is marked `VERIFY` in the source.

To unblock:

1. Install Xcode from the App Store (~10 GB), then
   `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`.
2. Confirm the SDK arrived: `xcodebuild -showsdks | grep -i driverkit`.
3. Create a DriverKit target and add `src/dext/*` plus `src/core/*.c` and
   `include/`.

## Development signing

For local development, with SIP disabled and developer mode on, an ad-hoc
signature is sufficient. To get there:

1. Boot into recoveryOS (hold the power button on Apple Silicon),
   Utilities → Terminal, then `csrutil disable`, and reboot.
2. `systemextensionsctl developer on`

   This currently refuses while SIP is enabled — the tool says so explicitly —
   so it must come after step 1.
3. Sign the dext with the entitlements in
   `src/dext/FireWireLives.entitlements`.

Re-enable SIP (`csrutil enable`) when you are done developing.

### For distribution

Shipping to other people is a different matter. It needs:

- A paid Apple Developer account.
- `com.apple.developer.driverkit.transport.pci`, which Apple grants only by
  manual request against a specific team ID.
- Notarization of the containing application.

None of that is required to get the driver working locally, so it is
deliberately deferred.

## Hardware

An Apple Silicon Mac has no FireWire port. The controller only appears once an
Apple Thunderbolt-to-FireWire adapter is attached and the Thunderbolt device
has been authorised in System Settings.

Verify it enumerated with `make probe`. What you want to see is a device with
class code `0x0c0010` and **no driver attached** — unclaimed, and therefore
available for our dext to match.
