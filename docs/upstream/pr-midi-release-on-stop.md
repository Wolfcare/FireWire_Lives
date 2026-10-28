Title: MIDI: release the nubs on a full stop instead of terminating them
Branch: Wolfcare:fix/midi-release-nubs-on-stop -> mrmidi:main

## Problem and change

Unplugging the FireWire adapter while a MIDI endpoint is published aborts the dext.
`ServiceContext::Reset(Full)` calls `MidiNubPublisher::TerminateAll()`, which calls
`Terminate()` on nubs the kernel has already terminated with ASFWDriver's other
client services. DriverKit then asserts:

```
DriverKit  __assert_rtn
DriverKit  OSMetaClassBase::QueueForObject(unsigned long long)
DriverKit  IOService::Terminate(...)
net.asfw.driver  ASFW::Midi::MidiNubPublisher::TerminateNub(unsigned long long)
net.asfw.driver  ASFW::Midi::MidiNubPublisher::TerminateAll()
net.asfw.driver  ServiceContext::Reset(ServiceContext::ResetMode)
net.asfw.driver  ReleaseQuiescedRuntime(...)
net.asfw.driver  ASFWDriver::RequestRuntimeQuiesce(unsigned int)
```

`SBP2NubPublisher::Shutdown` already documents the same abort for SBP-2 nubs, and
handles it by dropping its bookkeeping. This change does the same for MIDI on a
full reset (`MidiNubPublisher::ReleaseAll`). A suspend reset still calls
`TerminateAll`, because the nubs are live then and wake publishes new ones.

## Prior discussion

None. It's a crash fix that follows the existing SBP-2 pattern.

## Architecture and scope

`Midi/Core/MidiNubPublisher` and `Service/DriverContext.cpp`. Applies to any family
that publishes MIDI nubs (DICE today; MOTU with the MOTU MIDI work).

## Testing and evidence

- New unit test `MidiNubPublisherTests.ReleaseAllForgetsWithoutTerminating`; full C++ suite passes (3,917 tests on this branch).
- Hardware: MacBook Air M2, macOS 26.7.1, Apple Thunderbolt to FireWire Adapter, MOTU UltraLite mk3 with MIDI ports published.
  - Before: adapter unplug crashed the dext twice in a row (17:42 and 18:58).
  - After: `releasing endpoint=... during shutdown` is logged, no crash report, and MIDI is republished after replug.
- Not tested: sleep/wake. The suspend path is unchanged.
- Not tested: a DICE device with MIDI.

## Regression risks

On a full stop the nubs are no longer explicitly terminated. That relies on the
kernel terminating them with the driver, the same assumption the SBP-2 publisher
already makes.
