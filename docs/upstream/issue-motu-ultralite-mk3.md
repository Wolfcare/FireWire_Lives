Title: MOTU UltraLite mk3: clock source, latency, MIDI and CueMix, tested on hardware. How would you like it split?

Hi! I own a FireWire UltraLite mk3, which I ran on macOS 15.8 with MOTU's own driver, and I've
been running ASFW on macOS 26.7.1 since the 0.6.0 release. Thanks for the work; audio
was solid from the start (as in #192).

I've worked on the gaps for this unit and tested everything on the hardware. Before
opening PRs, I'd like to ask how you want them split, per CONTRIBUTING. The code is
on my fork as `Wolfcare:motu/ultralite-mk3` (14 commits on top of `main` 0a3879e3).
Three generic fixes I found along the way are separate small PRs (#232, #233, #234).

### Results on hardware

MacBook Air M2 (Mac14,2), macOS 26.7.1 (25G241), Apple Thunderbolt to FireWire Adapter
(LSI FW643 rev 8), UltraLite mk3 (unit version 0x19, GUID 0001f2000008be94), local
ad-hoc builds of 0.6.0 with SIP off.

| Feature | Before (0.6.0 build 33) | After | Reference |
|---|---|---|---|
| Channels per rate | 176.4 kHz listed Return L/R twice (12 inputs) | 14/14, 12/14, 10/12 at 1x/2x/4x, as MOTU's driver | Core Audio dumps from MOTU 1.6 build 89555 on macOS 15.8 |
| 44.1–192 kHz | not tested | all six rates switch and stream | |
| Clock source | not offered | Internal / S/PDIF / SMPTE in Audio MIDI Setup; the unit confirms each switch | register writes captured from MOTU's driver |
| Latency | 128 frames fixed | MOTU's per-rate values (in 19/22/25, out 20/24/28); safety offset doubles per rate mode | MOTU's driver |
| MIDI | no ports | 1 in / 1 out; loopback test 66/68 passed (all channel messages on 16 channels, system common and real-time, SysEx up to 1,024 bytes, a 200-message burst) | `MotuMidi.hpp`; Linux slot layout |
| CueMix mixer | none | about 1,500 parameters (monitor, mixes, input/output EQ and dynamics, reverb) in Device Controls; the state dump reads back; writes work; front-panel changes show live | snd-firewire-ctl-services `command_dsp.rs`; MOTU's driver for the destination write |
| Volume keys | none | Main Volume is the device's Core Audio master volume, so the Mac's volume keys drive it | |

### Proposed split (your call)

1. Clock source selection (`8391d399`)
2. Per-rate latency and safety offset (`826be2e8`, `977f7f8a`)
3. MIDI in/out (`2499de7a`, `8b479089`)
4. CueMix: command DSP protocol and mixer model (`a259c69b`, `317d0f6b`, `fa072f2b`, `7a92ebdc`)
5. CueMix hardware fixes (`f33b8c54`) and Main Volume as the Core Audio master (`77fb4d2a`)

Item 4 is the largest (about 1,700 lines, mostly a parameter table and tests),
which is why I'm asking before opening it.

### Things I found that you may want to weigh in on

- **Stale DSP session.** A unit that stays powered ignores every command from a new driver instance until it's power-cycled. Cancelling messaging (tag 0) and releasing the destination before registering fixes it; `f33b8c54` does that.
- **`evt_missing_ack` on every write response** to the unit's DSP messages, about 30 a second. Write responses go out but are never acked, and the mixer works regardless. Speed and ack_pending handling as Linux does (I have it on a local branch) didn't change it, so I'm not proposing that change.
- **MIDI only moves while audio is streaming**, because it rides in the isoch stream. Should opening a MIDI port start the stream?
- **The first MIDI message after a port opens is lost**, reproducibly. Everything after it arrives byte-exact. I haven't found the cause yet.
- **Main Volume is a linear 0..1 gain**, so Core Audio reports its range as 0..1 dB. A dB mapping would be nicer; open to how you'd model it.
- **Active sensing (FE) didn't come back in the loopback.** It may be filtered by the unit.

### Not tested

- Other MOTU models. The model tables are gated to unit version 0x19 where they come from UltraLite measurements.
- Sleep/wake.
- Intel Macs.
- S/PDIF and SMPTE as actual clock sources: the switch is confirmed, but nothing was connected.

I can take review comments and follow up on regressions. If any of this doesn't fit
the direction you want, I'm happy to drop or rework it.
