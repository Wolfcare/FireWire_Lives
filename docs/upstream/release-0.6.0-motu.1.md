Tag: v0.6.0-motu.1 (on Wolfcare:motu/ultralite-mk3, 77fb4d2a)
Title: ASFW 0.6.0-motu.1 (unofficial MOTU UltraLite mk3 test build)

**Unofficial test build.** This is a modified build of [ASFireWire](https://github.com/mrmidi/ASFireWire) (Apache 2.0) with MOTU UltraLite mk3 work that hasn't been merged upstream yet ([issue #235](https://github.com/mrmidi/ASFireWire/issues/235)). It isn't the official ASFW release and isn't supported by its author. For the official, signed build, use [asfw.net](https://www.asfw.net/install/).

### What's new for the UltraLite mk3

- Clock source selection: Internal, S/PDIF or SMPTE, from Audio MIDI Setup
- MOTU's own latency values at every sample rate, 44.1–192 kHz
- MIDI in and out on the unit's DIN ports
- The CueMix mixer in the ASFW app (Device Controls): monitor, mixes, EQ, dynamics and reverb, with front-panel changes shown live
- The Mac's volume keys control the UltraLite's main volume
- Fixes: no crash when the adapter is unplugged, and the app now receives live updates from the driver

### Requirements

- Apple Silicon Mac, macOS 26 or later, Apple Thunderbolt to FireWire Adapter
- **System Integrity Protection (SIP) must be off.** This build isn't signed by Apple, so macOS only loads its driver with SIP disabled. That lowers your Mac's security; turn SIP back on when you stop using it.

### Install

1. If the official ASFW is installed, remove its driver first: open ASFW and click Delete, or run `systemextensionsctl uninstall F6YA6B56LR net.asfw.driver`.
2. Turn SIP off. Shut down and hold the power button until "Loading startup options" appears. Choose Options, then Utilities › Terminal, run `csrutil disable`, and restart.
3. Unzip the download, open Terminal in the folder, and run `./install.sh`.
4. In the ASFW app, click **Install**. Approve the driver in System Settings › General › Login Items & Extensions, then restart.

### Known limits

- MIDI only works while audio is streaming. The first MIDI message after a port opens is lost.
- Tested on a single UltraLite mk3 (MacBook Air M2, macOS 26.7.1). Other MOTU models are untested.

SHA-256: `41717fe1f5ba005ce25d5c4904eea5a4e0ad220ffe3cebe91c74cfa8f3ef39ac`
