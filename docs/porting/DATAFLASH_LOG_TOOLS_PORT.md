# DATA DataFlash Logs tools

## Scope and reference

MP10 `GCSViews/FlightDataView.axaml:678–705` has eight DataFlash actions. The
new Qt DATA tab retains that inventory and connects seven actual workflows:
Download DataFlash Log Via Mavlink, Review a Log, Auto Analysis, Create KML + gpx,
Convert Bin to Log, Create Matlab File and Organize tlog/rlog/bin/log. Only Geo
Reference Images remains explicitly unavailable, not redirected to an unrelated
tool. Telemetry Logs is a separate still-missing tab. The existing useful
Quick (Legacy) and standalone LogAnalysis are retained.

DATA now has eight tabs: seven mapped MP10 surfaces plus Quick (Legacy).
Eight reference tabs remain absent: Drone ID, Gauges, Transponder, Servo/Relay,
Aux Function, Scripts, Payload Control and Telemetry Logs. Seven implemented hub
actions do not make this screen or the overall Tools inventory strict-complete.

Review opens the existing modeless LogAnalysis with the selected file; this
does not claim a full MP10 LogBrowse replacement. Download opens the existing
exact-target LogDownloadWindow, including its disconnected state. The new
controller shares MP10's cached selected source across offline actions and
opens a fresh picker for Review. Filesystem operations run off the GUI thread.

The controller provides real named file pickers, default-Cancel KML/GPX and
organization consent, modeless progress/cancel and a readable Auto Analysis
report. MainWindow Close requests cancellation and waits for the active worker
after the other tools' close approvals. A denied close must not disable DATA.

The production Review fixture exposed a real retained BinLogParser defect: its
first timestamp-bearing FMT was not activated until a later FMT arrived, so a
valid GPS-only log could be treated as corrupt. Timestamp-schema activation
now occurs before the active-schema check; the fixture retains the minimal
valid log rather than adding a dummy FMT to conceal the problem.

## BIN/LOG MATLAB addition — verified

Create Matlab File prepares an immutable read-only source/schema plan, shows
the exact `<input>-<record count>.mat` in default-Cancel consent, and streams
actual Level-5 numeric matrices and nested cell/string content. It never
replaces existing output. Cancellation and source/output revalidation guard
publication; application close drains the worker.
See `DATAFLASH_MATLAB_PORT.md` for ASCII/UTF16 semantics, reference defects
and bounds. Build 5/full 305/305 pass (53.53 s); production X11 has zero audit
failures, and four suites each pass ten repeats (6.65 s). All 16 actual Release
oracles pass, including 14 small fixtures, real 292 legitimate variables and
the 2,000,003-record case. The oracle requires the shipped overlay with
Auto 3/Turtle 28 assertions and reference bin path and overlay SHA256: c331 exposed a stale
Debug reference, not a valid Copter 28 exception. That exception was removed;
the shared mode snapshot remains unchanged. The 100001 MSG/PARM firmware
prepass remains. Real-log exceptions are explicitly raw-source-validated.
Four independent comparison negative/proof tests pass.
The prior build 4 Debug large case took 7.69 s / 12,932 KiB with matching output.
The 304-test evidence below belongs to the prior six-action hub and is history.

## KML and GPX

The pair is generated from one verified private source copy. Outputs remain
`<base>.kml` and `<base>.gpx` beside the source, in KML-then-GPX order. Existing
outputs are never replaced. A failed first export does not start the second;
a completed KML remains available if the later GPX fails or is cancelled.
This is per-file publication, not an atomic two-file filesystem transaction.
The existing KML exporter retains its other callers' explicit replace policy;
this controller only lets it write a private staging file.

The GPX exporter uses the raw DataFlash reader, not timestamp-repairing plot
parsers. It preserves record order, one track/one segment, GPS-only point
selection, format-character scaling, intended MP10 attributes and whole-second
UTC output. No new segments are invented for clock jumps or missing fixes.
GPS2 may seed the shared GPS clock without producing GPS2 track points. Raw
boot-time precedence, FMTU instance-clock discovery, float string conversion
and no-anchor year-0001 behavior are checked against the actual primary-service
oracle, rather than inferred from a Python reimplementation.
Non-finite coordinates are explicitly excluded rather than producing invalid
GPX. Physical movement and live telemetry are not part of this workflow.

The actual MP10 `DataFlashLog.ExportGpx` throws `XmlException` before the first
point: `WriteStartElement("gpx")` chooses the empty namespace and the following
`xmlns` attribute tries to redefine it. Qt intentionally implements the valid
GPX namespace. Therefore there is **no successful actual-GPX byte-parity claim**.
The oracle captures the original error and compares Qt's parsed GPX against
actual MP10 `ReadTrack` coordinates and its exact timestamp formatting: all
1210 points in the real fixture match. Namespace correctness is an intentional
functional repair, not an unnoticed serialization difference.

The actual clock fixture confirms TimeMS precedence over GMS (UTC23:59:34/35).
Local year-one conversion is clamped rather than leaking a BCE date into GPX;
unrepresentable Int64 timestamp conversions and years beyond9999 fail closed.
These boundary fixes are included in the verified build4. Five additional BIN
fixtures match actual .NET for all17 analysis results in UTC, with GPX points
compared against separately executed reference oracles in UTC and Asia/Nicosia.
The explicit clock/zero-relative fixtures also match in Asia/Nicosia.

