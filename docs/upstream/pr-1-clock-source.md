MOTU: clock source selection for the UltraLite mk3

## Problem and change

The UltraLite mk3 (FireWire, unit version `0x19`) has no clock source control under ASFW, so it can only run on its internal clock. This adds a Core Audio clock source with the three choices MOTU's own driver offers: Internal, S/PDIF and SMPTE.

Linux only decodes the V3 source, so the write path was observed on hardware instead. With MOTU's driver (MOTUFireWireAudio 1.6 build 89555) on macOS 15.8, selecting each source in Audio MIDI Setup wrote `0x00`, `0x10` and `0x02` into the low byte of `0x0b14` and kept the rate field and the fetch bit:

| Audio MIDI Setup | `0x0b14` after |
|---|---|
| Internal | `0x02000000` |
| S/PDIF | `0x02000010` |
| SMPTE | `0x02000002` |

`EncodeClockSourceV3` does the same, and the protocol re-reads the register after writing, so the reported source is the device's, not the request's.

## Prior discussion

Follows #192 (UltraLite mk3 hardware reports).

## Architecture and scope

- `MotuRegisters.hpp`: pure `EncodeClockSourceV3`, and a per-model source list. Only the UltraLite mk3 has one, because only its list was read from MOTU's driver; every other MOTU model publishes no control, as before.
- `MotuProtocol` implements `IControlProvider` with one parameter (`ParameterSemantic::ClockSource`).
- `CoreAudioProjection`: a `ClockSource` parameter on a node with no ports is placed on the global scope as a `'clck'` selector, which is where the HAL takes a device's clock source from. Until now `Place()` only produced input/output scopes, so no clock source reached Core Audio.

## Testing and evidence

- Host tests: 8 new MOTU tests that pin the vendor driver's register values, the RMW, the read-back, refusing unlisted sources without bus traffic, and no control when the read fails; 2 projection tests (global placement, and a ported node staying off the global scope). Full suite passes.
- Hardware reference: MOTU's driver on macOS 15.8, Apple Silicon, UltraLite mk3, GUID `0001f2000008be94`, via IOFireWireLib register reads.
- **Not tested under ASFW on hardware.** The DriverKit build itself could not be compiled on the development Mac (Xcode 26.3 / DriverKit 25.2; unmodified `main` fails the same way there).

## Regression risks

- The projection change is generic: the Prism Orpheus "Sync Source" (a `ClockSource` parameter on its port-less settings node) now also appears in Audio MIDI Setup. It was app-only before. Untested on that device.
- Other MOTU models: unchanged (no control).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
