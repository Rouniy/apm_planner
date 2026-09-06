# Offline MagFit

This port implements Mission Planner 10's offline magnetometer fit as a real
analysis workflow and a separately gated exact-parameter apply workflow. It is
not an alias for onboard compass calibration, and analysis never sends a
vehicle command or changes the source log.

## Reference and routes

The functional references are:

- `MissionPlanner/Services/OfflineMagFitService.cs`;
- `MissionPlanner/ViewModels/OfflineMagFitViewModel.cs`;
- `MissionPlanner/Views/OfflineMagFitWindow.axaml` and its code-behind;
- the legacy `MissionPlanner/MagCalib.cs` fitting implementation; and
- `MissionPlannerTests/Avalonia/MissionPlanner.Tests/OfflineMagFitTests.cs`.

The Qt implementation is split into
`src/services/OfflineMagFitService.{h,cpp}` for bounded offline parsing and
fitting, `src/services/OfflineMagFitApplyService.{h,cpp}` for exact live
application, and `src/ui/OfflineMagFitWindow.{h,cpp}` for presentation and
lifetime control.

`MainWindow::showOfflineMagFit()` owns the shared modeless entry point. Both
`SETUP -> Compass -> Calibrate from Log` (`compassCalFromLog`) and
`TOOLS -> Developer Tools -> Offline MagFit` (`OfflineMagFitButton`) invoke the
same action and window. This avoids two independent analyses or two competing
apply owners while preserving the useful existing Setup routes.

The window is usable while disconnected. It provides asynchronous Browse,
Analyze and Cancel actions, a read-only source path, telemetry throttle
threshold 0..100 (default 30), the reference-default ellipsoid option, bounded
progress, a nine-column result table, apply eligibility/status and bounded
operation history. Changing the source or fitting options invalidates the old
result. Closing the window cancels its analysis and only its own admitted apply
operation; application-owned apply history remains visible after reopening.

## Input and reference math

The analyzer accepts regular, non-symlink `.bin`, `.log` and `.tlog` files. It
pins the canonical path and source metadata and rechecks them during and after
streaming so a changing file is rejected instead of producing a fit from mixed
contents. Parsing, retained samples and callbacks are bounded and cancellation
is polled throughout. Each compass is limited to 1,000,000 samples.

DataFlash parsing accepts `MAG`, `MAG2`, `MAG3` and instance-tagged `MAG`
records. The fitted sample is the logged magnetic vector minus its logged OFS
vector, matching the reference's additive-offset convention. DataFlash uses
all accepted samples; the throttle threshold is only meaningful for telemetry.
Binary and text scalar encodings share the same checked conversion path.

Telemetry parsing admits one magnetometer-bearing MAVLink sender, uses
`VFR_HUD.throttle` as the reference throttle gate, uses `SENSOR_OFFSETS` with
`RAW_IMU`, and maps `SCALED_IMU2`/`SCALED_IMU3` to the other compasses. The
reference sample thinning is retained: samples are grouped into 20-unit 3-D
buckets, at most three samples are retained per bucket, then the largest
one-sixteenth by magnitude is removed. TLOG wire length, CRC/resynchronization,
sender identity, finite values and truncation are checked explicitly.

At least ten samples are required. For each compass the analyzer:

1. computes the mean sample radius;
2. runs an additive sphere fit for X/Y/Z offsets and radius;
3. optionally runs diagonal and then full soft-iron fits;
4. normalizes the three diagonal terms so their squared sum is three;
5. reports sphere and final RMS residuals; and
6. reports spatial coverage as occupied signs of corrected X/Y/Z, from zero to
   eight octants.

The solver is the already-vendored ALGLIB Levenberg-Marquardt implementation,
using numerical differentiation and at most 100 iterations per stage.
Cancellation is checked in its reverse-communication loop and while evaluating
residuals. Every input, parameter and residual must remain finite.

The full residual applies the fitted additive offsets before the diagonal and
off-diagonal matrix. This follows MP10's correction of the original WinForms
MagCalib residual, which left ellipsoid offset terms inert. Qt does not repeat
that original bug. The fit uses the MP10 physical objective and sign convention,
but it is not claimed to reproduce
every ALGLIB iteration or every final bit from MP10.
Ellipsoid stages use the mean raw sample length as their fixed radius, not the
fitted sphere radius. Differentiation step is 0.1, the iteration limit is 100
per stage, and ODI X/Y/Z correspond to the symmetric XY/XZ/YZ terms.

