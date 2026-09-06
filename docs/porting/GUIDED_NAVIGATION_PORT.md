# DATA guided dialogs and Terrain 3D click

## Reference and scope

Primary reference: MP10 `FlightDataViewModel.SetGuidedAltitude`, `FlyToHere`,
`FlyToCoords`, `Terrain3DViewModel.SendGuidedTargetAsync`,
`MAVLinkInterface.setGuidedModeWP` / `doCommandIntAsync` and `Locationwp.Set`.
The retained Follow Me and External Guided wire/retry policies are unchanged;
their admission now also checks the shared exact command channel.

DATA's real map now exposes Fly To Here, Fly To Here Alt… and Fly To Coords.
Alt… edits the shared guided altitude; it does not fly to the clicked point.
The standalone Terrain window exposes that same editor and sends a consented
terrain-intersection target. Hover remains inspection-only. Terrain elevation
AMSL is never substituted for the commanded altitude.

## Altitude and coordinates

`GuidedAltitudeStore` is application-owned, in-memory intent keyed by the exact
physical autopilot instance. Nothing is automatically admitted from saved
preferences, live vehicle altitude or an observed POSITION_TARGET message.
Fresh settled selection and an autopilot heartbeat at most3000ms old are
required; being armed does not disable a flight-navigation operation.
Revisions prevent away/back and value-change ABA across dialogs. At most24
instances are retained, following the bounded telemetry registry.

The real editor accepts signed finite float-representable metres with Relative,
Absolute or Terrain frames. It displays the current altitude units. Like MP10,
`guided_alt` stores a display-unit default, while `guided_alt_frame` stores0/3/10;
the default is10m for Copter and100m otherwise, only as a dialog prefill.
Cancel changes neither preferences nor session intent. Persistence failure is
reported and prevents an automatic follow-on movement, although already
accepted session intent remains changed. The two preference writes are not a
group filesystem transaction.

Fly To Here and two-coordinate input ask for an altitude when intent is unset
or zero. Three-coordinate input accepts `lat;lng;alt` in display altitude units.
Latitude/longitude are bounded WGS84 degrees; (0,0) is rejected, either zero
axis alone remains valid. All vehicle-bound coordinates truncate to E7 integers.
Zero can be stored in the editor, but a zero-height movement is explicitly
refused: MP10's deeper setGuidedModeWP also suppresses it, without reporting why.

If already in Guided and an explicitly accepted local point exists, changing
altitude offers that same point at the new height; it never falls back to the
vehicle's current position. The altitude editor's local commit remains even
if the subsequent movement confirmation is cancelled.

## Exact command and outcome

One application-owned `GuidedNavigationService` reserves both the existing
GuidedTargetService lane and the VehicleCommandService exact endpoint lane.
The former prevents interference between Follow Me updates; the latter alone
owns COMMAND_ACK, retries and quarantine. Windows own neither ACK consumers
nor the transaction lifetime.

The typed COMMAND_INT operation shares the COMMAND_LONG exact reservation,
pending-command and late-ACK domain: MAVLink ACK cannot distinguish these wire
forms. Its final route, physical epoch, signing and intent validation occurs
immediately before the writer, on every attempt.
An accepted or rejected ACK after a retry also starts quarantine. Additional
terminal ACKs cannot shorten that interval, because more than one transmitted
frame can have an indistinguishable reply. The existing single-transmission
early-drain policy is retained.

Up to16 opaque prepared confirmations can coexist across windows. Each UI
discards only its own receipt on Cancel/close; executing one consumes only that
receipt and still revalidates the frozen target and intent. Live DATA, PLAN and
Simulation maps all attach the shared controller. Their former direct UAS
guided sender and private100m prompt are removed; unbound actions are hidden.

DO_REPOSITION uses p1=-1, p3=0, p4=NaN, current/autocontinue0. DATA requests
CHANGE_MODE; Terrain uses p2=0 and frame3 GLOBAL_RELATIVE_ALT. This deliberately
preserves MP10's numeric-height reinterpretation even when DATA saved an
Absolute/Terrain frame, with an explicit default-Cancel warning before movement.
Terrain requires an explicit |height|>=0.01m. A negative nonzero height is not
arbitrarily replaced with a positive-only limit.

The reference sends an initial frame plus three identical retries, each after
2000ms. An early audit incorrectly called this three total transmissions;
root corrected it against the actual `retrys = 3` loop. The Qt one-shot follows
the four-attempt limit, with a30-second absolute bound and no retries after
IN_PROGRESS. Follow Me's separate prior retry policy is not changed.

