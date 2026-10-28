Title: app: read driver notifications in IOKit's async completion layout
Branch: Wolfcare:fix/app-async-notification-layout -> mrmidi:main

## Problem and change

The app never processes the driver's async notifications. `handleAsyncMessages`
looks for a scalar count at byte 40, assuming a `mach_msg_body_t` and a port
descriptor after the Mach header. The message `AsyncCompletion` actually delivers
uses IOKit's layout from `OSMessageNotification.h`:

| Offset | Content |
|---|---|
| 0 | `mach_msg_header_t` (24 bytes) |
| 24 | `OSNotificationHeader64`: content size, type (150, `kIOAsyncCompletionNotificationType`), 64-byte `OSAsyncReference64` |
| 96 | `IOAsyncCompletionContent`: `IOReturn` result |
| 100 | the scalars |

Byte 40 lies inside the reference block and reads 0, so the `count >= 2` guard
drops every message. Status updates only arrive through polling, and control
changes (`'CTRL'` notifications) never arrive. In Device Controls, front-panel
and clock changes stay invisible until you click refresh.

The parser now reads that layout: it checks the type, and derives the count
from the content size.

## Prior discussion

None. It's a narrow parsing fix in one function. Happy to open an issue if you prefer.

## Architecture and scope

App only (`DriverConnector+Lifecycle.swift`). It affects every device family,
because every notification uses this path. The driver is unchanged.

## Testing and evidence

- Raw dump of received messages on hardware (temporary logging, since removed): size 120, `size=0x14`, `type=0x96`, the sequence at byte 100 and the reason at byte 108, matching the SDK header.
- MacBook Air M2 (Mac14,2), macOS 26.7.1 (25G241), ASFW 0.6.0 local builds 43/44, Apple Thunderbolt to FireWire Adapter, MOTU UltraLite mk3:
  - Before: no `receive()` call at all, even for clock source changes.
  - After: the front-panel volume knob and clock source changes show live in Device Controls.
- Not tested on other device families. Since the old code dropped everything, they get notifications for the first time.

## Regression risks

Code that relied on polling now also gets push updates. `DeviceControlsViewModel.receive`
already filters by GUID, revision and sequence, but other families' control
views receive pushes they've never had.
