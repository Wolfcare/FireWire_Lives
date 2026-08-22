# Prior art

## mrmid1's Tahoe FireWire stack

A developer posting as **mrmid1** independently rebuilt the FireWire stack on
macOS Tahoe (26) and reported it in r/mac in early 2026. Their description of
the approach:

> No tricks: just restoring every subsystem one by one: OHCI layer (lowest,
> controller), Asynchronous and later Isochronous transmissions stuff. All with
> DriverKit, PCIDriverKit and AudioDriverKit — modern user space approach.

That is the same architecture this project arrived at independently: a
PCIDriverKit driver for the OHCI controller, an async transaction layer, then
isochronous, then protocol layers on top. They also confirm the transport
detail we measured — the Thunderbolt-to-FireWire adapter "is basically a PCIe
device", which is exactly what `fwprobe` sees.

**This is validation, not a shortcut.** Their code is not released; the post
says only "someday". Nothing here changes what we have to write. What it does
change is confidence: the approach is known to work end to end, on this OS, on
Apple Silicon, because someone has already driven audio through it.

### Calibration

Seven months of work, and as of five months after the original post it worked
with **one** device — an Apogee Duet FireWire. Worth holding onto when
estimating M7. The hard part is not the idea, it is the long tail of
per-device behaviour.

A second developer in the same thread, working on a Digidesign Digi 003,
described the isochronous stream as "a hard nut" — matching our own assessment
that M5 is where the real difficulty lives.

### The protocol insight worth stealing

> all audio interfaces are sharing IEC 61883-6 protocol for isochronous
> audio/midi streaming, but differs in control/init etc asynchronous stuff

This is architecturally useful. It means the isochronous streaming layer is
**generic and written once**, while the per-vendor work lives in the
asynchronous control and initialisation path. Our roadmap should reflect that
split: build IEC 61883-6 streaming as shared infrastructure, and treat
DICE/TCAT, BeBoB and the rest as thin control-layer plugins above it.

Two concrete pointers from the thread:

- Focusrite Saffire Pro 24 DSP and Pro 40 share the **TCAT** (TC Applied
  Technologies, i.e. DICE) control protocol.
- Digidesign Digi 002/003 deliberately obfuscate their IEC 61883-6 streams.
  Documented at <https://gareus.org/wiki/digi003>, and already implemented in
  the Linux ALSA firewire stack — which is the reference to read rather than
  reverse-engineering it again.

## Reference implementations worth reading

- **Linux `drivers/firewire`** — the OHCI driver, transaction layer and
  topology code. The closest thing to a maintained, correct 1394 stack.
- **Linux ALSA `sound/firewire`** — IEC 61883-6 streaming plus per-vendor
  control for DICE, BeBoB, Fireface, Tascam and others.
- **Apple's own headers**, still shipping in the macOS 26 SDK at
  `Kernel.framework/Headers/IOKit/firewire/`. Apple deleted the code, not the
  interfaces, so their class declarations remain the authoritative description
  of the stack that was removed.

## DriverKit families available to us

Checking the DriverKit 25.5 SDK, there is a purpose-built family for every
device class in scope. This matters: it means the upper layers plug into
CoreAudio, IOStorage and friends through supported API rather than needing a
bridge of our own.

| Framework | Relevance |
|---|---|
| `PCIDriverKit` | The OHCI controller. **In use now.** |
| `AudioDriverKit` | `IOUserAudioDevice` / `IOUserAudioDriver` — the M7 path into CoreAudio. |
| `SCSIControllerDriverKit` | SBP-2 tunnels SCSI command blocks, so this is the natural M4 target. |
| `BlockStorageDeviceDriverKit` | Alternative storage path if the SCSI family is a poor fit. |
| `MIDIDriverKit` | FireWire MIDI, which rides the same IEC 61883-6 streams as audio. |
| `VideoDriverKit` | Possible route for DV capture in M6. |

The roadmap previously said M4 would "bridge to IOStorage" by hand. Using
`SCSIControllerDriverKit` instead is both less work and better supported,
since SBP-2 is already a SCSI transport.
