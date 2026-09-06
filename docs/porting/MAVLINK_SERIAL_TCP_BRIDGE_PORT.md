# MAVLink Serial TCP Bridge port

## Scope and verification

The native Qt workflow connects one TCP client to one autopilot UART through
MAVLink `SERIAL_CONTROL`. It does not configure `SERIALn_*` parameters, flash a
device, or offer a general-purpose TCP/MAVLink router. The Developer Tools action
`MavlinkSerialTcpBridgeButton` resolves the shared application action
`actionMavlinkSerialTcpBridge` and opens one modeless
`MavlinkSerialTcpBridgeWindow`.

Verification status (2026-09-06): **Qt5/audio build and tests pass**. Focused7/7
(35.08s), full257/257 (46.54s), ten repeats of each of the3 bridge suites
(3.44s), and the production X11 audit (exit0, zero failures) pass. Root inspected
the readable complete window and default/Escape-Cancel confirmation at
`/tmp/apm-serial-bridge.tOnvpg/serial-consent.png` and `.window.png`. Final build,
focused/full/repeat and X11 logs are in that directory. No real UART or aircraft
was written; the fixture uses in-process sys234/link910110 and loopback TCP only.

Developer now has24/32 working actions and8 unavailable; this fills one existing
action and adds no Setup page or fixed Tools menu item. Full port, hardware and
native Windows/macOS/reference visual parity are not claimed complete.

## Reference anchors

Paths below are relative to `/home/alex/SRC/MP/MissionPlanner` unless specified.

- `ViewModels/MavlinkSerialTcpBridgeViewModel.cs:28`: defaults; lines 35–64:
  complete device and baud inventories; line 126 onward: frozen target and
  confirmation before starting.
- `Views/MavlinkSerialTcpBridgeView.axaml`: controls, counters, and start/stop
  surface; `Views/MavlinkSerialTcpBridgeWindow.cs`: shared-window lifetime.
- `Services/MavlinkSerialTcpBridge.cs:101`: UART session; line 211 onward:
  280-byte TCP reads, 50 ms serial poll, bounded reverse queue and client lifetime.
- `ExtLibs/ArduPilot/Mavlink/MAVLinkInterface.cs:3093`: flags, 70-byte data
  chunks, 10 ms write pacing and zero-flag release.
- `/home/alex/SRC/ArduPilot/libraries/GCS_MAVLink/GCS_serial_control.cpp:33`:
  actual firmware handling; lines 105–116 change flow control/baud;
  lines 151–156 wait inside the handler; the device switch has no SHELL case.

## Controls and defaults

All 15 reference device choices are retained, in reference order:

| Choice | Wire device |
| --- | --- |
| TELEM1 | 0 |
| TELEM2 | 1 |
| GPS1 (default) | 2 |
| GPS2 | 3 |
| SHELL | 10 |
| SERIAL0–SERIAL9 | 100–109, respectively |

The ten baud choices are Keep current baud (`0`, default), 4800, 9600, 19200,
38400, 57600, 115200, 230400, 460800 and 921600. The TCP listen port defaults to
500; the UI accepts 1–65535. Service/server port zero is supported only for
ephemeral-port callers and isolated tests. Binding a low port may require OS
permission; failure is reported, not worked around with elevated privileges.

Remote clients are disabled by default: bind is `127.0.0.1`. Explicitly enabling
the checkbox binds all IPv4 interfaces. There is no TCP authentication or
encryption; even localhost callers must be trusted. Remote exposure requires a
trusted network and appropriate firewall restrictions.

## Wire and ownership contract

The application-owned `MavlinkSerialTcpBridgeService` freezes the selected target
generation, exact endpoint, vehicle instance and physical link-session epoch
before the default-Cancel confirmation. Before ordinary traffic it revalidates
a fresh, disarmed autopilot component 1, the original selection, one
known MAVLink system on that link, and the production physical route. The
heartbeat freshness limit is 3 seconds. Arming, target/session changes, discovery
loss or a failed route check stop ordinary UART traffic.

