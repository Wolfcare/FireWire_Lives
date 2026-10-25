Proposal: CueMix (command DSP) control for the MOTU mk3 family

CONTRIBUTING asks for agreement before a large PR, so this proposes one first. The work is done and tested on the host (branch `pr/motu-cuemix`, ~1,700 lines on top of the clock source and latency PRs); I'd like to know whether you want it, and in what shape.

## What it does

The mk3 family (828mk3, 896mk3, UltraLite mk3, Traveler mk3, Track16) has a DSP mixer that MOTU's CueMix FX drives: 8 mixes, per-input EQ/dynamics/reverb, output EQ/dynamics, reverb, monitor. Under ASFW none of it is reachable. The branch adds it for the UltraLite mk3, the model whose ports are known:

- `MotuCommandDsp.hpp`: command encoding and decoding, 248-byte framing with tag and sequence number, reassembly of the unit's frames.
- `MotuDspParameters.hpp`: one table of every parameter (address, kind, range), ~1,500 on the UltraLite mk3. Decoder, state, control model and writes all read it.
- `MotuDspMessages.hpp`: a 256-byte slot per unit past the existing MOTU notification quadlets, reached through the existing MOTU local-request participant (no Service wiring change).
- `MotuProtocol`: registers the message destination, begins messaging, sends commands, records acknowledged writes. `MotuControlModel.hpp`: a `MixerNode` and port-less nodes, so none of it reaches Core Audio; it is for the app, as CueMix is.

## Evidence

- Protocol from snd-firewire-ctl-services (`protocols/motu/src/command_dsp.rs`, `version_3.rs`).
- Checked against MOTU's own kext (MOTUFireWireAudio 1.6 build 89555, arm64e, decompiled from its shipped symbols): `BoxWithFxDSP::SendCueMixPacketReadAddress` writes the destination as one 8-byte block to `0xffff'f000'0b38` and all ones to stop (the reference writes two quadlets and zeros; the branch follows the vendor); `SetCueMixPacket` sends 1–255-byte packets to `0xffff'0001'0000`. The kext passes the unit's frames to CueMix FX unparsed, so their content rests on the reference.
- 19 codec/table tests, 10 protocol tests (registration, frames on the wire, readback, refusal of out-of-range writes, shutdown). Full suite passes.
- **Not tested on hardware.** The float encoding (little-endian per the reference's code, though its comments say big-endian) is the first thing a hardware run would confirm.

## Open questions

1. Is a ~1,500-parameter control model what you want in the app, or should a first PR carry only the mixer (8 × 10 crosspoints + mix outputs)?
2. Meters arrive inside the isoch stream (Linux `motu-command-dsp-message-parser.c`) and would touch the RX hot path; the branch leaves them out.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
