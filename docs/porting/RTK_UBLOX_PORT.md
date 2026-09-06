# SETUP RTK/GPS Inject — u-blox receiver configuration

2026-09-06. This slice replaces the u-blox placeholder callbacks on the actual
`ConfigGpsInjectView` SETUP route. The page remains available without a drone.
It does not claim complete RTK/GPS Inject parity.

## Working workflows

- Serial Connect with Automatically Configure Receiver opens one QSerialPort
  only after a default/Escape-Cancel confirmation naming the port and settings.
  Plain Connect submits the MP10 M8P/F9P setup; it does not reset the base mode
  or implicitly start Survey In.
- Connect with a selected saved base performs setup, disables/resets the old
  base mode, then sends fixed LLA. Restart explicitly performs disable/reset,
  receiver setup and Survey In using the displayed duration and accuracy.
- Save Current Position consumes valid NAV-SVIN/NAV-PVT or the retained RTCM
  base position. Connected Use sends only fixed TMODE3 and a TMODE3 poll,
  without repeating the baud search or receiver reset.
- UBX parsing provides live survey position, duration, accuracy, validity and
  ACK/NAK diagnostics. Serial and NTRIP RTCM parsing/injection remain intact;
  raw UBX is never forwarded to the vehicle as correction data.

The protocol codec implements baud discovery ending at 460800, UART1/USB
protocol configuration, 1 Hz stationary navigation, NMEA suppression, MON-VER,
NAV-SVIN/PVT and the reference RTCM MSM4/MSM7 selection plus raw receiver
messages. QTimer sequences replace blocking sleeps. Restart retains the
reference save-to-BBR/reset timing. Serial receiver bytes refresh the 30 s
silence watchdog even before Survey In supplies RTCM; NTRIP continues to
require valid correction frames.

## Ownership and result semantics

`UbloxBaseStationService` writes through the correction source's already-open
serial handle. Every operation freezes its receiver-session identifier and
disconnect/loss stops subsequent commands without replay on a new session.
NTRIP cannot expose receiver writes. Unsupported source adapters retain safe
default virtual implementations. Sequence size and UBX payloads are bounded.

The reference uses timed writes, not per-command ACK transactions. **Submitted
means bytes accepted by the host serial queue**, not receiver acceptance or a
survey result. ACK/NAK is explicitly diagnostic; a real NAV-SVIN valid flag is
separate from configuration submission. Disconnect/cancellation cannot undo
earlier writes. The initial configuration consent describes these limitations.

## Evidence

Evidence directory: `/tmp/apm-ublox-port.thxRlW/`.
Qt5/audio configure and final application build3 pass; build4 recompiles only
the source watchdog test. Final full **317/317 passes in 110.60 s**
(`full4.log`), including the 41.17 s RTK runtime. Production X11 exits0 with
zero audit failures; root inspected `rtk-native.png` and its consent image.
Earlier failing receipts remain: one legacy test omitted its selected source,
the first runtime checked before queued serial delivery, and the initial
watchdog regression assumed a coarse Qt timer could not round its deadline.
These test assumptions were corrected without weakening the final watchdog
interval or exact transmitted-command checks.

Protocol and service tests cover exact sequences/bytes, both MSM variants,
fixed/survey encoding, source-session loss and cancellation. Model/widget tests
exercise real confirmation and connected actions. `--ublox-audit` runs the
production SETUP route and QSerialPort against a private Linux pseudo-terminal:
Cancel does not open it, Connect does not send TMODE3, Restart does, live NAV
enables Save, and Use sends only fixed TMODE3 plus poll before Disconnect.
The fixture crosses the real 30 s watchdog with 1 Hz NAV-only input before
publishing survey validity; it does not pretend that RTCM is available during
Survey In.
No commands are sent to the user's SITL, vehicle or physical GNSS receiver.

Claude TCP4096 c350 supplied an independent normal-workflow reference check;
its report SHA256 is
`dcf23d6a5697bee7abc3c88198c4bfe2ff24396c12d9012588cd261566d27b6d`.
Claude c351/c352 independently caught the serial watchdog's original
RTCM-only activity rule, which would disconnect a normal in-progress survey.
The primary references are MP10 `ConfigGpsInjectViewModel.cs`,
`ConfigGpsInjectView.axaml` and `ExtLibs/Utilities/ubx_m8p.cs`.

## Remaining work

Septentrio still has no receiver driver; its controls explicitly report that
configuration is unimplemented while injection remains usable. Unicore is
inject-only, matching the MP10 page. ECEF-form saved input, receiver-version
presentation/UBX message counters, per-command verification and real hardware,
caster and Windows/macOS evidence remain outside this slice. The Qt source
stops after its 30 s silence watchdog instead of MP10's 10 s reopen/reconfigure.
Input bounds and explicit consent are intentional differences.
Restart also clears the persisted active fixed base, unlike MP10 which can
reapply the previous fixed base on a later connection; saved rows remain.
The default fixed-position accuracy field preserves the reference's raw value 1 rather than
claiming that the legacy helper's accuracy-unit convention is corrected.

The existing vehicle injection target/large-frame policy is unchanged and is
not upgraded to a new exact-target correction service by this work. UI polish,
minimum-size/HiDPI and automated MP10 screenshot comparison remain separate
gates. Navigation route counts are not proof of complete dialog functionality.
