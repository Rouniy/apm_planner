# Organize Log Directory

This offline Developer action follows MP10's folder-selection and log-sorting
workflow, with an additional read-only analysis and exact-file consent boundary.
It never connects to a vehicle or changes its parameters. Tests select only
temporary fixture directories, not user logs or the network SITL recording tree.

## Reference contract

MP10 `ConfigDeveloperToolsViewModel.OrganizeLogsAsync` calls
`Services/LogOrganizer.Organize`, then `ExtLibs/Utilities/LogSort.SortLogs`.
Candidates are recursive `.tlog`, `.rlog`, `.bin` and `.log` files, including
already-organized folders. There is no date component despite the reference UI's
wording. Intended output is `root/[SITL/]MAV_TYPE/SYSID/[BRD_SERIAL_NUM/]`.
Zero-byte candidate logs are deleted; nonempty files of at most1024 bytes go to
`SMALL`; telemetry without usable heartbeat evidence goes to `BAD`.

The reference moves every sibling whose filename begins with the candidate's
stem, not only known sidecar extensions. Qt retains literal prefix matching and
lists every resulting file before execution, including unrelated-looking prefix
matches. Wildcard characters in filenames are not interpreted as patterns.
Nonempty same-stem formats use a stable primary (TLOG, RLOG, BIN, LOG), disclosed
in the plan, and their whole listed companion group follows that classification.
Different overlapping candidate prefixes and same-stem groups mixing empty and
nonempty candidates are left wholly untouched with warnings; independent groups
can still proceed. Destination collisions are explicit errors, never overwrites.
Empty deletion applies only to explicitly listed zero-byte candidate files;
nonempty files are never deletion operations.

`FlightLogClassifier` is read-only and streams timestamped TLOG, raw MAVLink RLOG
and binary/text DataFlash through the existing bounded readers. DataFlash uses
the first SYSID/serial parameters, the first100 MSG records and actual SIM
records. Telemetry uses MP10's101-heartbeat boundary, comp190 exclusion and
last-primary selection when several endpoints exist. Serial evidence belongs
to the selected exact sender. Warnings disclose skipped/truncated bytes and
heuristic classification; telemetry identities are not authenticated.

Intentional reference bug fixes:

- DataFlash type detection checks message contents instead of counting a
  sequence of booleans, which made MP10 treat every log with MSG as a rover.
  HEXA and OCTA/OCTO frame names are interpreted as actual frame text.
- Integral DataFlash numeric values such as `1.0` are accepted without brittle
  integer-string formatting; missing or invalid identity remains explicit.
- A serial number is a real directory component. MP10's integer-plus-char
  expression instead added47 or92 to the number and prefixed it to filenames.
  Existing mangled filenames are not guessed or silently repaired.
- PARAM_VALUE names are NUL-terminated correctly and serial values are not
  borrowed from another telemetry endpoint. Known MP10 MAV_TYPE folder names
  extend through GRIPPER48 without requiring a wire-dialect replacement.

## Plan, consent and execution

`FlightLogOrganizer::Analyze` produces an opaque immutable plan and performs no
file mutations. It enumerates exact source/destination/byte-count operations,
records full-content hashes and metadata and checks path containment, snapshots
and destination collisions. Candidate counts are distinct from planned entries.
Traversal, warnings and companion matches are bounded at20,000 entries; oversized
trees fail explicitly. Existing organized destinations are no-ops. Symlinks,
filesystem roots, the complete home directory and aliased root ancestors are
refused or skipped with a warning, never traversed outside the selected root.
Malformed unclassifiable logs can be left with an explicit warning while other
valid candidates are planned. No date-sorting or complete-flight interpretation
is claimed.

The existing `OrganizeLogDirectoryButton` opens named asynchronous directory,
plan and progress dialogs. The plan tree shows every move and permanent empty-
file deletion; Cancel is the default and Escape action.
The root is shown above the tree; relative paths keep filenames readable and
absolute paths remain in item data/tooltips. The Bytes column no longer consumes
unused stretch width. Root inspected the actual final X11 screenshot at
`organizer-plan-final.png`; initial `organizer-plan.png` exposed the elision bug.
Public programmatic
analysis still ends at this consent boundary. Neither analysis nor a cancelled
plan starts execution. The worker captures only immutable data and shared atomic
state; close/destruction requests cancellation. Admission is shared with GPS,
Split, DashWare, APJ, MAVFTP and vehicle operations.

Execution validates the complete plan before its first mutation and checks each
entry again before moving or deleting it. Moves do not overwrite destinations.
Only listed files that are still empty may be deleted. Progress and the final
log report exact completed operations, failures and remaining entries; this is
not a group-atomic transaction or automatic rollback. Already-applied operations
remain applied when cancellation or a later failure stops the run.
Newly created destination directories can remain after a later failure or
cancellation; completed counts refer to listed file operations. Known distinct
storage volumes are refused to avoid QFile's cross-volume copy/delete fallback.
Unknown storage metadata and subvolume/filesystem behavior are not proof of
universal per-file atomicity.

Stop recording and close other log writers before organizing. Active writers
are not detected or locked. Hash/path checks cannot prevent an external process
from racing the final filesystem call. No hostile-process exclusion, universal
filesystem atomicity, undo or power-loss durability guarantee is made.
Closing a cached page requests cancellation and retains the terminal partial
report for reopening. Destroying the page also requests cancellation, but its
cached report is not a durable journal across page destruction or application
exit. Application-owned job history/recovery remains a separate functional gate.
Analysis and execution currently make multiple full hash passes over sources;
large-log IO optimization must not weaken the content checks. Progress is on a
consistent per-candidate scale; final snapshot verification can extend that phase.

## Verification

Qt5/audio configure/build, final246/246 CTest (30.12s) and final production X11
exit0/zero audit failures pass. Focused9/9 (8.66s) passed before the last symmetric
Move guard, which is covered by the final full suite. A deterministic callback
test failed on the pre-fix empty-file deletion, then passes after fresh metadata
checks; both final DeleteEmpty and Move callback regressions pass independently.
Qt5 directory-option order, immediate progress visibility, immutable Plan lifetime,
completion preallocation and a consistent analysis progress scale are also covered.

Dedicated classifier, organizer and widget suites cover fixture classification,
exact plans, unchanged analysis/Cancel, collisions, source changes, symlinks,
cancellation and partial outcomes. The production runtime drives the actual
Tools page, directory picker, visible plan and Execute button using temporary
small/empty/companion files, then continues the previous Developer workflows.
Independent fixture scripts and evidence: `/tmp/apm-log-organizer.MLu9xS/`.

The independent nine-file fixture uses pymavlink-generated raw/timestamped
Plane telemetry (SYSID42, serial1234), a copied real5,275,648-byte ArduPilot BIN
independently identified as SITL/QUADROTOR/0, a BAD raw log, SMALL log, prefix
companions, one empty log and an unrelated file. Analysis leaves all nine files
unchanged. Execution applies eight planned operations (seven moves and one empty
deletion); all nonempty bytes and the original reference log SHA-256 remain
unchanged. Re-analysis is a no-op. No user logs are organized during verification.

Reference visual geometry, high-DPI, native Windows/macOS, active-writer
coordination and representative external filesystems remain explicit gates.
Claude TCP c208–c210 provided independent reference and filesystem reviews;
three Codex streams implement classification, plan/execution and UI/tests,
with root-owned integration, production runtime and independent verification.
