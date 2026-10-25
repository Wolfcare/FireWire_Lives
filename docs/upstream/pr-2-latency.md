MOTU: per-rate safety offset, the UltraLite mk3's vendor latency, and a per-rate pin against MOTU's driver

Stacked on the clock source PR.

## Problem and change

`MotuProfile` declared a 64-frame safety offset and 128-frame latency at every rate. Fixed frames shrink in time as the rate rises: 64 frames is 1.45 ms at 44.1 kHz but 0.33 ms at 192 kHz. The offset now doubles per rate mode (64/128/256), the way MOTU's own driver's does (24/48/96), so the margin keeps its duration; 1x is unchanged.

The UltraLite mk3 now declares the latency MOTU's driver declares for it, read from Core Audio on macOS 15.8 (1.6 build 89555): input 19/22/25, output 20/24/28 frames at 1x/2x/4x. This follows the Duet profile, which takes its vendor kext's table. Other MOTU models keep 128.

A new test feeds the clock status MOTU's driver left on the unit at each of the six rates and requires ASFW's channels to equal, in order, what MOTU's driver published in Core Audio at that rate (14/14, 12/14, 10/12). They already do; this rules out a geometry or naming mismatch at 88.2–192 kHz before anyone runs those rates on hardware.

## Prior discussion

#192.

## Architecture and scope

`MotuProfile.cpp` only, plus `ProfileTimingPinTable.inc`: the 51 MOTU rows this changes are updated with the reason; no other profile's timing moves.

## Testing and evidence

- Host tests: 3 profile tests, the per-rate vendor pin, the timing pin table. Full suite passes.
- Vendor values captured on macOS 15.8, Apple Silicon, UltraLite mk3.
- **Not tested under ASFW on hardware**, in particular whether 64 frames at 1x is still the right floor. A loopback measurement on hardware should tune it.

## Regression risks

Every MOTU model: larger safety offsets at 2x and 4x only. 1x is unchanged.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