The independent numerical oracle is
`/home/alex/SRC/claude-reports/magfit_oracle.py`, SHA256
`736ffaed1c2f867036151961fa4a28dcab25669e24bc5ce664c900527dd2dba8`.
It is evidence only for the numerical objective, offset sign and fit quality;
it is not evidence for transport safety, provenance, parameter application or
ALGLIB-identical results. In particular, a weak four-octant ellipsoid fit is
non-unique. Low RMS and finite coefficients do not by themselves make it safe
to apply, so the UI warns when coverage is below eight rather than inventing an
arbitrary coverage hard gate.

## Analysis provenance and apply eligibility

A successful fit can remain analysis-only. The report exposes
`applyEligible`, an explicit `applyUnavailableReason`, logged device IDs and
the logged frame parameters. Results remain visible when application is
refused.

TLOG results are always analysis-only. Modern telemetry magnetometer fields may
already include scale, soft-iron and motor corrections, while
`SENSOR_OFFSETS` is insufficient to reconstruct the raw frame. Applying the
small residual OFS from such a stream could erase a valid onboard calibration.

DataFlash application is enabled only when records preceding the accepted
samples prove the limited compensation model supported by this port:

- every accepted MAG record has complete finite OFS, explicit zero
  `MOX/MOY/MOZ`, and `Health=1`;
- prior `COMPASS_DIA*` is exactly identity or exactly disabled, `COMPASS_ODI*`
  is zero, and `COMPASS_SCALE*` is zero or one;
- each compass has an explicit, stable, nonzero integer `COMPASS_DEV_ID*`;
- a present nonzero `COMPASS_PRIO*_ID` agrees with that device ID; and
- stable integer `AHRS_ORIENTATION`, `COMPASS_ORIENT*` and
  `COMPASS_EXTERNAL`/`COMPASS_EXTERN*` values describe a supported frame.

Missing defaults are not treated as proof. Conflicting, changing, non-finite,
custom-rotation or unsupported compensation metadata makes the result
analysis-only with a reason. The implementation does not invert arbitrary
pre-existing DIA/ODI, scale, motor or rotation corrections.

Even this metadata does not prove that the log came from the currently selected
vehicle. The confirmation therefore names the exact source and endpoint and
asks the operator to establish that relationship. The live apply service then
requires matching current device IDs and matching orientation/external frame
parameters before it can prepare a plan and at every write gate.

## Exact and fail-closed application

Preparation freezes the successful report, ordered results, calculated values,
source, selected target generation, physical vehicle instance, live parameter
types and the relevant live snapshot. Apply requires a settled current target,
a fresh heartbeat no older than three seconds, a disarmed ArduPilot autopilot
component, exactly one vehicle on the physical link and an eligible exact
single-vehicle route. The asynchronous confirmation defaults to Cancel and
shows the endpoint, source, result quality, poor-coverage warning, lack of
rollback and same-vehicle requirement.

The service validates calibration bounds before any write. Current firmware
limits are used where safely available; otherwise the conservative defaults
are absolute offsets below 1800 on each axis and RMS at most 16. Sphere radius must be strictly between
150 and 950. Ellipsoid diagonal terms must be between 0.2 and 5 and absolute
off-diagonal terms below 1. If an ellipsoid was fitted but the current firmware
does not expose the required DIA/ODI schema, application is rejected with an
instruction to reanalyze sphere-only; coefficients are never silently dropped.

The ordered write plan optionally starts with `COMPASS_LEARN=0`, then writes
OFS X/Y/Z for each fitted compass and, for an eligible ellipsoid, DIA X/Y/Z and
ODI X/Y/Z. It neither reboots the vehicle nor starts onboard calibration.
Completion explicitly asks the operator to reboot and verify heading before
flight.

One immutable plan holds both exact command and parameter endpoint
reservations for the entire operation. This prevents calibration/command work
from entering the same endpoint while the parameter sequence is active. Every
first or retried write has a final callback that rechecks:

- the same target generation, physical instance and eligible route;
- fresh disarmed vehicle state;
- the immutable plan, schema, device IDs and frame parameters;
- the relevant current parameter values; and
- exact encoding and representability of the value about to be sent.

Success requires the shared exact ParameterService acknowledgement for the
same name, runtime type and value. A submit notification is not success.
Receipts are appended only after a matching acknowledgement. The terminal
report records the operation ID, source, endpoint, total writes, confirmed
writes, remaining writes, ordered receipts and one of Completed, Cancelled,
Rejected or OutcomeUncertain. History is bounded to 512 entries and the whole
apply has a ten-minute monotonic deadline.