ACCEPTED, rejected and uncertain outcomes are distinguished. No failure is
silently converted to the old Plane MISSION_ITEM/other SET_POSITION_TARGET
fallback, and an ACK is not proof the vehicle has arrived. Only accepted exact
commands record the local target, and only while the original intent revision
is still current. Closing/cancelling prevents later attempts, not an already
applied target. The application retains ownership until pending ACKs and the
central late-ACK quarantine drain.
The extra admission query also closes an A→B→A gap: selection change may retire
the old GuidedTargetService reservation before its exact command drains.
Follow Me/External now report a definite Busy before opening/submitting on that
endpoint, rather than misreporting a blocked writer as transport uncertainty.
This changes admission/error semantics, not their flight-command payloads.

## Terrain identity

Production position/attitude and heartbeat come from the same exact registry
instance, including link-session and vehicle-instance epochs. Old worker
results cannot publish into a replacement instance. A click uses the actual
rendered camera and snapshot, not newer telemetry; its hit point and instance
are frozen across consent. Stale rendered position and changed selection or
intent refuse submission. Render status and command status use separate labels.

## Remaining differences and gates

- Terrain imagery draping still requires the canonical tile cache/atlas.
- No unacknowledged legacy compatibility fallback or import of another GCS's
  legacy guided-intent writes. Existing Follow Me/External inputs retain their
  separate accepted-target state; this slice does not reinterpret those loops.
- Local accepted-target bookkeeping is explicit; MP10's DO_REPOSITION success
  path itself does not update GuidedMode.x/y, while legacy readback does.
- The wire has no transaction nonce. Quarantine limits late-ACK confusion but
  cannot prove a vehicle's physical movement or universal old-firmware support.
- Default-Cancel movement confirmations, exact target banners and cancellation
  progress extend MP10. Reference screenshots, HiDPI and native Windows/macOS
  evidence remain separate from functional Linux verification.
- This is one Tools workflow slice, not full Tools/SETUP/Settings parity.
  Signing transitions, DATA log-tool routes and BIN/LOG MATLAB remain queued;
  Settings follows single-vehicle tools and Swarm remains last.

## Verification

Qt5 with required Multimedia/TextToSpeech configured and built successfully.
Final full CTest: **300/300 pass in51.93s** (`full6.log`); previous full runs
also passed. The six new unit suites each passed ten repeats in30.52s:
exactcommandint, guidedaltitudestore, guidedaltitudedialog,
guidednavigationservice, guidednavigationcontroller and guidednavigation_admission.
The admission suite includes both away/back quarantine and the active legacy
guided session's definite Busy stop when the exact command lane becomes owned.

Actual production MainWindow X11 audit exits0 with zero failures: DATA and the
real modeless Terrain window exercise editor, default-Cancel consent, zero-TX
Cancel, exact COMMAND_INT bytes and terminal ACK. Root inspected
`final.altitude.png`, `final.consent.png`, `final.terrain-consent.png` and
`final.terrain.png`. This uses an isolated in-process TCP test link, not a real
socket/vehicle; no network SITL or physical vehicle was changed.
The fixture supplies HOME_POSITION and checks the existing legacy ACK lane
before consent, fixing startup-request races without weakening production gates.

Native X11 dialog suite7/7 and controller suite30/30 pass. Terrain suite11/11
passes plus three full repeats after fixing the old hover test: the generic
word terrain was already in its initial hint, and native mouse moves to an
unchanged cursor position need not emit an event. The test now waits for exposure
and distinct edge/centre event delivery before asserting an actual AMSL hit.
Earlier failed attempts remain in the evidence directory, not counted as passes.

Evidence root: `/tmp/apm-guided-navigation.S8Mmwd/` (`configure.log`,
`build6.log`, `full6.log`, `repeat4.log`, `x11-4.log`, `widgets-x11-4.log`,
`terrain-x11-6.log`, `terrain-x11-6-repeat-1..3.log` and the screenshots).
Three Codex implementation/test streams contributed. Independent Claude TCP
reviews were fully read and SHA256 verified; c314's surviving direct sender and
single prepared-plan slot, c315's retry-ACK quarantine gap and root's additional
duplicate-ACK/synchronous-cancel findings were corrected before the green gate.
Final c316 review found no new defect. Report hashes:

- c313 architecture: `e8b7f363e6589416c9534334722f7990226dd5b62514818ca727df934f92c221`.
- c314 controller review: `23e8787b7b5df19988965dfd32ebd4d4ff3a8aed03cc34e1142281dea4ed6076`.
- c315 re-review: `227c5ebfb48511d78adc026cd54bf9897e7c23f941acbfb59b9d9ee38091156b`.
- c316 central review: `06b4f857d9c3786da45f25d2f3300609d4ccdffd328c08ffc483e0a90664410c`.
