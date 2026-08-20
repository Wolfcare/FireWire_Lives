# Test hardware

The physical setup this project is developed and validated against.

## Host

| | |
|---|---|
| Machine | MacBook Air, M1 (T8103) |
| OS | macOS 26.6.2, build 25G83, arm64 |

An Apple Silicon Mac has no FireWire port, so all 1394 hardware arrives over
Thunderbolt.

## FireWire host controller ✅ verified

Apple Thunderbolt-to-FireWire adapter. Confirmed enumerating 2026-08-20 via
`make probe`:

| | |
|---|---|
| Controller | LSI / Agere `11c1:5901` rev 08 (FW643 family) |
| Location | PCI 3:0:0, Thunderbolt-tunnelled |
| Class code | `0x0c0010` |
| BAR0 | 4096 bytes, 32-bit memory space |
| IOMMU | DART, 16 KB mapper pages |
| Driver attached | none — unclaimed |

## Target device: Samsung 860 EVO ❌ not yet visible

A 500 GB Samsung 860 EVO SATA SSD in a FireWire enclosure. Transcribed from the
drive label, since the OS cannot read it yet:

| Field | Value |
|---|---|
| Product | Samsung V-NAND SSD 860 EVO |
| Capacity | 500 GB |
| Model | `MZ-76E500` |
| Part number | `MZ7LH500HALU` |
| Serial number | `S4XBNF0N802493F` |
| WWN | `5002538E9080F0EF` |
| Manufactured | 2020.08 |
| Rated | DC +5V, 1.2 A |

Two notes on those values:

- The **WWN is a SATA-side identifier** and will never appear on the 1394 bus.
  What appears there is the *enclosure bridge chip's* EUI-64 GUID, read from
  its config ROM. The drive's own model and serial arrive separately, via a
  SCSI INQUIRY tunnelled through SBP-2.
- The label also carries a **PSID**, which is deliberately not recorded here.
  A PSID authorises a cryptographic-erase revert that destroys every byte on
  the drive, and it has no diagnostic use. It does not belong in a git repo.

Transcribed from a photograph taken at an angle; verify the serial against the
physical label before relying on it for anything that matters.

## Acceptance criteria

These are the checkpoints that prove each milestone against this hardware:

- **M2 (controller bring-up)** — the dext claims the OHCI controller, `Version`
  reads back plausibly, and a bus reset yields self-ID packets that
  `fw_selfid_parse()` decodes into a topology containing the enclosure.
- **M3 (transaction layer)** — the enclosure bridge's config ROM is read, its
  GUID reported, and a unit directory classified as `FW_UNIT_SBP2`.
- **M4 (storage)** — SBP-2 login succeeds and a SCSI INQUIRY returns model
  `MZ-76E500` and serial `S4XBNF0N802493F`, matching the table above. The
  volume then mounts in Finder.

M4 is the point at which the question "do the model and serial match the label"
becomes answerable from the machine rather than from a photograph.
