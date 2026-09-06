# MicroDrone Downlink

The existing Developer Tools action opens a real modeless serial-output window,
including while disconnected. It is an encoder and transmitter, not a decoder.
The fixed 24-entry Tools menu and SETUP section count do not change.

## Reference and protocol

Primary sources are MP10 `Services/MicrodroneDownlinkEncoder.cs`,
`ViewModels/MicrodroneDownlinkViewModel.cs`, `Views/MicrodroneDownlinkView.axaml`,
`ExtLibs/ArduPilot/CurrentState.cs` and the HOME_POSITION/receipt-time handlers in
`Mavlink/MAVLinkInterface.cs`. The active reference targets .NET 10. The older
WinForms float formatter is not the numeric reference for this port.

One frame contains #1, #4, #5, #6, #7, #8 and #9, each with decimal checksum and
CRLF. The checksum is the modulo-256 byte sum XOR 255, not NMEA's XOR checksum.
GPS week/seconds use UTC since 1980-01-06 without leap-second correction, and
the sample time uses integer counter/10. Invalid/pre-epoch clocks fail before
submitting a new frame. The reference's fixed magic fields, legacy ECEF formula
and altitude factor 0.0001 are retained, not silently replaced by standard ECEF.

Float telemetry arithmetic is preserved before widening into the encoder's
double fields. Raw pressure temperature and primary magnetometer values follow
CurrentState, including its inconsistent HIGHRES_IMU temperature convention.
RAW_IMU is not the only magnetic source: SCALED_IMU and the primary HIGHRES_IMU
update flags also matter. Secondary MAVLink components never populate the cache.

## Source and lifetime

The service pins selection generation, exact link/system/component, physical
session epoch and discovered component instance. Stop, window Close, shutdown,
source change/retirement, physical loss and serial error close only the owned
output. Re-selecting the same numeric endpoint does not revive an old session.
Current telemetry is cached while the window is idle and reset at every source
transition. Neither a queued receipt nor an old callback may seed the next
source's cache. Reconnect requires an explicit new Connect.

The production integration observes epoch-tagged LinkManager messages, not a
raw retained link pointer or aggregate Swarm/UAS state. Registry freshness
(currently ten seconds of component activity) is an intentional stricter stop
condition than MP10's open-link check. Non-autopilot selections are not silently
replaced or refused solely on component type; initial unknown values remain
zero, matching MP10's immediate first frame.

Settings are copied before validation/open callbacks. An active vehicle's serial
port is refused by canonical device identity. Other secondary tools are protected
by QSerialPort's exclusive open; there is no shared port-owner registry. No TCP,
UDP, vehicle command, serial write-back, baud negotiation or output consumer
acknowledgement is introduced.

## Serial/UI contract and deliberate differences

- 580x440 modeless window; serial ports plus Refresh; eight reference baud rates
  4800/9600/14400/19200/28800/38400/57600/115200, default 57600; Connect/Stop,
  source, status and last complete #9 record. Port/baud/Refresh are locked while
  opening or emitting. Each independently opened window owns its own service.
- A precise 100 ms GUI-thread timer attempts up to 10 Hz. At most one bounded
  frame is staged; partial/zero writes resume, but busy serial buffers skip new
  ticks rather than accumulating minutes of old telemetry at 4800 baud. Skipped
  ticks do not advance the nominal sample counter. The production backend has
  its own 64 KiB cap; a frame is additionally capped at 8192 bytes.
- Complete submission updates the frame count and last record only after all
  bytes are accepted by the local output. It does not prove wire delivery or
  receipt by a MicroDrone device. Stop/error can leave a partial record already
  transmitted; there is no rollback or retry of an uncertain prefix.
- Opening uses synchronous QSerialPort open/configure on the GUI thread; ongoing
  writes are bounded and event-driven. MP10 opens on a worker and blocks its
  worker while writing. Slow device-open latency is a remaining platform gate.
- Altitude/speed use SI with no operator display multiplier or altitude display
  offset. MP10 captures user display units through CurrentState getters; those
  settings must not silently change this Qt wire format. Reported home altitude
  for HIGH_LATENCY relative altitude is separate from display offsets.
- Receipt UTC drives the reference altitude-derived vertical-speed EMA (0.2 s
  gate, float 0.4/0.6). Live receipt timing may differ from MP10 state-update
  batching; replay uses live receipt timing here, not a persisted log clock.
  The cache starts when this window opens, not at application startup.
- The Qt warning/status layout, native serial enumeration order, HiDPI and
  Windows/macOS rendering are not strict reference screenshot parity.

Physical MicroDrone consumer compatibility, unusual driver behavior and native
Windows/macOS serial operation remain acceptance gates. Isolated byte fixtures
and screenshots do not establish hardware interoperability.

Source loss/retirement currently uses the reference's common target-changed
stop message, not a separate stale-versus-disconnected explanation. The generic
service revalidates selected ingress; the production adapter filters unrelated
traffic before its component-registry scan. Floating `std::from_chars` requires
a C++17 standard library implementing that overload; native toolchains must
verify this along with the serial backend.

## Verification

Qt5/audio configure/build3 pass; full278/278 (48.68s). Encoder21, telemetry8,
service40, view-model6 and window7 cases pass ten repeats with no skips; the
sixth repeated suite is the32-case NTF projection follow-up (total3.08s).
The independent console oracle links the actual MP10 encoder and runs on
.NET10.0.11: two full frame goldens,13 formatting cases (exponent threshold17,
subnormal, signed zero and nonfinite values), and saturating float-to-int
conversion are pinned. No universal cross-platform trigonometric last-bit
equivalence is inferred beyond the tested fixtures.

Production X11 exits0 with zero audit failures. The actual Developer button
opens the offline modeless window; injected serial capture verifies initial
seven-record frame, timer, locked settings, explicit restart, source change and
retained-window Close. Both offline/emitting screenshots were inspected. The
existing Developer runtime audit also exercises production epoch-tagged ingress
through an in-process physical-link fixture, including foreign-component
rejection and cache invalidation. No real serial port or network SITL is opened.

Evidence: `/tmp/apm-microdrone.5ewMlI/`, especially `full.log`, `repeat.log`,
`oracle.json`, `x11.log`, `window.png` and `window.png.running.png`.