`SERIAL_CONTROL` has **no destination system/component fields**. An exact local
link and a single discovered system do not prove that a transparent radio,
router or remote network cannot forward bytes elsewhere. Use only a dedicated,
trusted physical path. Production permits serial, outbound TCP, unicast UDP
client, and a UDP listener with exactly one peer whose ingress revision and
session remain current. TCP listeners, known fan-out, playback and unsupported
routes are rejected. Signing-required links must also be ready. The generic
`ExactLinkTransmitter::sendMessage` rejects `SERIAL_CONTROL`; only the private
bridge entry point admits its bounded wire payload under a live link epoch.

Listening alone does not claim the UART. When the single TCP client connects:

- OPEN: selected device, selected baud (zero preserves baud), timeout 100 ms,
  count zero, `EXCLUSIVE | RESPOND | MULTI`.
- Poll: every 50 ms while eligible and connected, the same flags and timeout,
  zero baud and no payload. Target safety is checked on this timer too.
- TCP writes: pull at most 280 bytes, split into at most 70-byte packets,
  `EXCLUSIVE`, timeout/baud zero, with `RESPOND` on the actual final chunk.
  Subsequent data chunks use a precise Qt timer and a monotonic minimum 10 ms
  interval checked around callback boundaries, without blocking the Qt event
  loop. The first chunk can be submitted immediately. This is local submission
  pacing, not measured physical UART delivery timing.
- Replies: exact link/epoch/system/component and device must match, and the
  `REPLY` flag must be set. Payload count is bounded to 70. MAVLink 2 omitted
  zero-tail bytes are decoded as zeros; this is not string processing.
- Graceful TCP disconnect: retain the bounded input and the exclusive client
  slot, stop read polls, and pace already accepted bytes to MAVLink before
  releasing the UART. Late UART replies have no TCP sink and count as dropped.
  Stop, fatal socket failure, window close or target loss cancel pending bytes.
  Attempt one empty zero-flag, zero-timeout, zero-baud release to the still-valid
  original instance. Cleanup can release after arming or selection change, but
  never retargets to a replacement instance or changed physical session.
  Heartbeat age alone does not suppress this one cleanup attempt: the registry
  must still contain the original instance and epoch, and the full physical
  route check must pass. A retired instance is never resurrected for release.
  Terminal status distinguishes submitted, refused and uncertain submission;
  none of these is an acknowledgement from the remote UART.
  A second system discovered after the claim also blocks the untargeted release;
  the previous UART may then need recovery through another connection or reboot.

There is no command ACK, UART-claim ACK, release ACK, reliable stream sequence,
or automatic retransmission of serial data. Counters mean bytes submitted to
the local MAVLink transport or written by the local TCP socket, not bytes
acknowledged by an attached device or TCP application. A reply does not prove
exclusive ownership. Missing/unsupported UARTs may be silent.

The firmware can disable UART flow control for exclusive access and retain a
requested nonzero baud. Release does not restore either setting. Selecting the
telemetry UART carrying this very session can lock out MAVLink and make release
impossible; selecting a GPS UART can interrupt navigation input. ArduPilot's
referenced handler does not implement SHELL despite retaining the reference
choice; a genuine non-ArduPilot autopilot is not excluded by family.
Firmware with the shown handler may wait up to 100 ms **inside its main-thread
handler** for UART bytes, even without the `BLOCKING` flag. A 50 ms GCS poll is
not a guaranteed end-to-end latency or flight-safe scheduling guarantee.

## Qt lifetime and bounds

`SerialBridgeTcpServer` uses QtCore/QtNetwork events, not blocking socket waits.
Only one active client is accepted; extra clients are rejected without replacing
it. The read buffer is 32 KiB and uses TCP backpressure; outgoing queued data is
limited to 64 KiB. Over-cap writes are rejected before any prefix is queued.
The service stops on output overflow rather than silently discarding serial
bytes and pretending the stream stayed intact. Graceful FIN keeps at most the
bounded socket input plus one 280-byte service batch until drained; additional
clients cannot take over during this drain. Stop and fatal errors clear owned
pending data; stale-client callbacks cannot feed a replacement.

