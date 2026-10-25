MOTU: MIDI in and out

## Problem and change

MOTU units with MIDI ports get no MIDI under ASFW: the MIDI layer is built around AM824's MPX slot, and MOTU carries no MPX slot. A MOTU data block holds at most one MIDI byte in its message bytes, a flag (`0x01`) and the byte; which bytes depends on the model and direction (Linux `motu-stream.c:117-131`).

This adds MOTU MIDI end to end:

- `Audio/Wire/MOTU/MotuMidi.hpp`: per-model slot (2nd quadlet 4/6, 3rd quadlet 8/7; the Audio Express mixes them), a receive demux, and a transmit pacer that sends one byte per `rate/3093` blocks as Linux does (`amdtp-motu.c:211-254`).
- Receive: `RxMidiExtraction` and `MidiReceiveBinding` take an optional MOTU slot; `RxAudioPacketProcessor` reads it before the PCM binding check, so MIDI flows with no Core Audio client.
- Publication: `MidiEndpointPublication` now serves MOTU endpoints too. MOTU reports no MIDI in its registers, so `ProjectMotuMidiCapabilities` takes the model from the builder's profile (`AudioProfileRegistry::MotuUnitVersionForBuilderId`).
- Send: `MotuMidiTxComposer`, `MidiTxComposer`'s counterpart: same frontier, window and armed-end rules and the same reservation scope, writing into the message bytes, which the MOTU payload writer never touches. The producer records the unit version it armed, and `BindMidiTx` picks the MOTU composer for a MOTU stream. The nub RPC and `ASFWAudioDevice.cpp` are unchanged.

## Prior discussion

#192. Linux and libffado reference only; written fresh.

## Architecture and scope

Audio wire (MOTU), the receive processor and consumer, MIDI capabilities and publication, the endpoint runtime's binding, the audio driver's MIDI TX glue. No transport change.

## Testing and evidence

- 12 receive tests (codec, processor, consumer into the MIDI ring), 6 composer tests on packets armed by the production packetizer, publication tests for the UltraLite mk3 and for MOTU models without MIDI, and one end-to-end test through the real TX producer: an UltraLite mk3 run sends a Note On/Off in order, at least 16 blocks apart, with no byte outside the MIDI slot differing from the same run without MIDI. Full suite passes.
- MOTU's driver on macOS 15.8 publishes "UltraLite mk3 MIDI Port" in and out (and a sync source this PR does not add).
- **Not tested under ASFW on hardware.**

## Regression risks

- DICE MIDI: publication and the AM824 composer are untouched apart from a shared seam-mapping helper; the existing AM824 MIDI tests pass unchanged.
- MOTU models now get MIDI ports where Linux lists a slot (828mk2, Traveler, UltraLite, 8pre, 828mk3, Traveler mk3, UltraLite mk3, Track16; Audio Express mixed); none for the 896HD, 896mk3, 4pre and V1 models.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
