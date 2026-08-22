# FireWire_Lives

Bringing IEEE 1394 (FireWire) back to macOS 26 and later, where Apple removed
the driver stack entirely.

Target platform is **Apple Silicon**. FireWire hardware reaches the machine
through a Thunderbolt-to-FireWire adapter, which presents a standard 1394 OHCI
controller over PCIe.

## What Apple removed

Verified on macOS 26.6.2 (build 25G83), arm64, in August 2026:

| Component | Status |
|---|---|
| `IOFireWireFamily`, `AppleFWOHCI`, `IOFireWireSBP2`, `IOFireWireAVC`, `IOFireWireSerialBusProtocolTransport` | Absent from every kernel collection |
| `/System/Library/Extensions/*FireWire*.kext` | Bundle husks only: `Info.plist` and code signature, **no binaries** |
| IOKit FireWire headers in the SDK | **Still present** — 57 headers in the macOS 26.5 SDK, byte-identical in count to the 15.4 SDK, including the kext KPI headers. Nothing implements them. |
| `IOFireWireLib` / `IOFireWireAVCLib` / `IOFireWireSBP2Lib` userspace plugins | Still shipping as arm64e, but they are user-client shims for kernel drivers that no longer exist |

The boot kernel collection contains 374 kexts. `IOPCIFamily` and
`IOStorageFamily` are present; nothing FireWire-related is.

What Apple deleted was the **code**, not the interfaces. That distinction is
useful rather than academic:

- `Kernel.framework/Headers/IOKit/firewire/` still contains Apple's own class
  declarations (`IOFireWireController`, `IOFireWireDevice`, the SBP-2 ORB and
  login structures). They are the most authoritative description of the stack
  that was removed, and a direct reference for reimplementing it.
- `IOKit.framework/Headers/firewire/IOFireWireLib.h` still defines the
  userspace CFPlugin ABI that existing FireWire applications are written
  against. A user client that matches it would let unmodified software work
  again, which is a stretch goal worth keeping the door open for.

One detail worth noting: `AppleFWOHCI.kext`'s `Info.plist` reports version
5.7.6, built against the **macOS 26.6.1 internal SDK with Xcode 17**, with
`LSMinimumSystemVersion 26.6`. Apple still builds this driver internally. They
simply ship it without a binary. The bundle is a tombstone.

Reproduce all of the above with:

```
./scripts/survey-macos-firewire.sh
```

## Approach

Kexts are a dead end: the FireWire KPIs are gone, and Apple Silicon requires
reduced security to load third-party kernel code anyway. So the stack is
rebuilt in userspace on **PCIDriverKit**.

That this works on macOS 26 / arm64e is not an assumption. Apple ships
`com.apple.DriverKit-Apple16X50PCI.dext`, which links `PCIDriverKit.framework`
and carries exactly the entitlements this project needs
(`com.apple.developer.driverkit`, `...driverkit.transport.pci`). It is the
working reference for the design.

```
        FireWire device (disk / camcorder / audio interface)
                              |
                    1394 cable, S400 or S800
                              |
        OHCI 1394 controller inside the Thunderbolt adapter
                              |
                       Thunderbolt / PCIe
                              |
   +--------------------------------------------------------+
   |  FireWireLives.dext        (PCIDriverKit, userspace)    |
   |    OHCI register programming, DMA contexts, bus reset   |
   +--------------------------------------------------------+
                              |  IOUserClient
   +--------------------------------------------------------+
   |  libfirewire              (portable 1394 protocol core) |
   |    self-ID / topology, config ROM, async transactions   |
   +--------------------------------------------------------+
        |                     |                      |
     SBP-2                  AV/C                   audio
    storage             DV / HDV                DICE, BeBoB
```

The protocol core is deliberately free of any DriverKit dependency, so it
compiles into the dext and into host-side unit tests alike.

## Status

Working and tested:

- **CRC-16** — validated against an independent bitwise reference over 2000
  random vectors, plus exhaustive single-bit-flip detection
- **Self-ID and topology** — parses OHCI's quadlet/inverse buffer format,
  reconstructs the bus tree (parent/child/hop counts), handles extended
  packets for PHYs with more than three ports, detects gap-count mismatch
- **Config ROM** — bus info block, root and unit directories, textual
  descriptors, SBP-2 management agent and LUN discovery, protocol
  classification for SBP-2 / AV/C / IIDC
- **Async packet codec** — encode and decode for every transaction code
- **fwprobe** — surveys the machine for an OHCI controller

119 checks pass, clean under `-Wall -Wextra -Wconversion` and under
AddressSanitizer + UBSan.

Not yet built: the dext itself. It needs Xcode's DriverKit SDK, which is not
installed on the development machine yet. `src/dext/` contains the bring-up
sequence written against the OHCI 1.1 spec, but it has never been through a
compiler — see `docs/building.md`.

## Building

Everything except the dext builds with the Command Line Tools alone:

```
make test     # run the core test suite
make asan     # same, under AddressSanitizer + UBSan
make probe    # survey the machine for a 1394 OHCI controller
```

## Documentation

- `docs/architecture.md` — layer-by-layer design and the reasoning behind it
- `docs/building.md` — toolchain prerequisites and the dev-signing workflow
- `docs/roadmap.md` — what ships in which milestone

## Acknowledgements

**[Aleksandr Shabelnikov (mrmidi)](https://github.com/mrmidi)** — for
[ASFireWire](https://github.com/mrmidi/ASFireWire), and for releasing it under
Apache 2.0 rather than keeping it to himself.

He rebuilt the FireWire stack on macOS 26 from nothing over seven months:
OHCI, all four asynchronous contexts, isochronous streaming, AV/C, the
isochronous resource manager, AudioDriverKit publication, DICE and Apogee audio
protocols, and DV capture. He did it alone, on the hardware he happened to own,
and then gave it away.

His code is also what pointed us at the descriptor bug that had our async
transmit context dying with `evt_unknown` — a rule we had got wrong and had
spent hours failing to find. We verified it against the OHCI specification and
cite the specification in the source, and no code of his was copied, but being
pointed at the answer is worth saying out loud.

If FireWire still works on your Mac in a few years, it is substantially because
one person decided that seven months of unpaid work should belong to everybody.
Go and look at [ASFireWire](https://github.com/mrmidi/ASFireWire). If your
device needs a protocol it does not yet speak, contribute it — that is what he
asked for, and it is a better use of your time than starting over.

Thanks also to the **Linux `firewire` and ALSA `sound/firewire` maintainers**
and the **FFADO project**, whose implementations remain the clearest surviving
description of how these devices actually behave on the wire.