The service is application-owned, but this window's owned bridge operation is
stopped when the window closes. Closing a Developer page is not the same as
closing the separate bridge window. MainWindow explicitly disposes the bridge
window on both normal close and destructor paths; LinkManager has a service
shutdown path before physical transports are removed. Normal window stop sends
guarded best-effort release. LinkManager's shutdown-only path has already closed
route admission, so it cannot submit release; abrupt service destruction also
blocks child notifications and closes local TCP without bypassing route guards.
Another connection or a vehicle reboot may be needed to recover a locked UART.
Operations and callbacks are token/identity scoped.

There is no Terminal/UART-resource interlock: the existing Qt Terminal is a
native host-serial workflow, not another `SERIAL_CONTROL` owner. No global claim
is made against other applications, other GCS stations, firmware users of the
UART, or a future MAVLink Terminal implementation.

## Deliberate differences and remaining verification

Functional differences are separate from GUI polish:

- Safety gates are stricter than MP10: exact physical-session/instance checks,
  fresh disarmed autopilot, known-single-system admission, trusted dedicated
  route and REPLY filtering. These do not establish remote topology proof.
  All known systems count until forgotten; stale discovery entries can keep a
  formerly shared link ineligible. Same-system additional components do not.
- Qt rejects additional active clients and fails closed on reverse-buffer
  overflow. MP10's reverse channel instead counts dropped packets when full.
- Qt marks the actual final data chunk `RESPOND`. MP10's `(length / 70) + 1`
  packet calculation omits that flag for exact multiples of 70. This off-by-one
  behavior is not copied. Timer pacing avoids MP10's blocking 10 ms sleeps.
- Layout, typography and native widget sizing are Qt adaptations, not a
  pixel-identical Avalonia rendering. All listed controls and defaults remain.

Verified tests include `test_serialbridgetcpserver.cpp`,
`test_mavlinkserialtcpbridgeservice.cpp`,
`test_mavlinkserialtcpbridgewindow.cpp`, and the generic-transmitter restriction.
The production Developer runtime fixture exercises Cancel/Yes, shared-window
routing, 281 binary TCP bytes split to UART, 74 reply bytes including a trimmed
zero tail, wrong-system/component/device/non-REPLY rejection, counters and one
best-effort release after arming. This fixture does not prove real firmware UART
ownership, physical serial timing, remote network isolation or hardware baud
restoration. A separate monotonic transmitter-clock regression asserts at least
10 ms between data frames while both the route validator and the frame writer
pump nested events. This proves local submission pacing, not physical UART
timing or delivery. Adapter tests cover4097-byte graceful-FIN tails,128KiB input
through32KiB backpressure, extra-client rejection, bounded output and cancellation.

Initial focused verification found a real EOF callback crash: facade destruction
deleted its child QTcpSocket before QtNetwork returned from the sender's signal
stack despite deleteLater. Retired socket/listener objects now detach from the
parent before deferred destruction; tests assert survival during callbacks and
eventual deletion, including the original crashing row. Shutdown after the event
loop has ended may defer reclamation until process exit; no active I/O remains.
The production fixture's first Cancel also raced the queued disarmed heartbeat
from the previous RemoteLog scenario; it now waits for both target-manager and
registry state before clicking, without weakening the real safety gate.

Claude reviewed the full MP10/ArduPilot contract and TCP, window, core and final
lifetime/pacing/cleanup deltas over TCP4096 (c245–c253), with full reports verified
by byte count and SHA256. Three Codex streams implemented the adapter, core and
window; root integrated, cross-reviewed and scheduled all builds/tests. Reviews
changed graceful-FIN handling, stale-heartbeat cleanup and release reporting.
Reference device choices,100ms poll timeout and generic autopilot support were
retained instead of narrowing the workflow or silently changing its wire timing.