Cancellation and target retirement stop later writes but cannot roll back an
already transmitted parameter. An attempted unacknowledged write is reported
as uncertain, its confirmed receipts are preserved, and the UI does not
automatically retry. The admitted operation ID is published before external
callbacks, synchronous completions are fenced, and page close/rebinding can
cancel only the operation owned by that window. Shared exact-route quarantine
reduces late-ACK aliasing, but MAVLink PARAM_VALUE has no transaction nonce;
arbitrarily delayed indistinguishable responses remain a protocol limitation.
The shared six-second isolation interval also follows parameter-list traffic.
Immediate apply while that protocol is still busy refuses without writing;
there is no automatic retry of the whole apply operation.

## Verification and remaining gates

Qt5/audio configure/build pass, focused11/11 pass, and the final full252/252
CTest run passes in45.44s. Production X11 exits0 with zero audit failures.
Evidence is in `/tmp/apm-magfit.KUILP5`, including `build-qt-dialog.log`,
`full-qt-dialog.log` and `x11-qt-dialog.log`. Root inspected
`magfit-consent.png` and `magfit-consent.png.window.png`: the full warning,
exact source/target, both consent buttons and the nine-column result table
are readable. The final picker explicitly uses a themed asynchronous Qt dialog
on every platform, matching the other Developer tools.

Commands were `cmake -S . -B build-codex-qt -DAPM_QT_MAJOR=5
-DBUILD_TESTING=ON -DAPM_REQUIRE_QT_AUDIO=ON`,
`cmake --build build-codex-qt --parallel 12`, and
`ctest --test-dir build-codex-qt --output-on-failure --parallel 12`.
The X11 application command is `apmplanner3 --developer-vehicle-tools-audit`
with DISPLAY=:0, QT_QPA_PLATFORM=xcb and QT_QUICK_BACKEND=software.
All vehicle writes use an isolated in-process sys234 fixture; no network SITL
or physical flight controller was modified.

The read-only `offline_magfit_probe` target analyzed
`/home/alex/SRC/ArduPilot/logs/00000002.BIN` in sphere and ellipsoid modes.
Both readers retain3x2299 samples and report4/8 coverage. Qt sphere offsets
are(5.36841867,13.12467405,-19.68310715), radius580.74281917 and
RMS0.27385235; the independent oracle's maximum offset delta is0.00003136,
radius delta0.00002864 and RMS delta below1e-12. Ellipsoid RMS differs
by0.00079947; coefficients deliberately are not asserted equal on this weak
sample geometry. The source SHA256 before and after remains
`16726aaea629c615e4828b72217c08240552061bba7d0eeff2ac879149cf6d5a`.
The production reader also warns about an incomplete40-byte final record.
JSON inputs/results/comparisons are retained in the evidence directory.
The oracle has weaker missing-metadata checks and uses forward rather than
ALGLIB central differences; none of its eligibility claims authorizes Apply.

The focused analyzer coverage includes additive sphere sign/radius, full
ellipsoid fitting, three compasses, modern and legacy DataFlash forms,
binary/text parity, scalar encodings, TLOG throttle/thinning/one-sender rules,
corruption/truncation, input mutation, cancellation and resource limits. It
also covers positive stable provenance and analysis-only outcomes for missing,
changed or unsafe device/frame/health/motor metadata.

Apply tests cover ordered sphere and ellipsoid writes, leaving matrix terms
untouched for sphere-only results, device/frame/schema snapshot failures,
exact acknowledgement receipts, arm/target ABA/route retirement, bounded
deadline, callback deletion, early operation-token publication and truthful
partial/uncertain cancellation. Window tests cover the MP surface, offline
analysis, source/settings invalidation, asynchronous picker cancellation,
worker lifetime, default-Cancel consent, exact apply, history after reopening,
owned versus foreign cancellation and close after a transmitted write.

The production runtime audit proves that Developer Tools and
Compass open the same window, picker and consent cancellation send nothing,
an eligible DataFlash fixture produces the expected numeric result, exact live
metadata is required, one accepted apply emits the expected ordered writes and
the source file remains byte-for-byte unchanged. Its192-sample full-coverage
sphere produces OFS(20,-10,5); the four confirmed writes are COMPASS_LEARN0,
COMPASS_OFS_X20, COMPASS_OFS_Y-10 and COMPASS_OFS_Z5. The fixture respects the
real6s uncertain-write and post-list isolation intervals instead of disabling
them. Initial missing-include, obsolete-tooltip and asynchronous/native-picker
test failures are retained in the evidence logs and corrected in the final run.

Physical flight-controller application, representative field logs across
firmware families, native Windows/macOS packaging and direct MP10 screenshot or
numeric parity remain separate gates. A successful offline fit is not a
preflight compass-health certification, and no documentation or UI message
should describe it as one.