Historical-zone fallback is **not** universally identical: actual .NET10 under
`TZ=America/New_York` formats a zero-relative/no-GPS-epoch point as
`0001-01-01T04:57:00Z`, while Qt5 emits `0001-01-01T05:00:00Z` on this Linux host.
Qt's out-of-range local-time policy differs from .NET's historical offset policy.
Both are synthetic dates, explicitly warned about, not usable flight UTC.
The anchored path deliberately stays UTC instead of MP10's local-time roundtrip,
so DST transitions also remain a compatibility gate. Do not infer general
timestamp parity from the real anchored fixture. Current GPS/UTC offset18s
must be maintained if the leap-second table changes.

## Auto Analysis

All 17 MP10 checks are ported, in order: Empty, Vibration, GPS, VCC, Compass,
Motor balance, NaN, Event/Failsafe, Brownout, Duplicate data, Parameters, PM,
Pitch/Roll, Thrust, IMU mismatch, Autotune and Optical flow. The analysis is
advisory, never writes parameters and does not certify flight readiness.
Unknown/NA are distinct from Good. Reports preserve alias precedence, selected
field order, last-wins parameters, source line alignment and raw boot time.
Indexed nearest/previous joins avoid the reference's quadratic scans while
retaining source-order ties.

The reference's heuristic limitations are not silently rewritten: modern
instance-bearing GPS/MAG/IMU records are mixed by FMT name; legacy IMU2 absence
can produce NA even with two modern IMUs; the legacy FRAME_CLASS motor table
and CTUN climb-unit assumptions remain. These need a separate deliberate
algorithm update, not an unannounced parity change. Source format errors and
resource-limit failures are explicit instead of silently sampling a large log.

Bounded analysis retains at most 1,000,000 selected samples, 4,000,000 numeric
cells, 32MiB text and 100,000 parameters, with a 1GiB input cap. The caps are
not claims that every maximum-size combination has been benchmarked. Blank
ASCII lines and a known incomplete ordinary EOF tail produce explicit warnings;
unknown framing, truncated metadata and conflicting schemas fail. Both input
and SHA-verification reads reject growth. No active writer, filesystem locking
or general source-authentication guarantee is implied by path/metadata/hash
checks; use stopped logs. Legacy mode detection and wider real-log coverage
remain compatibility gates. Qt messages use deterministic invariant numeric
formatting; MP10 can localize current-culture numeric text. Actual .NET10 casts
saturate integers and map NaN to zero; custom numeric formats use decimal
half-away rounding, separately from standard P1 percentage formatting/grouping.

## Verification and remaining gates

The coordinating Qt build and focused tests pass. Actual MP10 .NET model
comparison matches all81 scenarios and1377 status/message results exactly;
all17 real-BIN check results also match exactly. GPX point/time comparison
matches1210 real points through actual ReadTrack as described above. These are
functional/numeric checks, not proof that the17 heuristics diagnose every
modern firmware/vehicle correctly.

Qt5/audio configure and build4 pass. Full **304/304** passes in52.38s;
GPX/analyzer/controller/spectrogram suites each pass ten repeats (3.59s).
Actual production X11 has zero audit failures, including the real Review parser,
export consent/cancel/existing-file refusal, all17 report lines, organization
plan/cancel/execute, source preservation and MainWindow close/drain. Root inspected
1280/1120-width DATA and consent/report/organization screenshots. Some existing
header/tab labels elide at minimum width; reference/HiDPI polish remains open.
No network SITL or physical vehicle was modified. The organizer test deleted
only its disposable empty-log fixture, as explicitly confirmed in the test plan.

Three Codex streams and Claude TCP c317/c319/c321/c322 cross-reviews contributed.
Full reports and byte counts/SHA256 were checked. Final c322 report SHA256:
`8672e23b7f0489500c326f93e973ad8738a3b5c50305e709fe76d743698c4a19`
(6547 bytes, including c324's acknowledged timezone correction).
Its same-platform historical-zone equivalence claim was rejected using the
actual west-zone oracle above. Controller's full native-X11 suite also passes
(12 cases including fixtures). Initial failed build/test logs are preserved.
Evidence directory: `/tmp/apm-dataflash-tools.PEZxmw/`.
Reusable primary .NET oracles and the Qt CLI probe are under `tests/oracles/`
and `tests/DataFlashToolsProbe.cpp`. The model fixture contains81 cases, each
evaluated by all17 actual reference checks (1377 results). Actual .NET numeric
probing confirms saturating .NET10 unchecked integer conversion and the
different custom-format versus standard-percent rounding rules.

Remaining whole-port gates include the unavailable Geo Reference Images action,
Telemetry Logs replay/presentation, Signing transitions, native Windows/macOS
filesystem/UI evidence, HiDPI/reference screenshot comparison and the broader
SETUP/Settings inventory. This tab does not close the Tools port. Next are
GeoRef, Telemetry Logs and other non-swarm dialogs/Signing. Settings follows the
remaining single-vehicle tools; Swarm remains last.
