# MOTU UltraLite mk3 on macOS 26+: what a port needs

The goal: a FireWire-only **MOTU UltraLite mk3** working as a Core Audio
device on an Apple Silicon Mac running macOS 26 or later, through Apple's
Thunderbolt-to-FireWire adapter.

This document maps every piece involved, what already exists, what is missing,
and the reference data captured from MOTU's own driver for checking a port
against. Status as of 2026-10-25.

## Where things stand

**On macOS 15.8 it works today**, with MOTU's own kext. Only one MOTU build
qualifies. MOTU 1.6 build **89555** (September 2021) is the last release that
has both Apple Silicon code and the FireWire transport. Builds 1.6 b5003c51d
and 1.7 11ea3df24 dropped the transport; they compile in only the USB and
Thunderbolt-PCI providers. The 2019 build 83634 is Intel-only.

**On macOS 26 it cannot work that way.** Apple removed `IOFireWireFamily` and
`AppleFWOHCI` (see the README), which MOTU's kext links against. Every layer
below the audio device has to be replaced, and on Apple Silicon that means
userspace DriverKit.

**ASFireWire already does most of this.** Its 0.5.0 MOTU stack includes the
UltraLite mk3 (unit version `0x19`, protocol V3). ASFireWire issue
[#192](https://github.com/mrmidi/ASFireWire/issues/192) reports audio
streaming on real hardware at 44.1 and 48 kHz with test build 22. So this is
not a from-scratch port: the work is validation and the remaining gaps.

## The stack, layer by layer

| Layer | macOS 15.8 (working) | macOS 26+ replacement | State for UltraLite mk3 |
|---|---|---|---|
| Physical | Apple Thunderbolt-to-FireWire adapter | Same | ✅ unchanged; it is a PCIe 1394 OHCI controller |
| OHCI controller | `AppleFWOHCI` kext | ASFireWire dext (PCIDriverKit) | ✅ shared by every device ASFireWire supports |
| Bus, async, isoch, IRM | `IOFireWireFamily` | ASFireWire | ✅ shared |
| MOTU control protocol (rate, streams, notifications) | MOTU kext `FireWireProvider` + `BoxUltraLitemk3FW` | `ASFWDriver/Audio/Protocols/MOTU/MotuProtocol.cpp` | ✅ implemented; 44.1/48 kHz confirmed in #192 |
| MOTU wire format (data blocks, SPH timing) | MOTU kext | `ASFWDriver/Audio/Wire/MOTU/` | ✅ implemented; timing tuned on an 828mk3 capture, not an UltraLite one |
| Channel layout and names | MOTU kext | `MotuPortLayout.hpp`, `version == 0x19` | ✅ same channels and order as MOTU; names differ (below) |
| Core Audio device | `IOAudioFamily` engine | AudioDriverKit device in the dext | ✅ shared |
| Clock source selection | Internal / S/PDIF / SMPTE in Audio MIDI Setup | — | ❌ ASFireWire reads the source but cannot set it |
| MIDI in/out (the UltraLite's DIN ports) | `Motu MIDI Driver.plugin` | ASFireWire MIDI layer exists | ❌ not wired for MOTU: the payload codec carries no MIDI |
| Mixer (CueMix FX) | CueMix FX app → kext user client | — | ❌ nothing; the front panel still works standalone |
| Latency reporting | Per-rate values (below) | Fixed 64 / 128 frames | ⚠️ placeholder values |
| 88.2 kHz to 192 kHz | Supported | Layout coded | ⚠️ untested on hardware |

## Reference data from MOTU's driver

All captured on this repository's test machine: MacBook Air, Apple Silicon,
macOS 15.8, MOTU 1.6 build 89555, UltraLite mk3 GUID `0001f2000008be94`.
Raw output is in [`reference/ultralite-mk3/`](reference/ultralite-mk3/).
Reproduce with `make cadump` and `make motu`.

### Channels per rate

MOTU's driver publishes these, in this order:

| Rate | Inputs | Outputs |
|---|---|---|
| 44.1 / 48 kHz | 14: Analog 1–8, S/PDIF 1–2, Return 1–2, Reverb 1–2 | 14: Main Out 1–2, Analog 1–8, S/PDIF 1–2, Phones 1–2 |
| 88.2 / 96 kHz | 12: Analog 1–8, S/PDIF 1–2, Return 1–2 | 14: same as 1x |
| 176.4 / 192 kHz | 10: Analog 1–8, Return 1–2 | 12: Main Out 1–2, Analog 1–8, Phones 1–2 |

Input streams are grouped 8+2+2+2 at 1x, 8+2+2 at 2x and 8+2 at 4x; outputs
2+8+2+2, then 2+8+2 at 4x.

ASFireWire's `version == 0x19` layout produces the same channel set in the
same order at every rate (Reverb only at 1x, S/PDIF dropped at 4x). Only the
names differ: ASFireWire says `Main L/R`, `Return L/R`, `Reverb L/R` and
`Phones L/R`, where MOTU says `Main Out 1/2`, `Return 1/2`, `Reverb 1/2` and
`Phones 1/2`. That is cosmetic. What Core Audio cannot show is which wire
chunk carries each channel, so the mapping still needs a signal test on
every port (see the test plan).

### Clock sources

What MOTU's driver publishes in Core Audio, and what it writes to the unit
when each is selected (`cadump --clock ID`, then `motuprobe`, at 44.1 kHz):

| Core Audio ID | Name | `0x0b14` written | Source field (low byte) |
|---|---|---|---|
| `0x0` | Internal | `0x02000000` | `0x00` |
| `0x2` | S/PDIF | `0x02000010` | `0x10` |
| `0x6` | SMPTE | `0x02000002` | `0x02` |

Only the low byte changes; the rate field and the fetch bit are kept. The
source values are the ones Linux `motu-protocol-v3.c` decodes, so a write
path can reuse its constants. Nothing was connected to the S/PDIF or SMPTE
inputs, and the driver accepted the selection anyway.

### Latency and safety offset (frames)

| Rate | Input latency | Output latency | Safety offset |
|---|---|---|---|
| 44.1 / 48 kHz | 19 | 20 | 24 |
| 88.2 / 96 kHz | 22 | 24 | 48 |
| 176.4 / 192 kHz | 25 | 28 | 96 |

ASFireWire currently reports 128 latency and 64 safety offset at every rate,
on purpose, "until hardware says otherwise". MOTU's numbers describe MOTU's
own buffering, so they are a target to measure against, not values to copy.

### Device registers while MOTU's driver streams (44.1 kHz)

Read with `motuprobe`; MOTU registers live at `0xffff'f000'0000 + offset`.

| Register | Value | Meaning |
|---|---|---|
| `0x0b00` isoc comm control | `0x61620000` | Device receives on channel 33 and transmits on channel 34, both active |
| `0x0b04` / `0x0b08` async address | `0xffc10001` / `0x00000000` | Notifications go to host node `0xffc1`, address `0x0001'0000'0000` |
| `0x0b10` packet format | `0x00000002` | Transmit speed field 2 (S400), no "exclude differed chunks" bits |
| `0x0b14` clock status | `0x02000000` | 44.1 kHz, internal clock, fetch-PCM bit set |
| `0x0c94` optical mode | `0x00000000` | No optical (the UltraLite mk3 has none) |

Channels 33 and 34 are the same ones in ASFireWire's 828mk3 capture from
MOTU's driver (`documentation/fixtures/MOTU/828mk3-48k/`), which suggests
MOTU's driver allocates them the same way across models.

Idle, before any driver had touched the unit, `0x0b04` and `0x0b08` read
`0xffffffff` and the fetch bit was clear.

At the other rates, only the rate field of `0x0b14` changes. Channels 33/34,
the packet format and the fetch bit stay the same:

| Rate | `0x0b00` | `0x0b10` | `0x0b14` |
|---|---|---|---|
| 44.1 kHz | `0x61620000` | `0x00000002` | `0x02000000` |
| 48 kHz | `0x61620000` | `0x00000002` | `0x02000100` |
| 88.2 kHz | `0x61620000` | `0x00000002` | `0x02000200` |
| 96 kHz | `0x61620000` | `0x00000002` | `0x02000300` |
| 176.4 kHz | `0x61620000` | `0x00000002` | `0x02000400` |
| 192 kHz | `0x61620000` | `0x00000002` | `0x02000500` |

One oddity: once, after a run of fast switches, the register still read
192 kHz while Core Audio already reported 44.1 kHz. A re-read some seconds
later showed the right value. MOTU's driver may apply the rate to the unit
lazily, so read the register only after it settles.

## Test plan on macOS 26

The test machine dual-boots: macOS 15.8 stays as the reference with MOTU's
driver, macOS 26 runs ASFireWire. Same Mac, same adapter, same cable, so any
difference is the driver.

1. **Install.** macOS Tahoe on a separate APFS volume, then ASFireWire's
   signed build from <https://www.asfw.net/install/>, at least the build #192
   used. Confirm with `systemextensionsctl list` that `net.asfw.driver` is
   `[activated enabled]` at the new version; reboot if an older one lingers.
2. **Baseline (repeats #192).** 44.1 and 48 kHz playback and capture through
   Main Out and Analog 1–2.
3. **Every port, every rate.** A test tone into each input and out of each
   output, at 44.1, 48, 88.2, 96, 176.4 and 192 kHz. Compare against the
   channel tables above. #192 could not test the 1/4" inputs or S/PDIF.
4. **Rate switching** while Core Audio is idle and while playing.
5. **Recovery:** unplug and replug, power-cycle the UltraLite, sleep and wake.
6. **Long run:** an hour of playback and record; listen for clicks, check
   ASFireWire's transmit heartbeat logs for dropped frames.
7. **Report** each result with ASFireWire's exported driver logs on #192.

## Work items and status

The code lives on branch `motu-ultralite-mk3-fixes` of a local ASFireWire
clone (`../ASFireWire`), based on `main` at `0a3879e3` (2026-10-23). Nothing is
pushed. Every change has host tests; ASFireWire's full C++ suite passes (3,977
tests). None of it has run on hardware under ASFireWire yet.

This Mac cannot build the dext itself: Xcode 26.3 ships DriverKit 25.2, and
ASFireWire's unmodified `main` fails to compile against it with the same libc++
errors. ASFireWire's `build.sh` "Bump Version" script also breaks on a path
with a space in it ("Firewire lives"). Both are outside this work.

| Item | Status | Commits |
|---|---|---|
| Clock source (Internal / S/PDIF / SMPTE in Audio MIDI Setup) | Done | `029a51f3` |
| Latency: MOTU's per-rate values; safety offset doubles per rate mode | Done | `f9e4015d` |
| 88.2–192 kHz pinned against MOTU's driver | Done (audio needs hardware) | `d81c6aa5` |
| MIDI in and out, one port each way | Done | `9259a71d`, `7f0c26af` |
| CueMix mixer: 8 mixes × 10 sources, gain/pan/mute/solo, mix volume/mute/destination, main volume | Done | `94e0f4c3`, `abeaf649`, `6fd1d4f9` |
| CueMix reverb, input/output EQ and dynamics, meters, talkback | Not started | |
| Channel names matching MOTU's | Not done: better proposed on #192 | |

### MIDI

`MotuMidi.hpp` holds each model's slot (UltraLite mk3: third quadlet, flag
byte 8, data byte 7, both ways). MIDI ports are published for MOTU endpoints
from the model (`ProjectMotuMidiCapabilities`), the receive path lifts bytes
into the MIDI ring, and `MotuMidiTxComposer` writes outgoing bytes into armed
packets at the unit's cable speed. An end-to-end test through the real TX
producer sends a Note On/Off and checks that nothing outside the MIDI slot
changes.

### CueMix

Protocol from `snd-firewire-ctl-services` (`protocols/motu/src/command_dsp.rs`),
then checked against MOTU's own kext by decompiling its shipped symbols
(`BoxWithFxDSP`):

- The host registers a message destination with **one 8-byte block write to
  `0x0b38`** (host node ID, then the 48-bit offset), and writes **all ones** to
  stop. The registers are write-only: reading them gives an address error.
- Commands are block writes of at most 248 bytes to **`0xffff'0001'0000`**
  (MOTU's `SetCueMixPacket` sends 1–255 bytes there): tag, sequence number,
  commands, zero padding to a quadlet.
- The unit sends its state, then every front-panel change, to the destination.
  MOTU's kext passes these frames to CueMix FX unparsed, so their content is
  the reference's description, not vendor-confirmed. The float values are
  little-endian per the reference's code (its comments say big-endian).

In ASFireWire the mixer is a control model for the ASFW app; it stays out of
Core Audio, as CueMix does.

## Hardware test checklist (macOS 26 + ASFireWire built from this branch)

Beyond the test plan above:

1. Clock source menu in Audio MIDI Setup shows Internal / S/PDIF / SMPTE and
   switching it changes the front panel.
2. A MIDI keyboard into MIDI IN appears in a DAW; a DAW sending to MIDI OUT
   plays a synth.
3. The ASFW app's Device Controls shows the mixer with the unit's current
   settings; moving a fader changes the sound; turning the front-panel knob
   updates the app.

## Tools in this repository

| Tool | Runs on | What it does |
|---|---|---|
| `make motu` (`tools/motuprobe`) | macOS 15 and earlier | Reads MOTU registers through Apple's IOFireWireLib; `--rate` sets the sample rate. Works alongside MOTU's driver. |
| `make cadump` (`tools/cadump`) | Any macOS | Dumps a device as Core Audio presents it: channels, names, rates, clock sources, latencies. `--rate` switches rate first. Use it on 15.8 against MOTU's driver and on 26 against ASFireWire, then diff. |

ASFireWire's own `tools/motu/motu_probe.py` does register snapshots on macOS
26 through its app's diagnostics endpoint; its output is the counterpart to
`motuprobe`.
