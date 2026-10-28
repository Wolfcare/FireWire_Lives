# Apple DriverKit entitlement request (Wolfcare team)

Submit at: https://developer.apple.com/contact/request/system-extension/
(Apple Developer › Account › request DriverKit / System Extension entitlements.)
Sign in with the paid developer account. Apple answers by email; it can take a few weeks.

---

## Bundle IDs to register first (Certificates, Identifiers & Profiles)

- App: `com.wolfcare.asfw` (the app that installs the driver)
- Driver extension: `com.wolfcare.asfw.driver`

## Text to paste

**What the driver does**

ASFW is an open-source (Apache 2.0) DriverKit driver that restores FireWire (IEEE 1394)
support on macOS 26 and later, where Apple removed IOFireWireFamily and AppleFWOHCI.
It drives the IEEE 1394 OHCI controller inside the Apple Thunderbolt to FireWire Adapter
from user space. It publishes FireWire audio interfaces to Core Audio (AudioDriverKit)
with their MIDI ports (MIDIDriverKit), and FireWire storage devices (SBP-2) as SCSI
targets (SCSIControllerDriverKit). My build adds full support for the MOTU UltraLite mk3
audio interface: mixer, clock source and MIDI.

Source: https://github.com/Wolfcare/ASFireWire (fork of https://github.com/mrmidi/ASFireWire)

**Hardware**

IEEE 1394 OHCI controllers, PCI class code 0x0C0010. Primarily the Apple Thunderbolt to
FireWire Adapter (LSI/Agere FW643, vendor ID 0x11C1, device ID 0x5901), also reached
through Thunderbolt 3/4 to Thunderbolt 2 adapters on Apple Silicon Macs.

**Entitlements requested**

- `com.apple.developer.driverkit`
- `com.apple.developer.driverkit.transport.pci`, matching PCI class 0x0C0010 (IEEE 1394 OHCI); at minimum vendor 0x11C1, device 0x5901
- `com.apple.developer.driverkit.family.audio`: FireWire audio interfaces in Core Audio
- `com.apple.developer.driverkit.family.midi`: their MIDI ports
- `com.apple.developer.driverkit.family.scsicontroller`: FireWire (SBP-2) storage
- `com.apple.developer.driverkit.userclient-access` for the app `com.wolfcare.asfw`, which installs the driver and shows its device controls
- `com.apple.developer.system-extension.install` for the app `com.wolfcare.asfw`

**Distribution**

Free, outside the Mac App Store, signed with Developer ID and notarized, from GitHub releases.
