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

## M2 — controller bring-up ✅ done

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

- [x] Install Xcode, get the DriverKit SDK (Xcode 26.6, DriverKit 25.5)
- [x] Compile and link the dext; ad-hoc signed with the PCI transport
      entitlement, links the same three libraries as Apple's own PCI dext
- [x] Bring-up sequence written against the real PCIDriverKit API
- [x] Disable SIP and enable dext developer mode
- [x] **Driver loads and claims the controller** — `FireWireLivesOHCI` appears in
      the IORegistry as a registered, matched, active child of the PCI device
- [x] Match the OHCI controller and open the PCI device
- [x] Map BAR0, read `Version` — OHCI 1.1 (`0x00010010`), GUID `0x000a270200754845` (Apple OUI)
- [x] Soft reset, LPS, link enable — `HCControl 0x008a0000`, `LinkControl 0x00000600`
- [x] **DMA under the DART IOMMU with 32-bit addressing — resolved.** The
      self-ID buffer maps to IOVA `0x80000000`, inside the 32-bit window
      OHCI's address registers require. `IODMACommandSpecification.maxAddressBits
      = 32` is honoured in practice, not just in the header.
- [x] Self-ID buffer DMA, and a first real bus enumeration — **2 nodes**,
      we are root at node 1, gap counts consistent, generation 7

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

## M4 — storage (SBP-2) 🚧 wire format done

Chosen first of the three protocols because it needs only async transactions —
no isochronous work — so it validates M3 end to end. The protocol encoding is
complete and tested; driving it needs the transport underneath.

- [x] Management ORB encoding (LOGIN / LOGOUT / RECONNECT and friends)
- [x] Login response parsing, including the command block agent address
- [x] Command ORB encoding with inline CDB, direction and page-table flags
- [x] Status block parsing with sense data, bounds-clamped
- [ ] Drive the login handshake over a real transaction layer
- [ ] Fetch agent: doorbell, ORB pointer, agent reset
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
| ~~DART IOMMU vs. OHCI's 32-bit DMA addressing~~ | **Resolved** | Self-ID buffer maps to IOVA `0x80000000`; `maxAddressBits = 32` honoured. |
| ~~PCIDriverKit may not expose enough DMA control~~ | **Resolved** | `IODMACommand` + `IODMACommandSpecification` are sufficient for OHCI. |
| Isochronous timing from userspace | Medium | Audio latency and DV frame integrity both depend on it. |
| `transport.pci` entitlement for distribution | Low | Only affects shipping to others, not local development. |
| ~~Thunderbolt adapter enumeration on Apple Silicon~~ | **Resolved** | Verified 2026-08-20: enumerates as an unclaimed `0x0c0010` PCI device. |
