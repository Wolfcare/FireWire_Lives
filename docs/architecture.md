# Architecture

## Why not a kext

The obvious instinct is to rebuild `IOFireWireFamily` as a kernel extension.
That does not work on this target, for three independent reasons, any one of
which is fatal:

1. **The KPIs are gone.** `AppleFWOHCI` declared a dependency on
   `com.apple.iokit.IOFireWireFamily`. That family is no longer in any kernel
   collection, so there is nothing to link against at load time.
2. **Apple Silicon resists third-party kexts.** Loading one requires reduced
   security mode and a reboot into recoveryOS. That is acceptable for a
   developer, not for anyone we would want to give this to.
3. **It is the deprecated direction.** Apple has been migrating drivers out of
   the kernel since macOS 10.15. Writing new kernel code in 2026 is building
   on a foundation that is actively being removed.

## Why PCIDriverKit works

The FireWire controller is, from the host's point of view, an ordinary PCIe
device: class code `0x0c0010` (serial bus controller / FireWire / OHCI
programming interface). PCIDriverKit exists precisely to let userspace drive
PCIe devices.

This is not speculative. macOS 26 ships
`com.apple.DriverKit-Apple16X50PCI.dext`, an arm64e PCIDriverKit driver that
links `PCIDriverKit.framework` and carries:

```
com.apple.developer.driverkit                => true
com.apple.developer.driverkit.family.serial  => true
com.apple.developer.driverkit.transport.pci  => true
```

Those are the entitlements a PCI dext needs, demonstrated working on this exact
OS and architecture. Our driver needs the first and third, plus
`userclient-access` so our own userspace stack can talk to it.

## Layering

The stack is split so that the part which is hard to test (hardware) is as thin
as possible, and the part which carries the protocol complexity is completely
testable without any hardware at all.

```
  +---------------------------------------------------------------+
  |  Consumers                                                     |
  |    CoreAudio plugin  |  storage nub  |  DV capture             |
  +---------------------------------------------------------------+
  |  Protocol layers                                               |
  |    SBP-2   |   AV/C + CIP   |   audio (DICE / BeBoB)           |
  +---------------------------------------------------------------+
  |  libfirewire  (portable, no DriverKit dependency)              |
  |    transactions, config ROM, self-ID/topology, CRC, packets    |
  +---------------------------------------------------------------+
  |  FireWireLives.dext  (PCIDriverKit)                            |
  |    OHCI registers, DMA contexts, interrupts, bus reset         |
  +---------------------------------------------------------------+
  |  IOPCIDevice -> Thunderbolt -> OHCI controller -> 1394 bus     |
  +---------------------------------------------------------------+
```

`libfirewire` deliberately depends only on `<stdint.h>` and friends. That is
what allows the same source files to compile into the dext and into the host
test binary, and it is why 119 checks can run on a machine with no FireWire
hardware attached.

## Controller bring-up

Order is dictated by OHCI 1.1 and is not negotiable. Getting it wrong produces
symptoms that look like broken hardware:

1. **Enable PCI bus mastering.** Skipping this is the classic failure: every
   register reads back correctly, and no DMA ever occurs. Self-ID buffers stay
   empty and it looks like the PHY is dead.
2. **Soft reset**, then poll `HCControl.softReset` until it clears.
3. **Set LPS** (link power status) and wait ~50 ms. Reading PHY registers or
   `NodeID` before LPS settles returns garbage. This is the second classic
   bug.
4. **Program the self-ID buffer address** and enable self-ID reception.
5. **Set `linkEnable`**, unmask interrupts, arm `masterIntEnable`.
6. Issue a bus reset to force enumeration.

### DMA on Apple Silicon

The one genuine unknown, and the biggest risk in the project. On Intel, OHCI
DMA addresses are CPU physical addresses. On Apple Silicon, devices address
memory through the **DART** IOMMU, so the controller must be handed an IOVA
produced by `IODMACommand::PrepareForDMA()`, not a physical address.

Compounding this: `SelfIDBuffer`, `ConfigROMmap` and the descriptor
`branchAddress`/`dataAddress` fields are all **32 bits wide**. OHCI is a
32-bit-addressing device. Every DMA mapping must therefore be constrained to
the low 4 GB of the device's address space. In DriverKit that means an
`IODMACommandSpecification` with `maxAddressBits = 32`.

If the DART refuses to produce a mapping under that constraint, or if
PCIDriverKit does not expose enough control over the mapping, that is where
this project gets hard. It is the first thing to validate once hardware is
attached.

## Bus reset and generation counting

A bus reset renumbers every node. A request in flight against a stale node ID
could otherwise be delivered to a *different physical device* that has since
inherited that ID — a data-corruption bug, not merely a stale-data bug.

The stack therefore carries a generation counter, incremented on every reset.
Every transaction records the generation it was issued under, and any
transaction whose generation no longer matches is failed rather than retried.
This is the single most important invariant in a 1394 stack.

## Device classification

After a reset the driver reads each node's config ROM and routes it by the
`(specifier_id, version)` pair in its unit directories:

| specifier_id | version | Protocol |
|---|---|---|
| `0x00609e` | `0x010483` | SBP-2 — storage |
| `0x00a02d` | `0x010001` | AV/C — DV and HDV camcorders |
| `0x00a02d` | `0x000100/1/2` | IIDC — machine vision cameras |
| vendor-specific | — | audio interfaces (DICE, BeBoB, and friends) |

Audio is the awkward one: there is no single registered protocol, so
identification is a vendor/model table rather than a clean lookup. That work
belongs in the audio layer, not in the generic ROM parser.
