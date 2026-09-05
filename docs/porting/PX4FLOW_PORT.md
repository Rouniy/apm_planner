# PX4Flow Setup port

Reference: MP10 `ConfigPX4FlowView.axaml`, `ConfigPX4FlowViewModel.cs` and
`Services/Px4FlowReceiver.cs`; original `ConfigHWPX4Flow.cs` and
`ExtLibs/ArduPilot/OpticalFlow.cs`. This is the grayscale image/focus page,
not the separate legacy `FLOW_ENABLE` Optical Flow page. Neither reference
page implements a firmware uploader; the old inventory acceptance wording
incorrectly assigned that workflow to PX4Flow.

## Surface and source

Optional Hardware has a real `ConfigPX4FlowView` route between Airspeed and
Optical Flow, governed by `displayPx4Flow`. It remains visible offline and
does not depend on the autopilot's parameter-list readiness. Its title,
status, aspect-fit grayscale image and Focus/Video button follow MP10.
Qt additionally exposes an explicit source selector. Selecting the sensor
never changes the active autopilot or fabricates a Swarm vehicle lease.

Generic component discovery accepts heartbeats or a valid RAW8U handshake,
is capped at 64 components and uses the existing physical-link epoch plus
its own component-instance epoch. Link loss, ten seconds without incoming
activity, or a changed heartbeat identity retire the old instance. Image
traffic renews presence because PX4Flow focus firmware can suspend its normal
heartbeat loop. Activity proves presence only, not flight-command capability.
IDs are configurable: historical SYS 81 / COMP 50 are not hard-coded gates.

## Image assembly

The pure assembler accepts RAW8U (wire type 2), positive dimensions,
`size == width * height`, payload 1..253 and exactly `ceil(size/payload)`
packets. The production limit is 1 MiB; the configurable pure core has an
absolute 16 MiB cap. Normal 64x64/17-packet and focus 376x240/357-packet
frames fit. Bytes are literal unsigned grayscale; Qt copies rows into
Grayscale8 with correct stride and keeps the aspect ratio.

All distinct packets are required. Out-of-order packets and identical
duplicates work; conflicting duplicates, invalid geometry and malformed
packets discard the assembly. Copy length follows the negotiated payload,
fixing MP10's overlapping 253-byte copies for smaller payloads. Completed
buffers move without the reference's intermediate full-frame clone/BGRA
allocation. Partial-frame and stale-stream timers expose missing input.

Neither image message contains a frame identifier or timestamp. A local
generation cannot distinguish a delayed packet from the preceding frame
after a new handshake. We do not invent such a guarantee or infer a lost
handshake from a decreasing sequence number (which also means reordering).
No packet-rate-based FPS is displayed.

## Parameters and lifecycle

One application-owned service uses the central `ParameterService` component
policy. Its domain-tagged lease shares endpoint reservation, typed runtime
reads, serialized writes, exact source/name/type/value ACK correlation,
bounded retries and uncertain-write quarantine with existing policies.
Swarm remains an independent, stricter command-capability domain. A physical
parameter frame is consumed exactly once before the legacy UAS gate; it is
not replayed after synchronous completion callbacks have created a successor.
While a component exact operation owns the endpoint, unrelated PARAM_VALUE
names from that component are dropped rather than entering the legacy cache.
The pre-UAS ingress deliberately uses the physical epoch instead of requiring
the legacy UAS/Swarm-session equality; Swarm exact operations retain their own
lease validation. Conflicting instance/domain cache provenance is purged before
successor admission, including values not reread by the successor.

Serial, TCP, UDP client and a listening UDP link with exactly one current
learned peer use the shared single-endpoint route policy. Peer revision and
physical epoch are rechecked before writes. The VIDEO_ONLY type/value is
read from the actual sensor; button state is never an optimistic click result.
Enabling Focus has a default-Cancel bench/disarmed warning because sensor
firmware can suspend optical-flow measurements. This is not an autopilot
arming-state assertion.

The service outlives its page while restoring VIDEO_ONLY=0. Source changes
cannot redirect cleanup to a successor. Uncertain writes retain their fence
before a bounded reconciliation/zero attempt; cleanup failure is a persistent
warning, not an asserted reset. Process crash, power loss or transport teardown
cannot guarantee a sensor-side reset. The original MP10 blindly blocks in
Dispose while issuing zero; Qt never blocks the GUI waiting for an ACK.
Final application shutdown does not introduce a transport-drain phase: cleanup
after LinkManager starts teardown is best-effort, not a guaranteed final zero.

Replay is read-only and pins a source within its replay generation. No live
parameter write may originate from recorded traffic.

## Validation and remaining gates

Full Qt5/audio build and **221/221 tests** pass (16.70 seconds). Evidence:
`/tmp/apm-px4flow.HrHy0E/`, including `build-final.log` and `tests-final.log`.
Tests cover assembler bounds, generic registry, component parameter ownership,
service lifetime and the actual view. The first full run exposed two outdated
test expectations (route counts and cache retention across domains); both were
corrected before the successful full rerun.

Real X11 against the isolated UDP SYS 81 / COMP 50 simulator verifies normal
64x64 (`live.png`) and focus 376x240 (`focus.png`) image rendering; default-Cancel
confirmation (`focus-confirm.png`); exact VIDEO_ONLY 1 and 0 acknowledged writes
(`video-restored.png`); missing-packet staleness (`dropped-stale.png`) and recovery
(`repaired.png`). Leaving the page for DATA while in Focus sends and receives
the zero ACK on the original sensor (`hidden-cleanup.png`). Disconnect clears
the selected image/source (`disconnected.png`); reconnect restores the vehicle
navigation but does not implicitly select the replacement sensor
(`reconnected.png`). App and simulator exit 0, with simulator final mode 0.
No physical device was written. Claude's independent TCP review c189 is
REVIEW-OK after the c188 fixes; builds and tests remained root-owned.

The independent Setup manifest retains all 53 MP10 routes; PX4Flow changes the Qt
inventory to 46 factories (45 mapped reference pages plus QML Plugins), leaving
eight absent reference routes, independently of connection visibility.

Physical PX4Flow hardware/direct USB behavior, representative real replay
files, native Windows/macOS packages and reference screenshot comparison
remain separate gates. Existing legacy Optical Flow and useful APM Planner
modules are retained; this slice does not claim their modern parameter parity.
The registry admits at most 64 components without eviction. Invalid sequence
or short payload resets an incomplete frame; a delayed large-frame packet after
a mode switch can therefore cost one frame, without publishing partial pixels.
