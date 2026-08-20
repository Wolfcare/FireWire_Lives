# Roadmap

Three device classes are in scope: **audio interfaces**, **storage**, and
**DV/HDV camcorders**. They share the whole lower stack, so the ordering below
front-loads everything common and defers the per-protocol work.

## M1 — portable protocol core ✅ done

No hardware and no DriverKit SDK required.

- [x] CRC-16, validated against an independent bitwise reference
- [x] Self-ID decoding, including OHCI's quadlet/inverse buffer format
- [x] Topology reconstruction: parent/child edges, hop counts, root election
- [x] Extended self-ID packets for PHYs with more than three ports
- [x] Gap-count consistency checking
- [x] Config ROM: bus info block, root and unit directories, textual
      descriptors
- [x] Protocol classification for SBP-2, AV/C and IIDC
- [x] SBP-2 management agent and LUN discovery
- [x] Async packet encode/decode for every transaction code
- [x] `fwprobe` hardware survey tool
- [x] Bounds-safety under ASan/UBSan against truncated and hostile ROMs

## M2 — controller bring-up 🚧 blocked on Xcode

Hardware validated 2026-08-20 with the Apple Thunderbolt-to-FireWire adapter
attached. The controller enumerates and is unclaimed:

| | |
|---|---|
| Controller | LSI / Agere `11c1:5901` rev 08 (FW643 family), at PCI 3:0:0 |
| Class code | `0x0c0010` — matches our `IOPCIClassMatch` |
| Driver attached | **none** — macOS 26 leaves it entirely unclaimed |
| BAR0 | 4096 bytes, 32-bit memory space (register file needs `0x800`) |
| Transport | Thunderbolt-tunnelled (`IOPCITunnelled = Yes`) |
| IOMMU | **DART present**, mapper page size 16384 |
| Required entitlement | macOS itself reports `com.apple.developer.driverkit.transport.pci` |

That last row is the important one: the OS states the entitlement a driver must
hold to claim this device, and it is exactly the one in our
`FireWireLives.entitlements`. The architecture is confirmed against real
hardware rather than inferred.

- [ ] Install Xcode, get the DriverKit SDK
- [ ] Compile the dext for the first time
- [ ] Match the OHCI controller and open the PCI device
- [ ] Map BAR0, read `Version` — the first proof of life
- [ ] Soft reset, LPS, link enable
- [ ] **Resolve DMA under the DART IOMMU with 32-bit addressing** — the
      biggest technical risk in the project, now confirmed present on the
      real adapter; see `docs/architecture.md`. Note the DART maps at 16 KB
      granularity while the self-ID buffer only needs 2 KB alignment, so a
      whole mapper page is the practical minimum allocation.
- [ ] Self-ID buffer DMA, and a first real bus enumeration

The milestone is done when `fwprobe` reports a real device's GUID and vendor
string, read off an actual FireWire bus. See `docs/test-hardware.md` for the
specific hardware and the per-milestone acceptance criteria.

## M3 — transaction layer 🚧 partly done

The hardware-independent half is complete and tested; the DMA half needs the
dext.

- [ ] AT/AR DMA contexts, descriptor rings
- [x] Async request tracking with round-robin tlabel allocation
- [x] Split-transaction timeout and expiry
- [x] Response matching, validated against responder node, target node and
      expected response tcode
- [x] Generation-guarded abort on bus reset (see the bus-reset invariant)
- [ ] Config ROM readout for every discovered node
- [ ] IOUserClient API, so userspace can issue transactions

## M4 — storage (SBP-2)

Chosen first of the three protocols because it needs only async transactions —
no isochronous work — so it validates M3 end to end.

- [ ] Login/reconnect/logout against the management agent
- [ ] ORB construction and the fetch agent
- [ ] SCSI command block encapsulation
- [ ] Bridge to IOStorage so a disk actually mounts in Finder

## M5 — isochronous

Everything below this line depends on isochronous DMA, which is where the real
timing difficulty lives.

- [ ] Isochronous receive and transmit contexts
- [ ] Cycle timer, cycle-match start
- [ ] Channel and bandwidth allocation via the IRM
- [ ] Buffer-fill and packet-per-buffer receive modes

## M6 — DV / HDV (AV/C)

- [ ] AV/C command/response framing (FCP)
- [ ] Descriptor and subunit enumeration
- [ ] CIP packet framing, DV frame reassembly
- [ ] Transport control: play, stop, record
- [ ] Deliver frames to a capture client

## M7 — audio interfaces

The hardest of the three, and the reason the isochronous layer has to be
genuinely good rather than merely functional.

- [ ] Vendor/model identification table (DICE, BeBoB, Fireface, …)
- [ ] AM824 / CIP audio packet framing
- [ ] Sample-clock recovery and drift handling
- [ ] Low-latency duplex streaming
- [ ] CoreAudio plugin so devices appear as normal audio interfaces

## Known risks

| Risk | Severity | Notes |
|---|---|---|
| DART IOMMU vs. OHCI's 32-bit DMA addressing | **High** | Confirmed present on the real adapter (16 KB mapper pages). Blocks everything from M2 onward. Validate first. |
| PCIDriverKit may not expose enough DMA control | **High** | If so, the project may need a different transport strategy entirely. |
| Isochronous timing from userspace | Medium | Audio latency and DV frame integrity both depend on it. |
| `transport.pci` entitlement for distribution | Low | Only affects shipping to others, not local development. |
| ~~Thunderbolt adapter enumeration on Apple Silicon~~ | **Resolved** | Verified 2026-08-20: enumerates as an unclaimed `0x0c0010` PCI device. |
