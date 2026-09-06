# APM Planner 3.0 current state and handoff

Updated: 2026-09-06. This is the short operational handoff for the Mission Planner 10 Qt/CMake port. Update it whenever a functional package is committed or the immediate priority changes.

## Goal and current milestone

The product goal is a cross-platform APM Planner 3.0 (`3.0.0`) that transfers the functionality and recognizable workflow of Mission Planner 10 to Qt/CMake on Linux, Windows and macOS. Existing QGroundControl and Hermes/GTU code may be reused where licensing and architecture fit.

The current milestone is **broadly usable functionality**, not final pixel parity. Small spacing, color, label and geometry differences must be recorded and deferred instead of consuming the main implementation stream. Missing or inert actions are functional gaps and remain high priority.

The complete prioritized route from the current checkpoint to functional and
release parity is maintained in `MASTER_PORTING_BACKLOG.md`. The primary stream
now ports the principal SETUP/TOOLS plugins and fills them with end-to-end
functionality. Qt Widgets and the trusted QML extension API are both available;
minor visual matching is recorded and deferred until the useful workflows exist.

The immediate user-directed order is:

1. Keep the fixed SETUP Advanced/Developer Tools action rebinding green, including MAVLink Inspector and every already implemented shared tool.
2. Port the remaining non-swarm TOOLS dialogs/workflows for one vehicle; empty placeholders, no-op actions and accidentally disabled implemented forms are functional defects.
3. Then return to Settings/CONFIG. The route catalogue is 15/15 with concrete factories, while the nine-section Planner page has only 21/64 direct MP10 controls and Advanced Tools has 14/16 intended working actions.
4. Keep the Swarm family at the end of the active queue. Preserve its tested exact foundations without expanding them while single-vehicle workflows remain missing.
5. Retain audited useful older APM Planner modules with honest Legacy/partial labels, track functional and GUI inaccuracies separately, and keep committing complete slices.

## Verified checkpoint

Latest slice (2026-09-06): **Restore Parameters (Recovery)** and its owned
**Cancel Parameter Restore** action. Actual asynchronous file selection and
default/Escape-Cancel consent show the exact file, target and entry count.
The application-owned service freezes parsed source order and vehicle lifetime,
prefetches names, applies ENABLE entries first, then restores all entries in
source order. Changed `_ID` parameters are reset to zero immediately before
their desired value. The existing 15 file exclusions and duplicate ordering
are retained; no additional identity/serial parameters are silently excluded.

Fresh typed reads, exact target/instance and disarmed checks, dual command/
parameter reservations and per-attempt gates protect the sequence. Desired IDs
must fit the endpoint's actual encoding before zero-reset. Confirmed ENABLE,
reset and final writes have separate receipts; cancel, target loss and lost
acknowledgement stop later work without pretending to undo earlier writes.
Page closure cancels only its own operation; service reports survive reopening.
The 30-minute deadline is checked by monotonic clock as well as timer. Sources
are buffered with a 1MiB limit, at most10000 entries, and validated MAVLink names.

The shared ParameterService now preserves the transmission-attempt flag across
synchronous writer/notification cancellation, deletion and replacement. Early
admission IDs, immutable Plan lifetime, nested callback ownership and receipts
after queued ACK/cancel are regression-tested. Claude TCP c222–c227 independently
reviewed reference semantics and code and found a foreign-terminal queue-cap
defect: exact correlation before buffering now prevents six foreign reports
from displacing the real nested reply. The real production audit also exposed
GUI text layout after QApplication teardown; the service-destruction callback
now clears ownership without touching the GUI during shutdown.

Qt5/audio configure and final build pass; final **249/249 (33.24s)** and production
X11 exit0/zero audit failures pass. The actual Tools route exercises both Cancel
boundaries, dynamic ENABLE-first recovery, exact ENABLE1 → GAIN12.5 →
COMPASS_DEV_ID0 → COMPASS_DEV_ID202, four receipts and cancellation after an
attempted unacknowledged write with no retry. Source files remain unchanged.
Root inspected the readable consent, target and buttons in
`/tmp/apm-parameter-recovery.N3ZG4M/recovery-consent.png`; build/test/X11/GDB
evidence is in that directory. Initial failures included a misplaced UI local,
two asynchronous widget fixture assumptions and an immediate-before-pump test
assertion; these were corrected without weakening production checks.
No network SITL or real vehicle was modified.

Developer is now **20/32 working,12 unavailable**. SETUP46/eight absent reference
routes, Advanced14 complete+1 partial Signing/disabled Support Proxy, fixedTools24
plus Signing extension and CONFIG15/15 factories/Planner21/64 are unchanged.
See `PARAMETER_RECOVERY_PORT.md`: prefetch latency, main-pass-only progress,
durable history, arbitrary-late-ACK ambiguity, hardware and native/reference
visual evidence remain explicit limits. Next: **Offline MagFit**, including its
shared Developer/Compass window, bounded sphere/ellipsoid analysis and separately
confirmed exact-target apply. Reference has no file-export action; export-only
must not be called full parity. Existing ALGLIB/log readers are reusable; avoid
mixing TLOG senders and preserve the additive offset/radius conventions. Then
remaining single-drone tools and Settings; Swarm stays last. The full port is
still incomplete.

Previous slice (2026-09-06): **Upgrade Bootloader**. The existing Developer action
now uses two named asynchronous default/Escape-Cancel confirmations, both showing
the exact selected target. One immutable plan is revalidated between prompts and
immediately before one COMMAND_LONG42650, confirmation0, parameters
`[0,0,0,0,290876,0,0]`. There is no automatic retry, downloaded bootloader or
implicit reboot. Both ACK inactivity and absolute lifetime are five minutes.
ACCEPTED means flashed or already current; lost ACK/link/target remains uncertain.
Warnings cover bricking, stable power/data for at least five minutes, expected
telemetry pauses and no automatic retry/power-cycle. Unsupported firmware can
reject; a failure after erasure is not a promise that flash was untouched.

The application-owned service retains admitted work and in-memory history after
page closure. Review also fixed post-callback service deletion in command
retirement/link-forget/deadline and both Developer post-reservation validation
branches, plus consent reopening after close-during-prepare. Tests cover those
lifetime paths, both cancellations, first-Yes zero TX, exact one-frame payload,
wrong ACKs, rejection/timeout, armed transitions and target ABA even after fresh
telemetry. An initial widget fixture omitted that fresh generation heartbeat;
the fixture was corrected without weakening production admission.

Qt5/audio configure and final build pass; final **246/246 (30.41s)** and production
X11 exit0/zero audit failures pass. The actual Tools route exercises both new
consents and all prior offline/MAVFTP/six vehicle flows using in-process sys234.
Root inspected `/tmp/apm-bootloader.zYAKC5/bootloader-final-consent.png`: complete
target/warnings and both buttons are readable. Logs are in the same directory.
No network SITL or real board was flashed or sent a mutation. Claude TCP
c218–c221 independently audited reference semantics and code; the full10170-byte
report hash was verified. Three Codex streams handled service, UI and lifecycle
review/fixes; root integrated, reviewed and ran all verification.

Developer is now **18/32 working,14 unavailable**. SETUP46/eight absent reference
routes, Advanced14 complete+1 partial Signing/disabled Support Proxy, fixedTools24
plus Signing extension and CONFIG15/15 factories/Planner21/64 are unchanged.
See `UPGRADE_BOOTLOADER_PORT.md`: physical flash/native/reference-pixel evidence,
durable job history, arbitrary-late-ACK ambiguity and conservative ACK rejection
after a long heartbeat stall remain explicit limits. Next: remaining single-drone
Developer tools, starting with Parameter Recovery and MagFit, then Settings;
Swarm remains last. This is not full-port completion.

Previous slice (2026-09-06): **Organize Log Directory**. The existing Developer
action now opens an asynchronous folder picker, performs read-only analysis,
shows every proposed move/empty-file deletion in a modeless default-Cancel plan,
then runs only the explicitly confirmed immutable plan. TLOG, raw RLOG and
binary/text DataFlash use shared bounded readers. The MP10 SMALL/BAD/SITL/type/
SYSID/serial layout and literal basename-prefix companions are preserved;
the original boolean-count type bug, serial-path integer addition and NUL-padded
serial decoding are corrected. Same-stem formats have an explicit stable primary;
mixed empty/nonempty and overlapping-prefix groups stay untouched with warnings.

Full-content snapshots, path/volume checks and no-overwrite admission protect
planned files. Execution pins its plan across callbacks, checks cancellation and
fresh source metadata before mutations, and reports completed/remaining operations
without claiming group atomicity. A test first reproduced deletion of newly
written contents from a final callback, then passed after the fresh empty-file
check; the symmetric Move regression passes too. Close retains a cached partial
report, but durable history after page destruction/app exit remains open.

Qt5/audio configure/build, focused9/9 (8.66s before the final Move guard), final
**246/246 (30.12s)** including both callback regressions, and final production
X11 exit0/zero audit failures pass. The actual route exercises folder Cancel,
plan Cancel and three exact operations, then all prior APJ/GPS/Split/DashWare/
MAVFTP/browser-consent/six vehicle checks. Root inspected the plan screenshot and
fixed hidden filenames/oversized Bytes: rows now show relative paths under the
visible root, with absolute paths retained in item data/tooltips. Final image:
`/tmp/apm-log-organizer.MLu9xS/organizer-plan-final.png`.
Independent pymavlink/Python evidence validates a copied real5,275,648-byte BIN,
raw/timestamped Plane telemetry, SMALL/BAD, companions and empty deletion: seven
moves plus one deletion, every nonempty SHA-256 preserved, repeat analysis no-op.
No user log directory, network SITL or physical board was modified.

Developer is now **17/32 working,15 unavailable**. Other counts are unchanged:
Advanced14 complete+1 partial Signing and disabled Support Proxy, fixedTools24
plus Signing extension, SETUP46/eight absent reference routes, CONFIG15/15
factories and Planner21/64. Claude TCP c208–c217 reviewed reference semantics,
filesystem policy and fixes; three Codex streams implemented classifier,
planner/executor and UI/tests, with root-owned integration and verification.
See `LOG_DIRECTORY_ORGANIZER.md` for evidence and explicit limits (20k bounds,
multiple full hash passes, unlocked active writers, durable journal, native/GUI).
Next: **Upgrade Bootloader** as a guarded single-command/two-confirmation slice,
then other single-drone tools including parameter recovery and MagFit. Settings
follows Tools; Swarm remains last. No full-port completion claim is made.

Previous slice (2026-09-06): **Embed Defaults in APJ**. The existing Developer
button now opens firmware/defaults file pickers, creates the exact MP10 output
`firmwarePath + new.apj`, and requires path-specific default-Cancel consent to
replace an existing output. A bounded cancellable worker validates JSON/zlib,
the packed defaults region and original unsigned descriptor CRCs, patches the
defaults and recalculates both CRCs. Signed images are refused. Unknown JSON
tokens, unrelated firmware bytes and inactive defaults are preserved; the source
files are never overwritten. No descriptor is also a supported case with an
explicit boot-compatibility warning. No vehicle connection or firmware upload
is involved. Shared admission and close/destruction cover all Developer flows.

Qt5/audio configure/build, focused **7/7 (8.23s)**, full **244/244 (30.22s)** and
production X11 exit0/zero audit failures pass. Real pickers exercise both Cancel
boundaries, successful output and default-Cancel overwrite; prior GPS/Split/
DashWare/MAVFTP/browser-target-consent/six vehicle-action checks remain green.
The first runtime run exposed a harness-only hidden deferred-delete dialog lookup;
the audit now drives visible dialogs and drains completed pickers before reuse.
Independent Python validation checks a copied real CubeOrange image (1,525,504
bytes, defaults capacity8192) and a synthetic unsigned descriptor, preserving
all unrelated binary bytes and JSON values. Signed/cancel/missing-defaults cases
publish nothing. Original reference hashes are unchanged; no network SITL or
physical board was modified. Evidence: `/tmp/apm-apj-defaults.pwtEdH/`;
implementation and explicit limits: `APJ_DEFAULTS_PORT.md`.

Claude TCP c204/c205/c206 supplied independent format/bootloader/code reviews.
Three Codex streams implemented backend/tests, UI/tests and production runtime;
root integrated, reviewed and scheduled all verification. Developer is now
**16/32 working,16 unavailable**. Other counts stay Advanced14 complete+1 partial
Signing and disabled Support Proxy, fixedTools24 plus Signing extension,
SETUP46/eight absent reference routes, CONFIG15/15 factories, Planner21/64.
Next: remaining single-drone/offline Developer tools (including MagFit), then
Settings/CONFIG; Swarm stays last. APJ semantic parameter validation, external-
flash-only defaults, cryptographic provenance, physical bootability, native
platforms and reference visual parity remain explicit gaps, not completion claims.

Previous slice (2026-09-06): **MAVFTP browser target-bound consent and lifecycle**.
The shared SETUP/CONFIG browser now captures its exact target, remote path,
entry type and destination before page-owned asynchronous file/confirmation
dialogs. Upload/Delete show the target and path with default Cancel. Every
operation uses expected-target admission; cached rows/directories carry their
source lease, and target changes retire prompts and clear stale lists. Returning
to the same MAV IDs still requires a fresh listing. Metadata-only updates do not
invalidate it; literal-root operations do not require a prior directory listing.

The browser cancels only its own operation ID, fences deferred refresh to the
original lease and handles close/reopen, page/service destruction and callback
replacement. SETUP's synchronous connection-page recreation is covered by
reacquiring both the browser and Developer page after component changes. Tests
found and fixed a post-prompt disabled-button state and a Qt5 progress-setter
repaint crash when a valueChanged listener deleted the page. Progress is now
published after Qt's setter returns, with lifetime/revision guards.

Qt5/audio configure/build, focused6/6 (8.10s), **243/243 tests (30.20s)** and
production X11 exit0 pass. The real Tools → Developer → SETUP MAVFtp route lists
an in-process file/directory fixture to EOF, exercises default-Cancel and changes
sys234 from component1 to2 during both Upload and Delete consent: zero destructive
FTP requests, cleared/destroyed old page, empty reopened page and explicit Refresh.
The direct127-byte binary download and all previous GPS/Split/DashWare/six vehicle
actions remain green. No network SITL mutation; test fixtures are stopped.
Evidence: `/tmp/apm-mavftp-consent.AgH20W/`; see `MAVFTP_BROWSER_TARGET_CONSENT.md`.
Claude TCP c197/c199/c201–c203 independently reviewed design/code/runtime fixes;
three Codex streams supplied service, browser/tests and production audit work.
Root reviewed, fixed harness/runtime findings and scheduled all verification.

Counts remain Developer15/32 (17 missing), Advanced14 complete+1 partial Signing
and disabled Support Proxy, fixedTools24 plus Signing extension, SETUP46/eight
absent reference routes, CONFIG15/15 factories and Planner21/64 controls. The
outdated Advanced12 count in SETUP-001 was corrected to match source/tests.
Next: **Embed Defaults in APJ**, then other single-drone tools and later Signing
transitions. The independent APJ audit identifies unsigned descriptor CRC
recalculation and signed-image refusal as required safety work; MP10's simple
marker patch does not handle them. Settings follows Tools; Swarm remains last.
MAVFTP64MiB buffering, sequential80-byte reads, GUI-thread browser local IO,
capability gating, exclusive local no-replace creation, native/hardware evidence
and reference screenshot parity remain; download whole-file CRC is not claimed.

Previous slice (2026-09-06): **Download MAVFTP File and owned FTP operations**.
The existing TOOLS/SETUP Developer button now follows MP10's direct workflow:
remote path (default `@SYS/threads.txt`), Save filename, modeless progress and
Cancel. It does not require directory listing or redirect to a different tool.
Developer is now **15/32 working,17 unavailable**. One exact target lease is
pinned before both prompts and rechecked at shared-service admission. A30-second
remote deadline, token-scoped cancellation and atomic worker output cover
timeout, target changes, close/destruction and reentrant callbacks. No parameter
snapshot or disarmed condition is required for this read operation.

Both the new controller and existing full MAVFTP browser use nonzero operation
IDs published before callbacks, scoped progress/results and owned cancellation.
They share the application-owned protocol service; no duplicate downloader or
fixed TOOLS menu item was added. Save may atomically replace one confirmed local
regular file, with path/snapshot checks and no direct-write fallback. The64MiB
buffer, sequential80-byte reads and absence of download whole-file CRC remain
explicit limits, not claims of verified/authenticated remote content.

Qt5/audio configure/build, focused6/6 (7.75s), **243/243 tests (29.82s)** and
production X11 Tools route exit0 pass. Actual path/Save cancellation sends no
FTP frames; the in-process sys234 fixture exercises Reset/Open/two Reads/
Terminate/Reset, rejects wrong sender/destination and compares127 binary bytes
including NUL/high bytes. Existing GPS/Split/DashWare/six vehicle actions remain
green. No network SITL state was changed; runtime fixtures are stopped.
Evidence: `/tmp/apm-mavftp-download.bQ9IqO/`; see `MAVFTP_DEVELOPER_DOWNLOAD.md`.
Claude TCP c214/c196 cross-review and three Codex streams covered service,
controller and runtime; root scheduled all builds/tests and reviewed integration.

Next: **fix the older MAVFTP browser's prompt-time target-switch consent gap**,
then remaining single-drone Developer tools (APJ defaults/MagFit candidates),
later Signing transitions. The browser's new operation ownership does not by
itself pin consent before its existing modal prompts; CONFIG-006 tracks this.
Settings follows Tools; Swarm stays last. Advanced14 complete+1 partial,
fixedTools24 plus Signing extension, SETUP46/eight absent reference routes and
CONFIG15/15 factories/Planner21/64 controls are unchanged. Native platforms,
physical FTP servers, capability gating and reference screenshot parity remain.

Previous slice (2026-09-06): **Create DashWare CSV**.
The existing TOOLS/SETUP Developer action now exports BIN/text LOG offline
through actual input/types/output dialogs, with MP10 default types
`GPS;ATT;NTUN;CTUN;MODE;BAT` and `<basename>-dashware.csv`. Worker progress,
Cancel, close/destruction and GPS/Split/vehicle interlock are wired. Developer
is now **14/32 working,18 unavailable**, not a complete page.

The bounded raw reader is shared with Split without changing its framing or
publication rules. CSV keeps first-definition column order including MP10's
artificial FMT header slot, sparse original-order rows, trailing empty cells,
wire-format scales, exact integer timestamps and backward clock jumps.
Time priority is TimeMS, TimeUS/1000, T, zero. Binary mode labels use the same
first100001 MSG/PARM prescan and an immutable bundled MP10 mode snapshot.
Quoted strings, explicit NaN/Infinity and no text ERR/EV column spill are
intentional fixes; conflicting schemas, malformed framing and selected binary
A fail explicitly. Known ordinary EOF tails and blank text lines warn.

QSaveFile without direct fallback preserves old output on observed failure or
cancellation. Canonical/symlink guards, two raw passes plus final source hash,
pre-index second-pass schema matching and duplicate-header rejection prevent
ordinary path alias and changing-file corruption. Successful explicit Save
may atomically replace one confirmed regular output; this is not a hostile
filesystem lock or a power-loss durability guarantee.

Qt5/audio configure/build, focused5/5 (7.60s), **242/242 tests (29.85s)** and
two production X11 runs exit0. Real pickers, cancellation, sparse CSV bytes
and all prior GPS/Split/six vehicle paths pass. An independent stdlib reader
compares5809 rows/36 columns from the real5.275MB BIN with raw records, including
scaled floats and timestamps; the40-byte XKF5 tail is explicitly reported.
The105MB/7million-record fixture produces147MB CSV in23.57s at10996KiB max RSS.
Independent streaming comparison confirms all7million rows/four columns;
cancellation at25% exits3 without output. See `DATAFLASH_DASHWARE_CSV.md`.
Network SITL and original logs were not changed;
runtime fixtures are stopped. Evidence: `/tmp/apm-dashware.iZuEbc/`.

Claude TCP c212/c190–c192 review plus three Codex streams supplied independent
reference/code checks, backend/UI/runtime work and raw-file verification.
Root cross-checked disputed mode-prescan/FMT semantics against complete MP10
control flow and scheduled all builds/tests. Next: **Download MAVFTP File**
route to the already implemented exact browser, then remaining single-vehicle
tools (offline APJ defaults/MagFit candidates) and later Signing transitions.
Settings follows Tools; Swarm stays last. Advanced14 complete+1 partial,
fixedTools24 plus Signing extension, SETUP46/eight absent reference routes and
CONFIG15/15 factories/Planner21/64 controls are unchanged. Native platforms,
reference pixel matching and actual DashWare consumer validation remain open.

Previous slice (2026-09-06): **Split DataFlash Log**.
The existing TOOLS/SETUP Developer action now works offline: pick BIN or text
LOG, choose2–1000 pieces (default10), review named default-Cancel consent, then
run with worker progress/cancel. Developer is now **13/32 working,19 unavailable**.
Every complete ordinary record is retained once and in order, with FMT/FMTU/
UNIT/MULT prefixed to each part. Size balancing keeps the final remainder;
reference byte-range truncation, duplicate range metadata and stale output tails
are not reproduced. Text produces actual.log parts and preserves string commas.

All parts are staged in a private sibling directory; existing paths/symlinks
are refused. Three source hash passes and staged verification precede final
publication. Cancellation applies through the last pre-publication gate; each
no-overwrite rename is separate, so late failure retains and reports exact
already-published paths rather than claiming group atomicity. A known ordinary
EOF-truncated binary record is omitted with its type/byte warning, enabling
real finalized logs; unknown framing and truncated metadata remain fatal.
Metadata16MiB/line4MiB/pieces1000 bounds keep memory independent of log length.

Qt5/audio configure/build, focused4/4 (7.46s), **241/241 tests (29.88s)** and
production X11 route exit0 pass. Actual file/count/Cancel/Accept/progress dialogs
and independent two-part binary parsing are covered, followed by the existing
GPS picker and six exact vehicle actions. Real5.275MB ArduPilot BIN splits7
ways, warning about40-byte XKF5 tail; raw hashing verifies129406 data records
byte-exact/in order, and pymavlink reads every part with zero BAD_DATA. A separate
complete-prefix copy also passes. A105MB/7million-record file splits10 ways in
6.78s at10924KiB maximum RSS, with independent byte/order confirmation and
successful no-output cancellation. Originals and network SITL were not changed;
all runtime fixtures are stopped. Evidence: `/tmp/apm-dataflash-split.JoYPE7/`.

Claude TCP c211/c185/c186 review informed EOF-tail/float16/text fixes; three Codex
streams supplied backend, GUI and production tests; root scheduled all builds.
See `DATAFLASH_LOG_SPLIT.md` for exact limits and evidence. Next: **Create
DashWare CSV**, then remaining single-vehicle Developer/TOOLS workflows and
later Signing transitions. Settings follows Tools; Swarm remains last. Advanced
14 complete+1 partial, fixedTools24 plus Signing extension, SETUP46/eight absent
routes and CONFIG15/15 factories/Planner21/64 controls remain unchanged. Native
platforms and reference visual parity are still open.

Previous slice (2026-09-06): **Outbound TLOG recording and privacy filtering**.
The shared production recorder now appends final typed outgoing MAVLink1/2
bytes, including signatures, after successful transport submission. Accepted RX
is captured before negotiation/presentation callbacks. File-session tokens
prevent an old admitted send from entering a callback-selected replacement log.
Open/write errors retire the failed recorder before notifying observers; logging
failure does not turn a successful transport submission into send failure.
SETUP_SIGNING remains absent from generic parser/observer/log paths; raw writes,
rejected ingress and protected diagnostic-only unsigned radio are not logged.

Anon Log now drops GPS_INJECT_DATA, GPS_RTCM_DATA, FILE_TRANSFER_PROTOCOL,
LOG_DATA and SETUP_SIGNING from every sender, even with zero offsets, reporting
aggregate counts without payloads. COMMAND_INT shifts explicitly understood
global coordinates; ambiguous COMMAND_INT/LONG are dropped while audited
non-coordinate commands remain. This is conservative partial filtering, not
complete anonymity: other embedded data, operator coordinates, identifiers,
free text and reference-unlisted fields still require inspection.

Qt5/audio build, focused8/8 (13.17 seconds) and **240/240 tests pass (29.84
seconds)**. The production signing runtime covers exact RX/TX bytes, signatures,
secret absence, rejected traffic, file replacement in three callback locations,
open failure, Linux /dev/full write failure/replacement and synchronous teardown.
Outgoing GPS corrections round-trip through the real extractor. Initial checks
found only harness issues: a local name collision and a test file not selected
because heartbeat discovery had already opened the automatic recorder.

Production transport and Developer Tools audits both pass on X11, exit0; the
latter drives the real file pickers, verifies seven bytes and the updated log
warning, then repeats all six exact fixture vehicle actions. No network SITL
state was changed and no test fixture remains. Evidence:
`/tmp/apm-tlog-tx.JioGvl/` (`build-3.log`, `focused-2.log`, `full.log`,
`signing-x11.log`, `developer-x11.log`, `developer-x11.png`). Claude TCP c210/c179
gave independent REVIEW-OK; three Codex streams implemented or reviewed disjoint
parts. Root scheduled all builds/tests. See `TLOG_RECORDING.md` and
`ANON_LOG_PORT.md` for exact boundaries.

Remaining producer gap: logging retains its heartbeat/enabled start rather than
starting at physical connect. Pre-start traffic and older RX-only logs cannot
be recovered. Submission is not delivery, wall-clock timestamps need not be
monotonic, and buffered QFile writes do not guarantee power-loss durability.
Next candidate is the offline **Split DataFlash Log** workflow, pending final
reference/safety design; continue genuine single-drone Developer/TOOLS actions
and later Signing transitions. Developer12/32, Advanced14 complete+1 partial,
fixedTools24 plus Signing extension, SETUP46/eight absent reference routes and
CONFIG15/15 factories/Planner21/64 controls are unchanged. Settings follows
Tools; Swarm remains last. Native platforms and reference visual parity remain.

Previous slice (2026-09-06): **Extract GPS Corrections**.
The existing Developer Tools action now works offline through the real TOOLS
and SETUP shared page: select a .tlog, choose `<name>-corrections.dat`, extract
on a worker, show progress/cancel and report exact byte/message counts. The
inventory stays32 with **12 working actions and20 unavailable**. All senders,
duplicates and fragments are concatenated in log order, exactly as MP10; there
is no sender filter, RTCM reconstruction or added file header. MAVLink1/2 and
zero-trimmed payloads work. Invalid GPS payload lengths fail; reader CRC skips
and truncated tails produce explicit warnings. An empty result is truthful
success, not fabricated correction data.

QSaveFile with no direct fallback preserves the old destination on observed
cancellation or failure. Canonical input/output guards and resolved directory
aliases prevent ordinary source-file replacement; output symlinks are refused.
The worker captures only paths/shared atomics; GUI progress is polled every100ms.
Close/destruction requests cancellation without retaining a widget in the worker.
File selection/extraction and vehicle actions share the page's operation gate.

Qt5/audio build, focused4/4 and **240/240 tests pass (29.70 seconds)**.
Core tests cover both packet types, all-sender order/duplicates/zero payload,
v1/v2 malformed lengths, CRC resync/truncated tails, empty/noGPS, atomic output,
path aliases and cancellation including the final progress callback. UI tests
cover both picker cancellations, asynchronous results, old-file preservation,
cancel/close/destruction and vehicle interlock. The initial production audit
revealed `QFileDialog::selectFile()` leaving a visible focused editor empty;
the audit now types the exact path and asserts selection, including spaces.
This was a harness correction, not a production extraction workaround.

Production offscreen and X11 audits open the real Tools action, use both file
pickers, compare7 expected bytes from two senders (including MAVLink2 restored
zeros), then rerun the six vehicle actions on isolated in-process sys234.
Both exit0 with no fixture left running. Evidence:
`/tmp/apm-gps-extract.jZo3TF/` (`full.log`, `focused-2.log`, `x11.log`,
`developer-gps-x11.png`). No network SITL was changed. Claude TCP c209/c174 and
three Codex streams supplied implementation, independent review and diagnostics.

At that checkpoint the newly verified producer gap was RX-only Qt logging, whereas MP10
also saves sent packets. Thus those older Qt .tlogs omit this GCS's own transmitted
GPS corrections. The UI explicitly named this; the extractor works on packets
actually present, including MP10 logs. The next slice above adds outbound logging using
exact final wire bytes after successful submission, excluding SETUP_SIGNING
entirely and auditing Anon Log's outbound-coordinate/opaque-RTCM drop policy.
Then continue remaining single-vehicle Developer/Tools workflows and Signing
transitions; Settings follows Tools and Swarm stays last. Advanced14 complete+1
partial, fixedTools24 plus Signing extension, SETUP46/eight absent routes and
CONFIG15/15 factories/Planner21/64 controls are unchanged. Native platforms and
reference visual parity remain open. See `GPS_CORRECTION_EXTRACTION.md`.

Previous slice (2026-09-05): **Six exact single-vehicle Developer actions**.
TOOLS → Developer Tools and the shared SETUP page now wire Set QNH, Adjust
Barometer Altitude, Force Accel Calibrated, Force Compass Calibrated, Reboot
Vehicle and Reboot to DFU. The exact inventory stays32: **11 working actions,
21 unavailable**, not a completed Developer page. Pressure uses REAL32
GND_ABS_PRESS or the capability-gated BARO1_GND_PRESS fallback and the MP10
11.1 Pa/m formula. These are ground-reference recovery tools, not certified
sea-level QNH or normal sensor calibration. DFU preserves42/24/71/99, not the
different hold-in-bootloader command; absent ACK/link loss remains uncertain.

One application-owned service captures target generation, physical session,
vehicle instance, fresh disarmed ArduPilot heartbeat and complete parameter
records before any default-Cancel prompt. It revalidates before each possible
write. Pressure holds both command and parameter lanes; active/uncertain
compass calibration blocks admission. Command reservations have a separate
single-vehicle route policy, with no broadening of Swarm routes. Closing the
page cancels consent, but not an admitted application's terminal waiter.

Qt5/audio build and **239/239 tests pass (29.59 seconds)**; focused7/7 pass.
New coverage includes wire payloads, REAL32 refusal, stale/armed/target-ABA and
parameter/capability snapshots, callback reentrancy, retry gates, lane ownership,
Cancel/Yes, page close/reopen, synchronous replies and shutdown. Initial checks
found test compile mistakes, stale text assertions and a runtime fixture racing
the existing6-second PARAM traffic fence; these were corrected without reducing
the production fence. Claude TCP c168/c170 and three Codex streams reviewed or
implemented independent parts; root scheduled all builds/tests.

The actual production Tools route passes both offscreen and X11 with isolated
in-process sys234: all six Cancel paths emit no change, confirmed actions emit
two exact pressure writes and four exact commands, with matched echo/ACK results
and clean fixture removal/shutdown (exit0). It opens no telemetry socket and
never changes the network SITL. The fixture's DFU ACK is synthetic route evidence,
not proof of physical DFU support. Evidence: `/tmp/apm-developer-vehicle.Nwtyz6/`
(`full.log`, `focused-3.log`, `x11.log`, `developer-x11.png`). Native platforms,
physical recovery actions and reference visual parity remain unverified.

Next useful offline slice: Extract GPS Corrections, with TlogReader, bounded
asynchronous extraction and atomic output; reference semantics are concatenated
GPS_INJECT_DATA/GPS_RTCM_DATA payloads, not RTCM fragment reassembly. Signing
rekey/disable/reconciliation also remains open. Settings/CONFIG follows Tools;
Swarm remains last. Advanced14 complete+1 partial, fixedTools24 plus Signing
extension, SETUP46/eight absent reference routes and CONFIG15/15 factories with
Planner21/64 controls are unchanged. See `DEVELOPER_VEHICLE_TOOLS.md`.

Previous slice (2026-09-05): **Guarded initial MAVLink Signing provisioning**.
TOOLS and SETUP Advanced share `MAVLink Signing — Keys and initial setup…`.
The working partial tool now sends one SETUP_SIGNING to an operator-known
unprovisioned, fresh disarmed exact ArduPilot target on a private dedicated
route. Default-Cancel consent names cleartext exposure, all-channel effects,
possible hidden prior keys and the absence of ACK/retry/recovery. Required-key
fingerprint and an unconfirmed marker persist before any possible write;
failures never reopen unsigned traffic. Use locally remains offline/no-TX.
Authenticated packets never silently clear the permanent uncertainty warning.
Rekey, disable and operator reconciliation remain unavailable: **Advanced is
still14 complete +1 partial**, not15 complete. The fixed TOOLS inventory stays24
plus the explicit Qt Signing extension. SETUP46 pages/eight absent routes and
CONFIG15/15 factories/Planner21/64 controls are unchanged.

Qt5/audio build and **237/237 tests pass (22.96 seconds)**. New tests cover
consent/delivery mutation, stale/armed/duplicate/radio/signed-target refusal,
exact signed secret submission without observers, uncertain writer removal,
strict persisted pending state and production-theme warning resize/scroll.
The rediscovery regression exposed legacy lifetime bugs: detach all vehicles
before destroying a shared link, retire registry/companion objects immediately,
guard deferred callbacks, detach retired protocol consumers, clear old pointers
in AP2DataPlot2D/UASRawStatusView/QGCMapWidget, and resolve the decoder target
before field zero with guarded nested-call state. These fixes preserve the
useful modules. Additional lazy HDDisplay/HSIDisplay/HUD lifetime risks remain
explicit follow-ups, not covered by this default-route claim.

An independent isolated ArduCopter sys233 fixture on **SERIAL1 TCP61980**, not
USB/COMM_0, was provisioned once through the actual Qt window. Pymavlink verifies
980 signatures: an unsigned read-only version request is ignored, a signed one
answered. Restarting that fixture without wiping storage repeats both checks
with its retained key. The GUI TLOG has27,090 valid frames, including5,402
independently verified signatures, and no SETUP_SIGNING/raw key/BAD_DATA.
Evidence: `/tmp/apm-signing-initial.g6VlP9/`. No network SITL key was changed.
Final-build X11 restores the locked pending profile after app restart, unlocks
the vault and uses the matching key locally without another provision. Reconnect
receives thousands of authenticated packets while keeping the full warning
readable at720-wide and560x520. The production signing runtime also exits0 on X11.
Close/reopen preserves the warning and unlocked vault; shutdown with that window
open exits0. All test windows and the isolated SITL are stopped.
Claude TCP c205/c206/c163 and three Codex streams supplied independent review,
implementation and production tests; root scheduled all builds/tests.

Next: reviewed Signing rekey/disable/reconciliation, then the remaining genuine
single-vehicle Developer/TOOLS workflows. Claude's c207 inventory recommends
exact-target Developer vehicle actions, Offline MagFit and Photo GeoRef; these
are planning candidates, not implemented claims. Settings/CONFIG follows Tools;
Swarm remains last. See `MAVLINK_SIGNING_PORT.md` for limitations and evidence.

Previous slice (2026-09-05): **Modeless local Signing key manager**.
TOOLS now includes an explicit `MAVLink Signing — Local keys` extension, and
SETUP Advanced opens the same modeless observer. Fourteen full Advanced
workflows remain; Signing adds **one working local-only partial workflow**,
while Support Proxy remains unavailable. The fixed MP10 top-level inventory
stays24; the new local-keys entry is a clearly named Qt extension.

The window creates/unlocks/locks the application-owned encrypted vault, adds
and deletes named keys, and selects a key for an already-provisioned OFFLINE
physical connection. First protection and deletion require default-Cancel
confirmation. Inputs are masked and cleared, seed visibility is explicit and
reset, names/status are non-secret metadata, and no vehicle command is sent.
Provider and activation callbacks revalidate physical object/id/profile plus a
runtime edit/connect/disconnect revision. Close cancels pending local activation,
not the application-owned vault; locking it does not revoke transport keys.
The worker also supports bounded fingerprint lookup without exporting every key
to the GUI; the current window deliberately uses explicit named-key selection.

Qt5/audio build and **237/237 tests pass (18.67 seconds)**; focused tests5/5.
The production runtime creates a fresh vault through the real window, adds a
fixture key, exercises Cancel/Accept on an offline UDP-client profile, verifies
persisted protection with epoch0/no TX, locks and reopens. The same full route
audit passes X11. Manual X11 verifies Tools opening, masked create/add,
named default-Cancel warning, unchanged policy after Cancel, close/reopen with
the same unlocked vault and application shutdown with the window open (exit0).
Evidence: `/tmp/apm-signing-ui.VUTMZC/`. No test window remains and no network
SITL key was changed. The initial4/5 focused run exposed the audit's missing
DeferredDelete drain after Cancel, now fixed; production widget tests already
passed. Claude TCP c203/c204 and three Codex agents contributed independent
review, widget/backend work and production-route coverage.

Next: exact fresh-disarmed no-ACK provisioning and reviewed key-change/disable
transitions, then isolated non-COMM_0 signed SITL and native-platform gates.
Do not count Signing as a complete Advanced workflow yet. Settings/CONFIG
remains next after single-vehicle tools; Swarm remains last.

Previous slice (2026-09-05): **Persisted locked Signing profiles and async vault**.
Manual UUID/startup-UDP identities and strict fingerprint/required-hint metadata
now restore before any factory connection. Missing/corrupt policies, duplicate
profiles and manual/startup port collisions stay blocked, including direct
transport reconnect, RX/TX/raw writes. Offline activation requires the matching
key; restart/re-add never implicitly borrows a retained key context. Uncertain
policy-publication failure also blocks the current process.

The application-owned vault service runs one bounded operation in a dedicated
worker, keeps KDF/file work off the GUI thread and delivers keys only through
explicit callbacks after token return. Nested-loop, callback-deletion, foreign
thread and shutdown cases are covered. Qt5/audio build and **236/236 tests pass
(18.61 seconds)**; focused signing/profile/vault tests are 7/7. Real X11 runs the
same production transport and settings-restoration audit with exit0. Evidence:
`/tmp/apm-signing-profiles.jrT00E/`. Claude TCP c201/c202 and independent Codex
reviews informed the pre-connect gates and async admission fixes.

Ordinary network SITL on UDP14550 also passes real X11 smoke: Quick shows
0.00 A/current and about 0.01 m/Home, and grows/shrinks at 1120x720 -> 1600x950
-> 1120x720. Independent TLOG parsing finds one HOME_POSITION, 625 positions,
418 each SYS_STATUS/BATTERY_STATUS with current0 and zero BAD_DATA. The smoke
application exited0; no test window remains and no vehicle signing key was
changed. Screenshots, logs and isolated configuration are in the same evidence
directory. The ordinary serial profile saved a stable UUID and required=false.

This is infrastructure, not a completed Signing tool: **Advanced remains 14/16**.
Next: modeless key manager/local selection, then exact fresh-disarmed no-ACK
provisioning and isolated signed SITL verification. Vault lock does not revoke
transport keys. There is no supported policy removal/rekey/reset UI. A sticky
global settings error blocks new connections for this process; any quarantined
profile prevents rewriting settings/activating another key until repair/restart.
Full Settings/CONFIG remains next; Swarm remains last.

Previous slice (2026-09-05): **Quick resize and missing Home acquisition**.
Adaptive description/number fonts now fit actual cell geometry, override the
production 11px stylesheet and grow/shrink with window/splitter/layout changes.
The user-reported DistToHome dash came from no explicit Home request on late
join. Fresh exact heartbeat+position now trigger REQUEST_MESSAGE(242) at bounded
0/5/10-second attempts then 30-second backoff, fenced by target/source/physical
epochs and the single-vehicle route. Only a real HOME_POSITION supplies Home;
unknown or stale distance is not substituted with zero.

Qt5/audio build and **234/234 tests pass (57.18 seconds)**. Synthetic UDP proves
11.12 m request/response, stale dash/recovery and exit0. Actual network SITL
192.168.0.43:14556 -> UDP14550 system/component1/1 was passively captured: 977
packets/10 seconds, fresh coordinates, current0 in SYS_STATUS/BATTERY_STATUS,
but no HOME_POSITION. Final X11 now displays 0.00 A and about0.01 m and visibly
rescales at 1120x720 -> 1600x950 -> 1120x720. TLOG snapshot has one Home reply,
314 global positions, 210 each SYS/BATTERY_STATUS and no BAD_DATA. Evidence:
`/tmp/apm-quick-home.NlMkar/`. The final live-SITL window was left open for the
user (PID2149325, exec74070, isolated live-config/live-appdata); do not confuse it
with synthetic UDP14700/system234, which is stopped. Counts and port priorities
remain unchanged. Pre-4.0 Home request fallback, raw DisplayText names, generic
read-command ACK banners and other catalog/native-platform gaps remain explicit.

Previous slice (2026-09-05): **MAVLink Signing production transport integration**.
The application-owned key-domain manager shares bounded replay contexts across
aliases, links and reconnects, with persistent stable profile IDs. Exact and
legacy sends share one finalizer/signing boundary; protected links refuse raw
write-back and MAVLink 1 downgrade. Original ingress bytes are authenticated
before discovery, services, logs and mirror fan-out. Unsigned radio is diagnostics
only. SETUP_SIGNING secrets are suppressed from observers/decoder/replay and
redacted from CSV/text export; TCP/UDP debug traces are metadata-only.

The full Qt5/audio build and 234-test suite pass, including an isolated test of
the actual LinkManager/Protocol/transmitter with two synthetic physical links,
cross-link/reconnect replay checks, real signed TX and no rejected-frame fan-out.
Real X11 verifies the same protected runtime gate plus ordinary UDP14699 traffic:
source233 appears, the independent peer parses GCS commands/heartbeats and the
TLOG contains 221 valid source233 heartbeats and no BAD_DATA. Normal app and peer
exit 0. Evidence: `/tmp/apm-signing-transport.j50T7t/`. The initial 233/234 exposed
an outdated Inspector audit checksum expectation; it now checks exact submitted
bytes against the final wire frame. Codex review also added a 256-context lifetime
cap without eviction, and Claude TCP c198/c199/c146 reviewed the final contracts.

At that transport-only checkpoint, the operator dialog and persisted policy
were still missing; the later checkpoints above supersede those gaps.
Ordinary unconfigured links remain unprotected and a signature alone is not
proof of authentication. SETUP remains **46 pages / eight absent reference
routes**, CONFIG **15/15 factories, Planner 21/64 controls**. Vehicle provisioning,
key-change/disable and non-COMM_0 SITL/native-platform release gates remain open;
the local-only Signing window is not a claim that those transitions are complete.
RX restart replay window, power-loss durability and historical binary-log secret
sanitization remain explicit limits in `MAVLINK_SIGNING_PORT.md`.

Previous slice (2026-09-05): **MAVLink Signing protocol/key-store foundation**
and a production signed-frame CRC fix. The new standalone library provides
exact-byte signing/verification, shared-key bounded replay state, a locked
process-crash-safe timestamp allocator and OpenSSL AES-GCM/PBKDF2 named-key
storage. Independent pymavlink vectors and adversarial/atomicity tests pass.
The old frame helper could turn BAD_CRC into OK after a signed trailer; the
application parser now rejects that frame without advancing replay/clock state.

Full Qt5/audio build and **232/232 tests pass (55.63 seconds)**. Real X11 with
isolated UDP14698 rejects forty bad-CRC/valid-MAC heartbeats (no target appears),
then accepts valid signed frames and creates source232. The resulting TLOG has
124 valid source232 heartbeats and zero BAD_DATA under independent pymavlink
parsing. Application exit is 0 while the valid peer remains active. Evidence:
`/tmp/apm-signing.krlrz9/`. Claude TCP c196/c197 reviews and three bounded Codex
streams are recorded in `MAVLINK_SIGNING_PORT.md` / `MAV_AUTH_KEY_STORE.md`.

At that earlier checkpoint, signing was not wired into transport or UI. Advanced
Tools remains **14/16**, SETUP **46 pages / eight absent reference routes** and
CONFIG **15/15 factories, Planner 21/64 controls**. A signature present on live
traffic still does not imply authentication. Next: application-owned same-key
context coalescing and stable link IDs, exact/legacy outbound funnels and raw
write gate, authenticated ingress/secret suppression, then modeless key manager
and exact-target no-ACK provisioning with non-COMM_0 SITL evidence. Persistent
RX restart protection, guaranteed power-loss durability and native-platform
crypto/packaging remain explicit gaps. Do not enable vehicle-side signing first.

Previous slice (2026-09-05): native **Warning Manager** with its real DATA Quick
coloring consumer. All IF/AND editor columns, repeat/tokens, atomic compatible
warnings.xml, single application engine/modeless observer, exact-target fresh
telemetry, bounded shared speech and a ten-second red HUD message are wired.
The catalog maps **394/486 numeric/boolean reference properties**, with the
remaining **92 names and producer/unit differences explicitly inventoried**.
The six-default native Quick grid uses MP10 colors, field/layout settings and
contrast rules; the useful old selector remains Quick (Legacy). Advanced Tools
is **14/16**. No direct SETUP/CONFIG route or top-level TOOLS item was added.

Full Qt5/audio build and **229/229 tests pass (16.61 seconds)**.
Initial 228/229 exposed a late static LinkManager destruction repaint after
QApplication; GDB identified it and the source now suppresses shutdown signals.
Real X11 verifies live yellow Quick color/red HUD warning, finite threshold
editing/save/reopen, stale-state reset, physical disconnect/reconnect recovery,
non-silent output-monitor speech capture and normal final-build exit 0 with
the sender still active. Evidence: `/tmp/apm-warning.2r8mcV/`. Contracts, deliberate reference
bug fixes and gaps: `WARNING_MANAGER_PORT.md`, `WARNING_TELEMETRY_CATALOG.md`.
Claude reviewed over TCP (c194/c195), with three disjoint implementation streams
and root-owned builds/tests. Signing transport/verification remains before
vehicle-side enable; upstream-placeholder Support Proxy remains unavailable.

Previous slice (2026-09-05): native **Advanced Tools Anon Log** for BIN, text LOG
and MAVLink TLOG. One application-owned single-thread worker and a reusable
760x740 modeless window now provide file selection, independent offsets,
default-Cancel privacy/overwrite confirmation, progress, token cancellation and
truthful counters/warnings. Source aliases cannot become destinations, source
content is hashed before/after processing, and QSaveFile publishes only a
successful complete result. This is coordinate obfuscation, not full sanitization.
Advanced Tools is **13/16**; no new Setup page or top-level TOOLS item is added.

Full Qt5/audio/Concurrent build and **225/225 tests** pass (16.70 seconds).
Both real ArduPilot SITL BINs (90,112 and 5,275,648 bytes) process successfully;
their known incomplete non-coordinate tails are preserved with warnings.
Independent pymavlink checks verify 1,211 GPS records shifted by +0.25/-0.25
degrees while altitude, speed, time and status remain unchanged. A final-build
105,000,089-byte synthetic BIN run processes 14,000,000 coordinate values in
5.16 seconds with 11,776 KiB maximum RSS. Real X11 verifies restored offline
Advanced 13/16, BIN/LOG/TLOG publication, default Cancel, usable result geometry,
close/reopen during processing, explicit cancellation preserving the prior
destination SHA-256, and normal application exit 0 with another job active.
Evidence: `/tmp/apm-anonlog.I5u4vQ/`; contracts, reproducible probe and remaining
limits: `ANON_LOG_PORT.md`.

Claude TCP reviews c190-c193 and three disjoint Codex streams caught and fixed
signed-I coordinate semantics, real incomplete tails, ambiguous Alt inference,
FMTU unit/multiplier redefinitions, unannotated RGPJ/RBCH/AIS coordinates,
partially zero-trimmed MAVLink 2 scalar handling and out-of-coverage counters.
The final independent review is REVIEW-OK. General BIN multiplier application,
LOG's exact Lat/Lng-only coverage, additional TLOG coordinate messages,
representative real TLOG/text logs and native/reference visuals remain explicit
gaps. Signing still requires complete transport signing/verification before its
vehicle-side enable action may become available.

Previous slice (2026-09-05): native **PX4Flow Setup**. The offline Optional Hardware
page now displays bounded RAW8U grayscale frames and performs exact typed
VIDEO_ONLY reads/writes through the central parameter service. Its explicit
generic-component selector is independent of the active autopilot. Physical and
component-instance epochs, one pre-UAS parameter ingress, shared endpoint
arbitration and application-owned bounded cleanup prevent stale responses or
page/source changes from redirecting a write. Focus has a default-Cancel bench
warning; replay is read-only. No Swarm feature expansion was made.

SETUP now registers **46 pages: 45 mapped MP10 routes plus QML Plugins**, against
53 reference pages and the same three groups (49 Qt navigation entries).
Eight reference routes remain absent; connection filtering is counted separately.
Full Qt5/audio build and **221/221 tests** pass (16.70 seconds). Real X11 with
an isolated UDP sensor simulator verifies 64x64 normal and 376x240 focus frames,
exact VIDEO_ONLY=1/0 ACKs, missing-packet stale diagnostics and recovery,
restoration to zero after leaving the page, disconnect/reconnect without implicit
sensor retargeting, and normal application exit 0. The peer also exits 0 in mode
zero. Evidence: `/tmp/apm-px4flow.HrHy0E/`; contracts and limits: `PX4FLOW_PORT.md`.
Claude reviewed through TCP 4096 (c186-c189, final REVIEW-OK after fixes), with
three disjoint Codex work streams and root-owned builds/tests. Physical PX4Flow,
real replay-file, reference visual comparison and Windows/macOS checks remain.

Previous slice (2026-09-05): native **Advanced Tools Param gen** and the real
Setup visibility matrix. A modeless application-owned generator now parses the
nine master/stable source roots and their libraries and force-refreshes all
nine PDEF products. Atomic per-file publication, token cancellation, bounded
network/parser resources and truthful partial results are wired. Both metadata
repository instances reload lazily while existing parameter pages retain their
snapshots and edits. Advanced Tools is **12/16**; no new Setup route is counted.

Full Qt5/audio build and **216/216 tests** pass (16.54 seconds). The manual
official-network probe exits 0 with all ten artifacts, including the 8,665,849-byte
generated-source catalog (SHA256
`62e4b7a54be95f590eeab44be256094eee339e1bc7f790d37ccb3283ae6acdc7`).
Real X11 verifies the offline Advanced button, exact default-Cancel confirmation,
successful 10/10 publication, close/reopen while downloading, explicit Cancel
preserving the old generated file hash, and normal application exit 0 during
another active download with its observer open. Evidence:
`/tmp/apm-paramgen.h8Bphz/`; contracts and remaining gaps: `PARAM_GEN_PORT.md`.
Claude reviewed the parser/fallback through TCP 4096 (c183/c184/c185, final
REVIEW-OK after fixes), with three disjoint Codex work streams and root-owned
builds/tests. Real input testing additionally corrected the obsolete MP10 stable
Rover URL to APMrover2-stable and handles SITL library-only/Helicopter products.

The current metadata trust flag is catalog-wide: adding any unversioned fallback
range makes the merged catalog advisory, including otherwise exact ranges.
Per-field range provenance, safe idle-page hot refresh, Heli/Blimp active-family
selection, Local override/legacy XML interchange and native-platform evidence
remain explicit follow-ups, not claimed parity.

Previous slice (2026-09-05): native **FFT Setup / Advanced Tools FFT**. Setup
now registers **45 pages: 44 mapped MP10 routes plus QML Plugins**, against
53 MP10 pages and the same three groups; nine explicit routes remain missing.
FFT opens both as a real Setup page and an independent 1000×700 modeless
window. Three metadata parameters, true bitmask editors, all-six-IMU batch
spectra, IMU fallback, bins/start/magnitude, bounded cancellable analysis and
notch suggestion are wired. Full Qt5/audio build and **213/213 tests** pass.
X11 verifies both routes, six curves, synthetic UDP CNT 1024→1056 and MASK
1→3 write ACKs, all three refresh reads, and normal exit 0 with FFT open.
Evidence: `/tmp/apm-fft.T1Aeqm/`; implementation/limits: `FFT_PORT.md`.

Live testing caught two defects beyond the initial green unit suite. A separate
single-vehicle exact parameter reservation policy now supports ordinary
serial/TCP/UDP-client and one learned UDP peer, with target generation,
instance/link epoch and peer-revision checks; the stricter Swarm policy is
unchanged. A pre-existing firmware-page hide callback no longer dereferences
a destroyed MainWindow header during shutdown. Both have production runtime
regressions. Claude independently reviewed FFT math/parser and the final
single-vehicle policy through TCP 4096 (c180/c181/c182, final REVIEW-OK);
Codex agents had disjoint edit leases and root owned builds/tests.

User clarification: Setup entries disappearing only while disconnected and
returning on reconnect matches MP10's `RequiresConnection` filtering; pages
were not removed. The full factory inventory is independent of visibility.
The production route audit now checks actual offline Advanced/Basic/Custom
visibility, independent Bluetooth gating and an isolated no-write Plane
connect/disconnect cycle before forcibly exposing factories. Real X11 confirms
the offline Advanced surface. Other vehicle/profile/partial-parameter matrices
and physical disconnect tests remain separate gates.

Previous single-vehicle slice (2026-09-05): native **Download Logs (MAVLink)**
replaces the production legacy dialog route. Refresh, Selected/All downloads,
missing-range repair, Cancel, guarded Erase and actual streaming Create KML are
wired through one application-owned exact-link service. File replacement is
atomic; list identities cannot cross vehicles or picker changes. UDP ingress and
queued output now carry peer revisions, peer changes retire the old physical
epoch, and queued lifecycle callbacks cannot dereference a removed link. The
new production runtime audit found and verifies that older removal crash.
Full Qt 5 build and **206/206 tests** pass. Real X11 verifies the TOOLS click,
two-entry list, Selected/All SHA-256 equality after intentionally missing packets,
400/4000-point KML files, cancellation preserving an existing destination,
independent busy window, two erase requests to a synthetic peer only, empty
refresh and normal application exit 0 with the tool open. Evidence:
`/tmp/apm-download-logs.FmmrKU/`; details and remaining limits:
`DOWNLOAD_LOGS_PORT.md`. Earlier 200-test checkpoints below remain historical.

Next single-vehicle candidate, not Swarm: MAVLink Signing
(persist fail-closed protected profiles, wire vault unlock/key selection and
implement guarded no-ACK provisioning plus the modeless dialog); Support Proxy
is also a placeholder in MP10 itself. Settings/CONFIG follows these working
single-vehicle surfaces. The eight absent Setup routes remain explicitly tracked.

The pre-Wave-1 functional checkpoint was `fb4f08b5` (`feat: port MinimOSD telemetry helper`), following `4290221f` (`feat: add antenna tracker output protocols`) and `e6fab89e` (`feat: port ESP8266 setup workflow`). The later verified checkpoints include `9d69cebb` (PLAN wrapper geometry), `9b180eba` (waypoint row actions), `e6d9e75b` (independent Inspector windows), `fbeacd6e` (Antenna Tracker pages), `e7d5e58b` (their parity-ledger checkpoint), `ed0d3706` (exact Waypoint Leader foundations), `a3594d95` (bounded Waypoint Leader profile control), `b1a12a39` (hardened exact swarm command dispatch), `5ad9d0d9` (exact swarm parameter transactions), `93c652b5` (complete disabled Waypoint Leader window shell) and `938db3bc` (fresh exact mission-observation lifecycle).

At this checkpoint:

- Test Speech is verified through the real Qt engine and the production CONFIG button. The previous binary was compiled without TextToSpeech: runtime speechd was installed, but `libqt5texttospeech5-dev` was missing and the old CMake default allowed a silent build. The development package is now installed; CMake requires Qt audio by default and this build explicitly uses `APM_REQUIRE_QT_AUDIO=ON`. Silent builds require an explicit opt-out and cannot count as speech evidence. Diagnostic states distinguish not compiled, missing engine, runtime error, queue unavailable and ready/busy; Planner refreshes the backend status asynchronously. The manual `speech_backend_probe` reports speechd ready and 306 voices, speaks the reference phrase and returns to idle with exit 0. An actual X11 Test Speech click produces a captured output-monitor signal (mean -16.6 dB) and the application exits normally; screenshots/WAV evidence are in `/tmp/apm-graph-speech-setup.irm6Qx/`. Full build and **200/200 tests** pass. This supersedes only the earlier silent-build audio limitation, not the remaining MP10 speech controls or native-platform checks.
- Historical pre-FFT checkpoint: SETUP registered **44 pages: 43 mapped MP10 routes plus QML Plugins**, compared with 53 MP10 pages and the same three collapsible groups. Ten routes were then absent: Install Firmware Legacy, Secure, Secure (Bootloader Keys), CubeID Update, NV Modem, PX4Flow, Antenna Tracker parameters, FFT Setup, Onboard Lua REPL and Local Script REPL. The older audit already recorded the eleven-gap baseline; the previous runtime check only validated our own 43-route list and did not enforce that reference inventory. The new independent 53-page manifest and explicit missing-route allowlist prevent silent omissions. Joystick now appears in Optional Hardware and launches the existing shared modeless editor; this is useful legacy reuse, not full MP10 control parity. Late action registration is covered, and extra Serial Ports/Initial Parameters/Motor Test visibility restrictions were removed without relaxing their write gates. Modern Heli keeps the validated H_SW_TYPE correction. Group arrows now toggle `<<` expanded / `>>` collapsed as requested; current MP10 itself has a static prefix. Real X11 verifies both arrow states and the actual Joystick dialog (no physical device), with normal application exit 0; **200/200 tests** pass. Evidence and independent reference inventory are in `SETUP_INVENTORY_AUDIT.md`, `SETUP_REFERENCE_INVENTORY.tsv` and `/tmp/apm-graph-speech-setup.irm6Qx/`.
- The native `MAVLinkInspectorView` now includes Show GCS Traffic and Graph It alongside the exact-source tree, Pause/Clear/filter and bounded Hz/Bps cache. Finalized typed submissions are observed exactly once with physical-link epochs captured before writes and revalidated afterward; arbitrary raw passthrough is excluded. Negotiation requests initialize all fields and use the local GCS source and remote target. Numeric scalar/array graphs retain 10..100000 history points (default 500), render at most 2000 extrema-preserving points per series and refresh every 100 ms. Every modeless graph owns its original link/replay pin, survives Inspector close and observes both directions independently of parent Pause/checkbox. Replay disables the separate outbound checkbox. Full build and **200/200 tests** pass, including production parser/transmitter/graph-lifetime coverage. X11 verifies outgoing COMMAND_LONG, live sine telemetry, the history prompt and continued graph updates on UDP 15550 after Inspector close and a header switch to UDP 15551; application shutdown with the graph open exits 0. Evidence: `/tmp/apm-graph-speech-setup.irm6Qx/inspector-gcs.png`, `graph-history.png`, `graph-live.png`, `graph-after-inspector-close.png` and `header-after.png`. Modern dialect, real replay-file and native-platform evidence remain; old inspector/relay sources are compiled but no longer the production UI.
- The focused `apmplanner3`, exact mission, exact command ACK, Swarm telemetry/rate and Waypoint Leader core builds completed successfully after the required standalone compiler preflights; concurrency stayed capped at `-j12`.
- The complete test suite passes with the Tools catalogue, Link Statistics, Tlog Convert / Extract, MAVLink Mirror, NMEA Output, CoT/TAK, Device Operations, DataFlash Spectrogram, 3D Terrain View, External Guided, Follow Me, Moving Base, RF Propagation settings/core/exact-target telemetry/DATA+PLAN overlays, OSD Video local decode/tlog timeline/MJPEG writer/window, the exact multi-link Swarm telemetry registry, offline Sequence editor, functional Copter/Rover Formation, Follow Path and Follow Leader slices, the production Waypoint Leader and Sequence adapters/windows/executors, central exact COMMAND_ACK arbiter, exact parameter transactions and exact mission snapshot cache, CONFIG Onboard OSD, DisplayView profiles, Planner startup UDP, native HUD/speech/Message Severity Planner controls and exact-target periodic/high-message speech source/queue, the native Planner Settings page, shared CONFIG/SETUP MAVFTP, both Heli Setup schemas, both current/legacy Frame Type pages, the native current Compass parameter/onboard/fixed-yaw workflow, the dedicated Compass/Motor route, shared native Flight Modes, Plane QP Extended Tuning, the production SETUP factory audit and the native Inspector store/source/view/replay audits: **200/200 tests**.
- Real X11 smoke verifies the exact 24-entry MP10 TOOLS order without late HIL/custom/panel actions, independent Link Statistics, Tlog Convert, MAVLink Mirror, NMEA Output, CoT/TAK, Device Operations, DataFlash Spectrogram, 3D Terrain View, External Guided, Follow Me and Moving Base windows from the main surface, modeless Inspector, Map Tile Cache, Plugin Manager and Log Download windows, Developer Tools navigation, and clean application exit with tool windows open. The Spectrogram run opens the production window through Ctrl+L, verifies the non-empty 1050×820 X/Y/Z surface and records `/tmp/apm-spectrogram-smoke.png`. The Terrain run opens the production entry through `TOOLS`, receives a live exact `SYS 1:1` target, renders a 33×33 elevation mesh with telemetry and hover coordinates, opens a second independent instance and closes both bounded windows; evidence is `/tmp/apm-terrain-smoke.fxoUiI/terrain-hover.png` and `/tmp/apm-terrain-smoke.fxoUiI/terrain-second.png`. The External Guided run opens the enabled TOOLS route and non-empty 620×390 window, verifies the default-Cancel warning, receives Accepted from a live local ArduCopter SITL, rereads an edited file on the next cycle and shows explicit Busy in a second independent window; evidence is `/tmp/apm-external-guided-smoke.VQu32f/tools.png`, `/tmp/apm-external-guided-smoke.VQu32f/external-guided.png`, `/tmp/apm-external-guided-smoke.VQu32f/confirmation.png`, `/tmp/apm-external-guided-smoke.VQu32f/running.png`, `/tmp/apm-external-guided-smoke.VQu32f/running-updated.png` and `/tmp/apm-external-guided-smoke.VQu32f/second-busy.png`. The Follow Me run clicks the enabled production TOOLS row, renders a non-empty 480×420 client window and exits with status 0; evidence is `/tmp/apm-followme-final-smoke.eT2MTm/tools.png` and `/tmp/apm-followme-final-smoke.eT2MTm/follow-me.png`. The Moving Base run clicks the enabled production TOOLS row, renders the non-empty 580×500 Serial/TCP/UDP/status surface and exits with status 0; evidence is `/tmp/apm-movingbase-smoke-live/tools.png` and `/tmp/apm-movingbase-smoke-live/moving-base.png`. The Mirror smoke used a live UDP heartbeat source, received 64 framed bytes through TCP Host, observed Tx accounting and Listening after client disconnect, then opened a second independent window and exited with status 0. A separate write-back run sent the exact `WRITEBACK_MARKER_4096` bytes from the TCP peer through the pinned UDP vehicle link, showed Rx 21 with the checkbox enabled and exited cleanly. The NMEA smoke received 100 TCP Host sentences from a live local MAVLink source, verified the first GGA/GLL/HDG/VTG/RMC cycle and its five checksums, opened a second independent window and exited with status 0. The CoT smoke opened the enabled menu route on a live UDP vehicle, selected TCP Host, delivered the same parseable CoT 2.0 event to two simultaneous clients, opened a second independent window and exited cleanly with the first session active. The Device Operations smoke opened the 760×520 window through TOOLS, opened a second independent instance through Ctrl+J, then opened a third through SETUP → Developer Tools and exited with status 0.
- A saved `SETUP → Advanced Tools` or `Developer Tools` page no longer freezes implemented buttons in a disabled state when it is restored before the TOOLS QAction catalogue exists. `SetupView::applicationToolActionsReady()` recreates only those two lazy pages after menu registration and restores the selected one. The production route audit reproduces page-before-actions startup and verifies all ten Advanced shared actions plus Device Operations, 3D Terrain and compiled OSD Video. Real X11 startup with `setup_lastpage=Advanced Tools` shows `10 of 16`, opens MAVLink Inspector from the restored enabled button and exits cleanly; evidence is `/tmp/apm-setup-advanced-restore.15wRuL/advanced-restored.png` and `/tmp/apm-setup-advanced-restore.15wRuL/inspector-from-restored-advanced.png`.
- `TOOLS → Connection Options` now opens the current MP10 settings surface instead of the unrelated old add-link form: fixed 340×230 modeless geometry, the exact eight default baud choices, GCS system id, heartbeat switch, staged Save/Close and visible status. The useful legacy creator was preserved honestly as `Add Connection` behind the header `+` button with its existing serial/TCP/UDP/UDPCl/WS workflow. Default baud remains a true new-connection default and is no longer overwritten when an existing serial link saves its per-port rate; heartbeat applies to current and future UAS instances and synchronizes the open Planner/legacy settings checkboxes. GCS identity is persisted but explicitly applies after restart because the existing exact services cannot yet change every ACK-correlated sender identity as one transaction; the legacy settings setter follows the same policy and shutdown/global directory saves no longer clobber the pending value. Focused tests cover defaults, legacy fallback, malformed profiles, staging, save/close, multi-window lifetime, runtime bridging, shutdown persistence, real SerialConnection per-port/default separation and the retained add-link route. Full build and 194/194 tests pass. Real X11 evidence is `/tmp/apm-connection-options-final.U7FaWC/connection-options-window.png`, `/tmp/apm-connection-options-final.U7FaWC/connection-options-saved.png` and `/tmp/apm-connection-options-final.U7FaWC/add-connection-window.png`. The follow-up `/tmp/apm-connection-options-exit.DR1A6u/restarted-settings.png` and `saved-settings.png` verifies saved values, changing GCS id 23 to 24, and normal application exit 0 with the dialog open; the pending id remains 24 after shutdown.
- The RF Propagation production smoke opens the enabled main-window shortcut into a complete 480×700 settings surface, opens a second independent modeless instance and exits cleanly with both windows present. Evidence is `/tmp/apm-rf-propagation-final-smoke.96e5GV/rf-settings.png` and `/tmp/apm-rf-propagation-final-smoke.96e5GV/two-rf-settings.png`; representative live terrain/vehicle overlay evidence remains.
- The OSD Video production smoke clicks its enabled bottom TOOLS entry with no external GStreamer sink override, opens the complete 1120×720 modeless source/tlog/offset/preview/output surface and keeps the application alive. Evidence is `/tmp/apm-osd-production-smoke.khi1Ik/result.png`. Synthetic Qt Multimedia decode and production-writer decode pass; representative real camera/tlog export, reference comparison and native-platform evidence remain.
- The Formation production smoke clicks the enabled `Swarm Formation (Beta)` TOOLS row, opens a non-empty 1180×720 modeless window with its safety banner, leader/capture controls, formation canvas, ten-column vehicle table and explicitly locked unported command controls, then closes the tool and application independently with status 0. Evidence is `/tmp/apm-formation-smoke.2wThX7/tools.png` and `/tmp/apm-formation-smoke.2wThX7/formation.png`; live multi-vehicle target delivery, reference screenshot comparison and native-platform evidence remain.
- The Follow Path production smoke clicks the enabled `Swarm Follow Path (Beta)` TOOLS row, opens a non-empty 1180×680 modeless window with its safety banner, leader/separation controls, populated vehicle table, enabled Start and visibly disabled bulk controls, then closes the tool independently and the application with status 0. A listening UDP leader is reported truthfully as route-locked. Evidence is `/tmp/apm-followpath-smoke.dKgC0r/tools-key-follow.png`, `/tmp/apm-followpath-smoke.dKgC0r/follow-path.png` and `/tmp/apm-followpath-smoke.dKgC0r/after-tool-close.png`; live multi-vehicle target delivery, reference screenshot comparison and native-platform evidence remain.
- The earlier Follow Leader production smoke verified its non-empty 1240×720 modeless window and, at that checkpoint, recorded Waypoint Leader as the sole disabled catalogue entry. The Follow Leader action still opens the danger banner, ground/air selectors, four settings, nine-column table, enabled Start and six visible disabled bulk commands, closes independently and leaves the main application alive. Evidence is `/tmp/apm-followleader-smoke.haHTY5/tools-follow-leader.png`, `/tmp/apm-followleader-smoke.haHTY5/follow-leader.png`, `/tmp/apm-followleader-smoke.haHTY5/after-tool-close.png` and `/tmp/apm-followleader-smoke.haHTY5/follow-leader.xwininfo`; the newer Waypoint Leader smoke below supersedes only that old disabled-entry observation.
- The Waypoint Leader production route now combines the transport-free MP10 geometry/state core with application-owned exact mission, command, parameter and swarm services. `LinkManager` owns the all-or-nothing executor and cancels it before link/session teardown; the adapter preserves full physical-link/session/instance identity, converts only complete main-mission snapshots, owns only its matching refresh token and handles synchronous, late and foreign completions fail-closed. The complete 1320×790 modeless window is enabled from TOOLS, hides an old mission until a newer observation revision arrives, disables Start while exact reservations drain or an outcome is uncertain, and keeps the persistent RTL/navigation-parameter change behind a default-Cancel warning. `swarmwaypointleaderwindowadapter_tests`, related service/window tests and the 193-test suite pass. The real-X11 production click opens the non-empty live-adapter surface, focuses one singleton on repeated selection, closes/reopens independently and exits cleanly; evidence is `/tmp/apm-waypointleader-smoke.Ljp8Nh/waypoint-leader.png` and `/tmp/apm-waypointleader-smoke.Ljp8Nh/waypoint-leader-reopened.png`. Live multi-vehicle flight evidence, physical/reference/native-platform checks and an explicit operator acknowledgement path for a terminal `OutcomeUncertain` remain.
- The earlier Swarm Sequence production smoke clicks the TOOLS entry, renders the complete non-empty 1380×820 modeless editor, discovers a live exact ArduCopter, creates `Smoke Layout` and adds its first ordered step. At that checkpoint Run and Takeoff were still visibly disabled. Evidence is `/tmp/apm-sequence-smoke.ddrXnE/tools.png`, `/tmp/apm-sequence-smoke.ddrXnE/sequence.png` and `/tmp/apm-sequence-smoke.ddrXnE/sequence-step.png`. The current slice supersedes that disabled-command state with an application-owned exact executor and live adapter; focused tests and the 194-test suite pass, while replacement X11 command-surface evidence is still pending and is not implied by the earlier screenshots.
- A new real-X11 CONFIG smoke used a local UDP target with two OSD screens and nine committed parameters, clicked the production `Onboard OSD` navigation row, rendered the non-empty canvas plus ALT/BAT item editors and closed the application cleanly. Evidence is `/tmp/apm-osd-smoke.kLNaLd/osd.png` for this workspace run; reference screenshot diff and physical-vehicle writes remain.
- A real-X11 Flight Modes smoke used one isolated exact Copter endpoint with a complete nine-parameter snapshot and live RC5=1500. It selected both production rows independently through SETUP and CONFIG/Settings and verified Current Mode=Auto, numeric Current PWM, all six rows, Save/Refresh/help controls and the green active-row paint before a clean exit. Evidence is `/tmp/apm-flightmodes-smoke.qQEFJI/setup-flight-modes.png` and `/tmp/apm-flightmodes-smoke.qQEFJI/config-flight-modes.png`; physical Copter/Plane/Rover/PX4 writes, reference screenshot diff and native-platform evidence remain.
- A real-X11 Plane CONFIG smoke used an isolated disarmed QuadPlane endpoint with `Q_ENABLE=2` and representative Q controller, RC and INS parameters. It clicked the production `QP Extended Tuning` row, rendered the non-empty 17-group/68-row native surface, exercised its inner scroll through Harmonic Notch and Filter Logs, and exited with status 0. Evidence is `/tmp/apm-qp-extended-smoke.TWFM0D/qp-extended-top.png` and `/tmp/apm-qp-extended-smoke.TWFM0D/qp-extended-bottom.png`; physical writes, reference screenshot diff and native-platform evidence remain.
- Real-X11 CONFIG smokes click the production `Planner` row and render the native scrollable Planner Settings page instead of the old generic widget. The original evidence `/tmp/apm-planner-settings-smoke.pSKCFA/planner-lower.png` covers the lower sections; the updated live-control run `/tmp/apm-planner-live-smoke.3anzjo/planner-after.png` toggles HUD off and Speech on, reports the intentionally missing developer-build speech engine, persists the exact `CHK_hudshow=false` and `speechenable=true` keys, switches to DATA without restarting and leaves its HUD host visibly blank in `/tmp/apm-planner-live-smoke.3anzjo/data-hud-disabled.png`, then exits with status 0. All nine section/order and bidirectional service checks are deterministic in `configplannerview_tests` and `hudcontrol_tests`.
- Planner speech has two real-X11 follow-ups. The first opens the original five Speech event controls only after `Enable Speech`, completes the production Mode and Battery prompts and verifies their exact persisted values (`/tmp/apm-planner-speech-smoke.PE5xfb/planner-after.png`). The current run shows all eight MP10 speech sub-controls in reference order, persists `speechenable=true` and exits cleanly (`/tmp/apm-planner-speech-c2.Jz91ZI/planner-eight-controls.png`). Event gates, transactional cancellation, unit conversion, cadence, exact-link filtering, No Data and bounded queue behavior are deterministic in `speechsettings_tests`, `speechannouncer_tests`, `speechtelemetrysource_tests`, `queuedspeechcontroller_tests`, `configplannerview_tests` and `batterymonitor_tests`.
- The current Message Severity X11 run drives one real production UDP endpoint, opens CONFIG -> Planner with persisted `severity=6`, verifies the visible `Message Severity = Info` control, switches through the production DATA action and observes `STATUS ERROR FROM EXACT LINK` on the native HUD before a clean exit. Evidence is `/tmp/apm-status-smoke.Ucj2NV/planner-severity.png` and `/tmp/apm-status-smoke.Ucj2NV/data-high-message.png`; the deterministic settings, chunking, normalization, target-epoch, speech and color cases remain covered by the full suite.
- A real-X11 route smoke with a connected local UDP target clicked the production `MAVFtp` row in both CONFIG and SETUP and rendered the same non-empty native tree/table/toolbar page from each shell. Evidence is `/tmp/apm-mavftp-smoke.al49Kd/config-mavftp.png` and `/tmp/apm-mavftp-smoke.al49Kd/setup-mavftp.png`; live MAVFTP-server transactions remain separate hardware/SITL evidence.
- A real-X11 CONFIG smoke used an isolated MAVLink-v1 helicopter target on UDP 15550 with the exact legacy `H_SWASH_TYPE` capability, clicked the production `Heli Setup` row and rendered the non-empty swash/manual-control, curve and live-servo page. Evidence is `/tmp/apm-heli-smoke.vPSoO8/heli-setup.png`; the application exited cleanly, while physical legacy-heli writes remain separate evidence.
- A separate real-X11 SETUP smoke used an isolated current helicopter target with `H_SW_TYPE`, clicked the production `Heli Setup (4.0+)` row and rendered hydrated servo/swashplate controls on the non-empty five-section page. Evidence is `/tmp/apm-heli4-smoke.HOEZdm/heli4-setup.png`; the application exited with status 0, while physical current-heli writes remain separate evidence.
- A real-X11 SETUP smoke used an isolated Copter target with `FRAME_CLASS`, `FRAME_TYPE` and `FRAME`, opened both production frame routes and rendered the hydrated `OCTAQUAD / H` preview plus all six legacy choices instead of the old blank combined widget. Evidence is `/tmp/apm-frame-smoke.qhCYvE/frame-current.png` and `/tmp/apm-frame-smoke.qhCYvE/frame-legacy.png`; the application exited with status 0, while physical frame writes remain separate evidence.
- A real-X11 SETUP smoke used an isolated Copter target with legacy compass parameters, clicked the production `Compass (Legacy)` row and rendered its non-empty basic setup surface. Live, Onboard and CompassMot calibration buttons were visibly disabled; clicking Onboard created no top-level window, and the application exited with status 0. Evidence is `/tmp/apm-compass-legacy-smoke.A50E1u/compass-legacy.png`.
- A separate real-X11 SETUP smoke used an isolated Copter target with current compass priority/device parameters and a scripted MAVLink peer. The production `Compass` row rendered its non-empty priority table, use/learn controls and calibration surface; Start emitted exact `MAV_CMD_DO_START_MAG_CAL`, the peer returned a correlated ACK plus `MAG_CAL_PROGRESS`, and the UI entered `Calibration running` with Mag 1 at 37%. A separate lost-ACK run entered the explicit fail-closed `OutcomeUncertain` state. Evidence is `/tmp/apm-compass-smoke-current4/compass-ready2.png` and `/tmp/apm-compass-smoke-current4/compass-running.png`; physical compass motion, reference screenshot diff and native-platform evidence remain.
- A real-X11 SETUP smoke used a dedicated TCP Copter peer, clicked the production `Compass/Motor Calib` row and rendered the non-empty ready, default-No confirmation, live 37.5%/12.50 A/27% XYZ plot and proven-success states. The peer observed the exact param6=1 start followed by two Finish ACK frames. A separate run killed the same physical TCP session while active and rendered the critical unconfirmed-stop warning before clean application exit. Evidence is `/tmp/apm-compassmot-smoke.OMIs2D` and `/tmp/apm-compassmot-smoke.FdD2Hd`; propeller-free physical validation and stored-parameter reconciliation remain.
- The DATA HUD AOA/SSA overlay no longer leaks its opaque black brush into the following speed/altitude tape outlines. Those side tapes retain the same translucent fill with AOA off or on; `hudcontrol_tests` compares the complete left tape pixel region between both render paths.
- The recovered PLAN row actions, explicit multi-instance MAVLink Inspector lifecycle, Antenna Tracker stack, corrected Tools-menu/window slices through Follow Leader and the non-visible Waypoint Leader exact foundations are verified. Any later working-tree changes must be reviewed and checkpointed as their own coherent slice.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 129 product rows: 66 `in-progress`, 37 `partial`, 26 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. The duplicate legacy-Heli inventory row has been removed. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

## Implemented foundations worth reusing

These are working foundations, though their parity rows may remain partial because visual, hardware or cross-platform evidence is outstanding:

- Mission Planner-like main shell and deterministic DATA/PLAN `QSplitter` layouts without KDDockWidgets.
- User-selectable compiled map backends through a common abstraction and one canonical filesystem tile cache.
- DATA HUD/map/telemetry surface and PLAN mission table/map/action surface.
- Typed Mission, Fence and Rally stores; mission file/transfer paths; QGC Plan and legacy fence/rally file handling.
- PLAN mission editing, context commands, store-aware undo, route geometry, polygon drawing/offset/fence conversion, Survey/Corridor import, terrain source, elevation graph and distance measurement.
- Exact `(link, system, component)` vehicle target registry, shared link transmitter, command service, parameter service and committed parameter store.
- Application-owned Swarm telemetry registry with independent exact `(link, system, component, link-session, instance)` leases, a 24-autopilot bound, heartbeat-only freshness, all-or-nothing group validation and atomic position/velocity/attitude/landed-state/MISSION_CURRENT/NAV_CONTROLLER_OUTPUT snapshots. Physical reconnect/rebind, stale heartbeat and meaningful `time_boot_ms` rollback retire old instances before reuse; parser and service epochs are pinned across every reentrant callback so an old byte batch cannot enter a new session.
- A singleton modeless Swarm Sequence editor with MP10-compatible bounded JSON, atomic save, ordered steps, common slot resizing, draggable East/North preview, bounded session background and exact-instance ArduCopter assignment. Its application-owned exact executor prepares immutable plans, repeats revision/anchor/assignment validation after default-Cancel confirmation and after reservations, pins physical-link/session/vehicle-instance identities, reserves `SwarmCommandService` then `VehicleCommandService` and drains them in reverse order. Run Step requires every commanded Copter already in exact GUIDED, requests the position stream at 10 Hz and queues zero-velocity global position targets; Takeoff serializes GUIDED ACK, newer heartbeat, ARM ACK, newer heartbeat and TAKEOFF 2 m ACK for each assigned vehicle. Known rejects continue to later vehicles, while `PartialEffect` and `OutcomeUncertain` block restart; closing the owning window cancels only its matching operation.
- An application-owned, single-session `SwarmCommandService` that prevalidates exact groups, field ages, required exact ArduPilot flight mode and routes, repeats the member/mode barrier after every injected route callback, sorts slots deterministically, limits batches by logical deadlines with a 2 ms scheduler-jitter allowance and stops on synchronous cancellation or partial transport failure. Formation uses it for all-or-nothing Copter/Rover position-and-velocity targets; Follow Path uses its position-only stream request and clean position target while checking route eligibility before confirmation. Plane-specific control and yaw/gimbal remain visibly locked.
- A central `VehicleCommandService` exact endpoint reservation and COMMAND_ACK arbiter. It supports parallel commands only on distinct exact endpoints, correlates link/session/source/target/command, caps all IN_PROGRESS extensions at one absolute operation lifetime, quarantines late ambiguous ACKs and drains safely on owner or vehicle retirement.
- `ParameterService` exact group reservations/read/write operations for swarm orchestration. Exact work is pinned to immutable endpoint leases, runtime parameter type and value; uses an absolute deadline; copies validators across reentrant callbacks; fences conflicting legacy traffic; and quarantines an endpoint/name after uncertain writes so a late clamped or type-changed echo cannot satisfy another operation.
- An application-owned `ExactMissionSnapshotService` that serializes each UAS mission protocol through `MissionProtocolCoordinator`, downloads MISSION/FENCE/RALLY on one exact route, publishes only complete generation/digest-bound immutable snapshots and invalidates them on reconnect or known upload/clear ownership. MAVLink 1 extension mission types fail at every route/send barrier.
- A transport-free `SwarmWaypointLeaderCore` with bounded MP10 path/signature geometry, fail-closed relative-altitude mission-frame validation, exact mission and group binding, staged heartbeat-confirmed GUIDED/ARM/TAKEOFF, line/V/interleaved target generation, return/RTL landing and fail-closed collision/batch handling. Its complete non-empty window shell exists, while the UI action remains disabled pending the application-owned executor and production adapter.
- Backstage SETUP/CONFIG infrastructure, the phase-one Onboard OSD layout editor and a substantial set of hardware/parameter pages listed in the parity ledger.
- A native Plane `QP Extended Tuning` page with all 17 MP10 groups and 68 stable rows, Plane-first Q aliases, metadata numeric/enum/bitmask editors, per-field unavailable state, full Roll-to-Pitch lock behavior and the more-than-double confirmation. It pins one exact target/component, gates Q rows through the complete `Q_ENABLE` state while retaining common RC/INS controls, blocks writes while armed or heartbeat-stale and reconciles partial ordered batches before editing resumes. Copter/Heli retain their useful legacy Extended editor under the same stable route id.
- A native Planner Settings page with the exact nine-section MP10 topology on the canonical CONFIG route, live unit/profile/runtime controls, shared live HUD-overlay and speech-policy services, all eight MP10 event gates/templates, exact-target periodic/No Data policy, a bounded serialized TTS queue, a truthful Test Speech status, dual startup UDP policy and an explicitly bounded Legacy settings dialog. CONFIG is now the sole native Planner route: the orphaned standalone action and duplicate top-level dialog have been removed to match MP10.
- A native legacy CONFIG Heli Setup page with the exact 43 logical parameter rows, swash/manual controls, MP10 curve math and live RC/servo visualization; dangerous manual writes are disarmed, confirmation-gated, exact-target and exact-batch correlated. A conservative uncertainty latch survives lost ACKs/raw echoes until an exact successful `H_SV_MAN=0` batch, with best-effort deactivation cleanup.
- A separate native SETUP `Heli Setup (4.0+)` page with all five MP10 sections and 83 exact bindings: eight fixed servo rows, current swashplate, rotor-speed, governor and miscellaneous parameters. Missing firmware fields stay visible and disabled, metadata drives current enums/ranges with safe current-heli enum fallbacks, non-zero `H_SV_MAN` is disarmed and confirmation-gated, and writes complete only through their immutable exact-target batch. A conservative manual-override latch survives raw echoes/lost ACKs, deactivation requests an exact zero, and only the successful zero batch clears the latch.
- Separate native SETUP `Frame Type` and `Frame Type (Legacy)` pages in MP10 order. The current page carries all 14 frame classes, the exact dependent subtype table, preview and Refresh; the retained legacy page carries all six `FRAME` choices including V-Tail. Both render a complete unavailable state before any parameter/version callback, hydrate without writes, block frame changes while armed and use immutable exact-target result-aware batches. A partial two-parameter result clears the optimistic geometry, disables edits and requires a complete refreshed snapshot before edits resume.
- The native current SETUP `Compass` page mirrors MP10's 11-column priority/device table, missing-device discovery, exact aliases and physical-slot mapping, Use 1/2/3, Learn, nine advanced fields, declination and Pixhawk defaults. It always constructs a non-empty disabled surface before a complete snapshot, writes only while disarmed through one immutable exact-target batch, treats the three priority parameters as one zero-filled transaction, requires full refresh after any partial terminal outcome and clears reboot-required only on the exact accepted command ACK. Its application-owned calibration service pins link/system/component/generation, ACK-gates Start/Accept/Cancel/fixed-yaw, waits for the complete multi-compass mask, never accepts failed sensors, retains reboot requirements per exact endpoint, releases stale UI ownership on a fresh target generation and treats long/stalled onboard timers as warnings rather than cancelling vehicle state. The same service now owns the separate `Compass/Motor Calib` route: it serializes all compass operations, sends the exact param6 start and two-frame Finish ACK, exposes bounded exact-session status/plot/log data, and issues fail-closed motor stops on deactivation, watchdog, target change, physical disconnect and shutdown. Start requires a disarmed ArduCopter multirotor, a complete snapshot, one autopilot on an operator-confirmed dedicated ordered Serial/TCP link and a default-No propeller warning; listening UDP, UDP client, simulation and unknown transports are rejected. MAVLink 1 point-to-point links remain supported with zero ACK target fields and terminal firmware text, while MAVLink 2 target extensions are used only as phase evidence, never as shared-link isolation. An uncertain outcome poisons ordinary compass work on that endpoint until the operator proves power removal; a proven terminal outcome permits onboard/fixed-yaw work but prevents another CompassMot run in the same physical-link epoch. `Calibrate from Log` remains visibly disabled because the separate OfflineMagFit workflow is not ported. The inherited single-compass page remains separately exposed as `Compass (Legacy)` with all unsafe global calibration actions quarantined.
- Shared trusted QML plugin engine/API (`docs/porting/QML_PLUGINS.md`); legacy binary APM Planner plugins are intentionally unsupported.
- Mission Command List editor and catalog, exposed to PLAN and QML.
- The MP10 Default Settings workflow: official frame catalog discovery/download/cache, compare/stage into the native raw-parameter editor, exact target-generation guards and operation-scoped cancellation.
- The MP10 HW ID page: `_ID`/`_DEVID` inventory, ArduPilot device-ID decoding and exact six-column sortable presentation.
- The MP10 ADSB page: metadata-backed `ADSB_`/`AVD_` parameter editing and batching, search, uAvionix flight-ID/registration read/write cadence and exact-target message filtering.
- The MP10 ESP8266 page: exact component-240 parameter loading, bytewise packed settings, 22 serialized writes, storage/reboot/reset commands and a target-safe Qt view lifecycle.
- The MP10 SETUP OSD page: the exact legacy MinimOSD telemetry helper surface and its ordered 24-parameter 2 Hz write batch, with committed-snapshot filtering, partial-vehicle reporting and exact-target transaction lifecycle guards.
- The MP10 Antenna Tracker output foundation: exact Maestro compact commands and ArduTracker/DegreeTracker text protocols with tested trim, reverse, clamp, wrap and tilt-flip arithmetic behind an injectable writer.
- Link-scoped MP10-compatible `RADIO_STATUS`/legacy `RADIO` snapshots and read-driven local/remote SNR filtering, isolated by physical `linkId` and cleared with link removal.
- Pure Antenna Tracker pointing geometry with MP-compatible AZ/EL/distance functions plus explicit validity, dateline and pole handling.
- The thread-confined Antenna Tracker raw-serial service: dedicated Qt SerialPort 8N1 transport, setup/centering pipeline, bounded latest-target-wins backpressure, write watchdog, generation-aware cancellation and fail-closed unplug/reconnect behavior.
- Visible per-row PLAN Up/Down/Delete controls that reuse the existing store-aware operations, including undo and DO_JUMP remapping.
- Explicit modeless multi-instance MAVLink Inspector windows and guarded replay multicast, with independent-close and application-shutdown coverage.
- An exact MP10 top-level TOOLS catalogue: real application tools are no longer mixed with view-dependent DATA/PLAN/SIM docks, unavailable workflows are visibly disabled with a reason, and late vehicle/custom-widget activity cannot append empty panels to the menu.
- A modeless MP10 Link Statistics window that follows the current exact target's physical link without retaining a stale link pointer, plus direct modeless entry points for Plugin Manager and MAVLink Log Download.
- A modeless MP10 Tlog Convert / Extract window with streaming, cancellable and atomic KML, GPX, CSV, text, parameter and complete-mission-snapshot exports; the unsupported Matlab button is disabled with an explicit reason.
- A modeless MP10 MAVLink Mirror window with independent per-window sessions, a complete-frame receive tap, immutable physical-link targeting, bounded Serial/TCP/UDP outputs and explicit live write-back control.
- A modeless MP10 NMEA Output window with independent per-window sessions, exact endpoint targeting, live 1/2/5/10 Hz selection and bounded Serial/TCP/UDP output of checksummed GGA/GLL/HDG/VTG/RMC cycles.
- A modeless MP10 CoT/TAK Output window with all six reference transport modes, preserved six-cell identity JSON, deterministic CoT 2.0 serialization and one event per positioned MAVLink endpoint on the pinned physical link.
- Advanced and Developer action inventories, working shared actions/parsers, Advanced Terminal and the user-facing trusted QML plugin manager.
- Cooperative shutdown ordering, including close with the modeless inspector open, verified by the real-X11 smoke above.

## TOOLS current phase

The header menu now has the exact 24 MP10 items, order, separators and shortcuts. In the verified Qt Multimedia build all 24 routes are enabled and open a modeless window or the specific Developer Tools page, including the production Swarm Waypoint Leader surface. A build without Qt Multimedia enables 23/24 and disables only OSD Video rather than exposing an inert route. The catalogue test synthesizes every compiled handler and proves each action fires exactly once; the Waypoint Leader X11 run proves the real MainWindow handler opens a visible non-empty window. Legacy DATA/PLAN/SIM docks, HIL actions and custom `.qgw` containers cannot enter this menu. Installed trusted QML tools may append one clearly named extension submenu after the reference inventory.

The shared `SwarmTelemetryRegistry` is wired to `LinkManager` as the safety foundation for the Swarm family. Every typed connect begins a new physical-link epoch after clearing parser and exact-service state, every disconnect/removal/shutdown ends only the captured old epoch, and messages are admitted only under the epoch captured before any synchronous callback. Equal sysids on different links remain distinct; reconnects and detected autopilot reboot cannot inherit a prior individual or group lease. Sequence options, anchor and one-to-one assignments retain the full instance lease and immediately follow heartbeat eligibility changes. Formation, Follow Path and Follow Leader consume the same leases through the shared sender. Waypoint Leader binds those leases to exact mission/current-navigation observations, central ACK/parameter owners and its application-owned group executor. Sequence now has a separate application-owned executor that composes the shared Swarm and VehicleCommand reservations without introducing per-window ACK ownership.

For Run Step, the window freezes the selected layout/step, anchor and one-to-one assignments before its default-Cancel prompt, rejects any nested-dialog mutation, and the executor revalidates the same immutable exact instances after acquiring both reservations. The intentional Qt safety tightening requires every target already in exact ArduCopter GUIDED; only then does it request POSITION at 10 Hz and queue one relative-altitude `SET_POSITION_TARGET_GLOBAL_INT` with zero velocity per target. Takeoff uses distinct assigned vehicles in stable system-ID order and advances each through GUIDED ACK → newer GUIDED heartbeat → ARM ACK → newer armed heartbeat → TAKEOFF 2 m ACK. A known rejection skips the remaining stages for that vehicle and continues the report; any possible partial effect or uncertain outcome prevents restart after reverse-order drain. Window close cancels only the generation it owns. Focused tests and the 194-test suite pass; a new production X11 click and live multi-vehicle evidence remain pending, with further Swarm work intentionally moved behind single-vehicle Tools and Settings.

`SwarmFormationWindow` is a singleton 1180×720 modeless window opened by the real TOOLS action. It discovers exact Plane/Copter/Rover instances across links, exposes the reference leader, capture, yaw/gimbal/Plane options, ten-column follower table, draggable East/North canvas and bulk-command row. Copter/Rover followers can be explicitly enabled and receive clean `SET_POSITION_TARGET_GLOBAL_INT` position+velocity targets at 10 Hz after default-Cancel confirmation. Start reserves the complete group, requests POSITION/ATTITUDE first and waits up to three seconds for a fresh leader position, velocity and attitude without sending a target; a missing/reconnected member, changed route or partial batch stops the session. Only TCP and UDP Client are accepted as exact routes, `(0,0)` after wire rounding is rejected, and automatic leader replacement clears offsets and disables followers. Plane attitude/PID, follower yaw/gimbal, Serial/radio, listening UDP and bulk arm/mode/takeoff/land commands stay visible but disabled pending their exact senders and ACK arbitration.

`SwarmFollowPathWindow` is a singleton 1180×680 modeless window opened by its real TOOLS action. It selects one exact Plane/Copter/Rover leader plus explicitly checked, uniquely ordered Copter/Rover followers and records a session-only bounded leader trail. After default-Cancel confirmation it reserves the complete group, requests only POSITION at 5 Hz, waits up to three seconds for fresh positions, withholds the whole batch until `maximum order × separation` exists and then queues clean-mask `SET_POSITION_TARGET_GLOBAL_INT` relative-altitude targets interpolated backwards from the newest trail sample. Followers must already be in the exact family-specific ArduPilot `custom_mode` for GUIDED on every tick; the ambiguous `MAV_MODE_FLAG_GUIDED_ENABLED` bit is never accepted as proof. Route eligibility is shown and rejected before confirmation, while reconnect/retirement, stale telemetry, mode loss even during a route callback, plan edits, a GPS jump over 500 m or partial transport stop and release the session. Plane guided-waypoint/ACK, automatic GUIDED switching and bulk flight commands remain visibly disabled.

`SwarmFollowLeaderWindow` is a singleton 1240×720 modeless window opened by its real TOOLS action. It selects a distinct exact ground master and ArduCopter air master plus up to 21 explicitly checked, contiguous-order ArduCopter followers. The ground master is observed while the complete group is reserved and POSITION streams are requested at 10 Hz; after a default-Cancel confirmation the air master receives a clean position+velocity target one Separation ahead at the configured absolute relative altitude, and follower N receives the interpolated trail point `(N-1) × Separation` behind at trail altitude plus the configured altitude. Ground velocity feed-forward is scaled by 0.6 for the air master and 0.5 for followers. The session-only trail is bounded to 5000 points, coalesces movement below 0.1 m and stops on jumps over 500 m; every commanded air role must remain exact ArduCopter GUIDED. Route, topology, role, settings, eligibility, reconnect, freshness, partial-send and window-close changes stop and release the complete session. MP10 near-waypoint geometry, exact mission/current-navigation storage and the central ACK arbiter are implemented and tested, but the window has not yet bound those new services per tick. Turn anticipation and six bulk commands therefore remain visibly disabled.

`SwarmWaypointLeaderCore` pins ground/air/follower roles and the main mission to exact instances plus content generation/digest and MP10 geometry signature; accepts only relative-altitude navigation destinations; implements staged GUIDED → newer heartbeat → ARM → newer heartbeat → TAKEOFF, path following, return and separated RTL landing; and preserves pending batch identity through cancellation or reset. Collision overrides run before normal progression only when every armed recipient is exact GUIDED, and never inject PositionTargets during RTL Landing. `SwarmWaypointLeaderExecutor` maps every intent to the shared stream/target/ACK/parameter services through atomic reservations, exact runtime parameter types and fail-closed draining. `SwarmWaypointLeaderWindowAdapter` supplies exact discovery, monotonic mission observations and lifecycle-safe delegation to the enabled non-empty production window. The remaining recovery gap is an explicit default-Cancel operator acknowledgement that clears terminal `OutcomeUncertain` only after vehicle state has been checked; restarting the application is the current conservative recovery.

`LinkStatsWindow` is a fresh 300×250 modeless window per invocation. Every refresh resolves the current exact target to its physical link again, converts the rolling Qt bit rate to bytes per second and reads per-link MAVLink received/lost counters. Link removal or absence shows zero/em-dash values and an explicit status. `LogDownloadDialog` now closes through normal dialog lifecycle, clears an interrupted vehicle's state/connections and presents an explicit no-vehicle state.

`MavlinkLogWindow` is a fresh 460×340 modeless window per invocation. Its private-buffer reader streams timestamped MAVLink 1/2 records without sharing a protocol channel, resynchronizes corrupt input, and cancellation or failure leaves no partial single-file output. KML/GPX use valid `GLOBAL_POSITION_INT` tracks, CSV/text decode bundled-dialect fields, parameter extraction preserves ArduPilot versus bytewise wire semantics, and mission extraction requires complete per-endpoint transfers and deduplicates snapshots. The source tlog can never be selected as an output, and automatically named mission siblings cannot silently replace existing files. All exports use an explicit sensitive-data confirmation whose default and Escape action is Cancel. Matlab remains visibly unavailable until a verified cross-platform MAT-file writer exists.

`SpectrogramWindow` is a fresh 1050×820 modeless window from both TOOLS/Ctrl+L and SETUP Advanced. It opens Unicode-path DataFlash `.bin`/`.log` files, selects ACC1–5/GYR1–5 and computes separate 1024-point Hann-windowed FFT images for X/Y/Z off the UI thread. A lightweight log-sink interface lets the existing ASCII/binary parsers stream only the chosen sensor instead of retaining the complete log. Direct ACC/GYR rows use fourfold overlap and split only on large forward timestamp gaps, so parser-repaired duplicate timestamps remain contiguous; current `QHBBHHQf` ISBH plus `QHHaaa` ISBD batches are correlated by number/type/zero-based instance/sequence with incomplete-batch fallback, and the ASCII parser accepts both flattened and bracketed `int16[32]` array fields. `SampleUS` has priority in the modern direct `ACC,QBQfff` layout. Parsing is cancellable, selected input is capped at 2,097,152 samples and each output is capped at 2048×512 by default; both ASCII and binary parsers now have a direct regression proving that a rejecting bounded sink stops parsing after the rejected row. When more FFT windows exist than raster columns, per-frequency maximum pooling retains transient peaks. The two obsolete legacy Load-button Ctrl+L bindings were removed so the MP10 Tools shortcut remains unambiguous. Deterministic direct and synthetic Unicode ASCII/binary batch tests cover axis peaks, the fifth sensor, malformed input, bounds, repaired timestamps and teardown.

`Terrain3DWindow` is a fresh 1100×760 modeless software-rendered heightfield view from both TOOLS and SETUP Developer Tools; it deliberately does not enable the unrelated legacy OSG/QGLWidget `Pixhawk3DWidget`. It binds HEARTBEAT, GLOBAL_POSITION_INT and ATTITUDE to one exact `(link, system, component, generation)` epoch, clears stale frames on target loss/change and shows armed state, mode, coordinates, AMSL, attitude and velocity. Cancellable workers sample the common local-GDAL/SRTM elevation source into a bounded 17–65 grid, build elevation shading/fog/grid/MAV marker frames and publish only while the captured target epoch is still current. Lock-to-MAV/free-camera movement, vertical exaggeration, automatic recenter, reload coalescing and terrain hover raycasts are active. Imagery controls remain visibly disabled until the canonical shared tile cache can supply a bounded in-memory atlas. The shared guided service now exists, but clicks stay read-only until current-altitude, per-click default-Cancel confirmation, explicit policy and tests are added. Core geodesy/dateline/projection/render, telemetry isolation/reentrancy and window lifecycle tests plus a live X11 main-menu smoke pass.

`ExternalGuidedWindow` is a fresh 620×390 modeless window from both TOOLS and SETUP Advanced. It accepts a bounded Unicode-path file containing exactly three C-locale fields, validates it before a default-Cancel warning and rereads it after consent so a changed command cannot bypass confirmation. The application-owned service pins one `(link, system, component, generation)` lease with a fresh heartbeat, serializes one owner, queues only the newest update and sends `COMMAND_INT DO_REPOSITION` with CHANGE_MODE until the first terminal Accepted acknowledgement. One-second rereads resume only after the preceding command is accepted; invalid runtime contents withhold the update without stopping. A lost acknowledgement retries the identical target up to three sends; an Accepted result after retry drains possible additional retry acknowledgements before sending the newest queued target. Stop releases its window immediately and ambiguous outcomes use a bounded late-ACK isolation interval. Since MAVLink carries no command transaction ID or echoed coordinates, the UI/documentation treats Accepted as command-level rather than proof of a particular coordinate payload. Target changes stop the session, `(0,0)` is withheld, and transport/rejection/unsupported states remain visible. MP10's legacy Plane/current=2 and unacknowledged position-target fallbacks remain compatibility work.

`FollowMeWindow` is a fresh 480×420 modeless window from both TOOLS and SETUP Advanced. It accepts either validated manual WGS84 coordinates or framed serial NMEA GGA input, with explicit port/baud, 0.25/0.5/1/2 Hz rates and relative altitude. Start requires a default-Cancel movement warning, then reserves the same application-owned guided sender against one fresh immutable target before opening the serial port; no command is sent until a valid fix exists. Valid updates are newest-wins and ACK-gated through `DO_REPOSITION`; malformed, no-fix and stale input withhold movement, while target change, link loss, input failure, Stop or window destruction ends the session and releases ownership. Exact `(0,0)` is deliberately rejected as ambiguous, but either zero-valued axis remains valid. The port does not reproduce MP10's unacknowledged fixed-rate/legacy fallbacks: the heartbeat freshness bound and command-level acknowledgement remain visible safety differences pending physical serial, older-autopilot, reference-screenshot and native-platform evidence.

`MovingBaseWindow` is a fresh non-empty 580×500 modeless window from both TOOLS and SETUP Advanced. It reserves one immutable exact vehicle target before opening an input, accepts framed GGA from Serial 8N1, newest-client TCP Host, TCP Client, first-sender-pinned UDP Host or an exact-endpoint UDP Client that announces its return port with one empty discovery datagram, and publishes the newest fix at 0.25/0.5/1/2 Hz. No-fix or stale input clears the Flight Data cyan `BASE` marker while leaving the input session active; target/link/owner changes stop and clear it. Host listeners bind all IPv4 interfaces but UDP Host refuses any port owned by an active MAVLink link and uses an exclusive bind. Settings persist only after Listening/Ready, and raw lines are bounded to 4096 bytes in a 4 MiB active log plus one capped backup. TCP Client deliberately does not auto-reconnect yet. Unlike MP10, Moving Base does not feed NTRIP GGA. Relative altitude and Rally Point 0 stay visible but disabled until an exact home-altitude source and an exact `MAV_MISSION_TYPE_RALLY`/`MISSION_ACK` transaction exist; MP10 has no position-offset controls. Unit, full-suite and production X11 click/open/clean-exit evidence pass; physical serial/network sources, live marker evidence, reference screenshot and native platforms remain.

`PropagationSettingsWindow` is a fresh non-empty 480×700 modeless window from TOOLS/Ctrl+W. It exposes and autosaves all 16 MP10 propagation settings. Application-owned DATA and PLAN controllers independently render elevation/terrain rasters, a 360-degree Home-centred RF terrain-intercept contour and red/orange Home/vehicle battery-distance rings through the shared map contract. Cancellable generation-guarded workers use the common GDAL/SRTM elevation source, a bounded 4096-pixel raster dimension and bounded RF sampling; SRTM admission is deduplicated before GUI dispatch and capped at eight unique queued/active tiles. Stale viewport, settings, terrain or exact-target epochs cannot replace newer results. Missing/ocean/zero raster samples remain transparent and missing RF sectors suppress the complete contour with a visible status rather than claiming verified coverage. Wrapped vector paths do not draw false world-spanning chords; raster extents crossing the dateline remain fail-closed until they can be split. The 0.5-degree azimuth option is functional and zero convergence cannot enter an unbounded loop. Exact-target telemetry combines HEARTBEAT, HOME_POSITION, GLOBAL_POSITION_INT/GPS_RAW_INT and primary battery data; distance-left is unavailable until consumption and valid armed GPS movement exist. Another production map backend, reference screenshots, native platforms and representative live terrain/vehicle evidence remain.

`OsdVideoOverlayWindow` is one modeless 1120×720 TOOLS/Developer Tools window. It loads a local video through Qt Multimedia, asynchronously builds a 100 ms `.tlog` state timeline after the first `MAV_COMP_ID_AUTOPILOT1` heartbeat, applies a -900..900 second offset and renders the private default `HudControl` into a new silent JPEG85 MJPEG AVI. Source resolution or a maximum 960-pixel width is selectable, input/output identities and overwrite are rejected, cancellation finalizes a playable partial file, and all frame queues are bounded: source frames are never silently substituted and overrun fails explicitly. Microsecond PTS rounding preserves ordinary CFR frames and EndOfMedia waits for accepted cross-thread deliveries. On Linux/Qt5, only the two unused eager GStreamer display controls are temporarily assigned inert sinks during player construction, avoiding an Intel VA-API crash while the real surface remains `QAbstractVideoSurface`. Audio copy, complete firmware-specific CurrentState/mode coverage, real camera/tlog reference evidence and Windows/macOS validation remain.

`SerialPassThroughWindow` is a fresh 480×380 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Start snapshots the current exact target's physical link; successful inbound MAVLink frames are reconstructed byte-for-byte after framing and forwarded without touching the original traffic. Serial uses 8N1/no flow control, TCP Host retains one newest client, and UDP Host learns and fans out to a bounded peer set but refuses a collision with an active vehicle UDP listener. A 256 KiB total mirror-owned backpressure bound drops newest complete frames with a visible counter instead of blocking the protocol thread. Peer bytes are counted and drained with write-back disabled by default; enabling it writes raw bytes through an ephemeral link-ID lookup and automatically disables the reverse path if the link rejects them. Closing one window stops only its own service.

`SerialOutputNMEAWindow` is a fresh 480×400 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Connect pins the exact `(link, system, component)` target and stops explicitly if that physical link disappears. `GLOBAL_POSITION_INT` drives position/altitude, with `GPS_RAW_INT`, `VFR_HUD` and `ATTITUDE` fallbacks/overrides matching MP10 precedence. Each timer tick emits one complete GGA/GLL/HDG/VTG/RMC cycle with checksums and CRLF through bounded Serial 8N1, newest-client TCP Host or learned-peer UDP Host outputs on port 14551. Nothing is written before the first usable `GLOBAL_POSITION_INT` or any `GPS_RAW_INT`; matching MP10 state semantics, a no-fix GPS report can still produce zero coordinates with GGA quality 0. Transport failures remain visible. Closing one window stops only its own service, and guarded service pointers make destruction order safe in both NMEA and Mirror windows.

`SerialOutputCotWindow` is a fresh 720×820 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. It reproduces the reference endpoint order/defaults, 0.1–3600 second interval, event/UID/callsign controls, six-column advanced identity grid, Connect/Stop, indentation and last-event preview. Connect pins the current physical link but emits for every exact system/component endpoint discovered on that link, matching MP10's one-comPort scope without collapsing equal IDs on other links. TAK multicast, UDP Client/Host, TCP Client/Host and Serial 8N1 are separate bounded transports; TCP Host broadcasts to up to 16 clients and UDP Host targets the newest sender. XML attribute/element order, precision, escaping, UTC start/stale and platform-native indentation match MP10, while invalid XML/non-finite state is rejected visibly. Closing one window stops only its session.

MAVLink Device Operations is now a fresh independent modeless window from TOOLS, Ctrl+J and SETUP → Developer Tools. Its event-driven service sends DEVICE_OP_READ/WRITE only on one generation-checked physical-link lease, rejects MAVLink 1, correlates replies by exact link/source/type/request ID, bounds all protocol fields and cancels safely on target change, link loss or destruction. The destructive ICM20948 write/read test requires a current disarmed heartbeat and refuses an edited destination outside the bound endpoint; ordinary register reads retain MP10's editable destination on the pinned link. All 24 MP10 TOOLS catalogue entries now have real handlers in the Qt Multimedia build; finish the partial single-vehicle workflows, then move the primary stream to Settings/CONFIG. Swarm remains last by user instruction.

The shared MAVFTP remote-file browser is now routed from both CONFIG and SETUP. It provides the MP10 Refresh/Download/Upload/Delete/Mkdir workflow plus explicit cancellation, lazy `/` and `@SYS` browsing, progress, atomic `QSaveFile` downloads with collision-avoiding names, and a default-Cancel warning before a potentially replacing upload. Its application-owned service pins one exact physical-link target per operation, strictly correlates the MAVFTP response envelope, paginates directories, retries with per-operation bounds, closes and flushes upload sessions before a 30-second-budget CRC verification, and cancels safely on target/link changes. The current safety bound buffers files up to 64 MiB; exclusive no-replace local creation, burst/streaming transfer and live hardware evidence remain.

## Settings/CONFIG current phase

`CONFIG_SETTINGS_INVENTORY_AUDIT.md` is the count baseline. MP10 and Qt now
both have 15 ordered concrete CONFIG factories and every supported vehicle
route dispatches to a truthful non-empty page. Plane now receives the native
QP Extended Tuning surface while Copter/Heli keep the useful legacy editor;
Flight Modes uses one native exact-target page shared with SETUP. Other useful
legacy tuning and GeoFence pages remain visibly classified instead of being
deleted to improve the raw count.

The latest source audit confirms 15/15 concrete direct route factories and all
nine Planner sections, but only 21 of the 64 MP10 Planner controls are native
equivalents. CONFIG → Planner is the sole production and source route: the
orphaned `actionSettings` path and duplicate top-level dialog have been
removed. The page now distinguishes Layout from the unported MP10 Theme/Edit
Custom editor and explicitly discloses the missing dist-to-home, Track Length
and safe runtime GCS-sysid controls. A production click-through test for every
CONFIG row is the next route-level Settings check.

The shared native Flight Modes page reproduces the six MP10 rows and exact PWM
bands for Copter, Plane, Rover and PX4 parameter schemas, both Copter Simple
masks, Current Mode/PWM, active-row highlighting and the reference help URL.
ArduPilot choices come from packaged metadata with unknown-value preservation
and the MP10 `31: ModelCal` extension; PX4 slot enum values remain separate
from packed heartbeat custom modes. A read-only MAVLink adapter accepts
heartbeat and RC data only for one immutable link/system/component/generation;
stale telemetry clears the live presentation. Edits require a fresh disarmed
heartbeat and Save submits one changed-only ordered exact-target batch, with
full refresh reconciliation after any possible partial application. Ordinary
parameter refreshes preserve staged edits, while reconciliation replaces them
with the exact committed snapshot.

Legacy `Heli Setup` is exposed only with MP10's exact `H_SWASH_TYPE`
capability. It is a dedicated non-empty native page, not Copter Basic Tuning:
all 43 reference logical fields/aliases, CCPM/H1, six visible manual-servo
actions, collective/acro plot, RC3/RC4 inputs, servo-output-6 cursor, three
position readouts and manual-only range capture are present. Writes use one
immutable exact target and finish by exact batch ID rather than an unrelated
same-name PARAM_VALUE. Non-zero modes require disarmed state and a
default-Cancel blade-removal confirmation; unsupported mode 5 remains visible
but disabled. Once a manual write is submitted, neither a raw zero echo nor a
lost ACK can clear its conservative uncertainty state; only the exact
successful zero batch can do that. Hiding/deactivating the page attempts
`H_SV_MAN=0`, and target or link uncertainty produces an operator-visible
warning. Current `H_SW_TYPE` helicopters now use the separate five-section
SETUP `Heli Setup (4.0+)` page.

Planner Settings now uses `ConfigPlannerView`, not `QGCSettingsWidget`, on the
active CONFIG route. A second source-level dialog shares that page but is
orphaned and is not counted as a production route. The exact nine
reference sections are always present and non-empty; MP10's 64 interactive
controls are audited by type. The safe slices make units, the shared
layout profile, exact startup UDP policy, map backend, audio/heartbeat/logging,
log directories, beta channel and proxy effective. HUD Overlay, Speech master,
Test Speech, Armed Only, Waypoint, Mode, Custom, Battery, Alt Warning,
Arm/Disarm, Low Speed and Message Severity now work live through shared
application services, bringing direct MP10 coverage to 21 of 64 controls.
Periodic phrases, the armed No Data warning and complete MAVLink 2 STATUSTEXT
messages consume one exact physical-target telemetry epoch; high messages use
the MP10 threshold/prefix policy, legacy ArduPilot severity normalization,
ten-second red/yellow/white HUD presentation and one-shot bounded TTS queue.
The retained Qt PreArm/Arm/#audio phrases now enter that same assembled,
deduplicated path instead of bypassing it. Vario and speech level remain the
next speech gaps; shortcuts, target-safe rates/identity, map overlays/ADS-B and
advanced policies are stated in place.
Useful legacy themes, file paths and seven-rate editing remain in an
explicit Legacy dialog; controls with no consumer or unsafe split ownership
are hidden.

## PLAN action-column audit

The direct action inventory has been compared against MP10 `FlightPlannerView.axaml`. MP10 has 24 direct layout items, including 19 interactive controls (12 buttons, two checkboxes, two combo boxes and three editors). The Qt panel has 38 direct items and 29 interactive controls, adding the working Polygon group, transfer cancellation/progress and another editor. No direct MP10 action is absent by count.

Seven Qt actions remain deliberately disabled because their end-to-end workflow is not yet ported: Grid display, View KML, WMS, WMTS, Inject Custom Map, Use MAVFTP and Write Fast. The remaining controls are working or conditionally disabled according to connection/transfer state. The lower blank area follows the same top-aligned scrolling layout behavior as MP10 and is a deferred GUI difference, not a missing dock or hidden action group.

The erroneous outer right strip is fixed separately from that legitimate inner
spacing: `DockableView::setPanelFixedExtent()` now constrains both the splitter
wrapper and its content. Production-order tests prove the exact 168-pixel
ActionPanel width, 210-pixel WaypointPanel height, full-height right column and
right-edge ownership across 1120, 1280 and 1600-pixel windows, including
hide/show and saved-layout restore.

The waypoint table now also exposes the MP10 Up, Down and Delete operations as
visible per-row controls. They select the clicked row and reuse the existing
QAction paths, so transfer locks, store-aware undo and DO_JUMP remapping remain
centralized; mouse and keyboard activation plus boundary rows are covered.

The next coherent PLAN package should combine Grid, KML preview/export, a persistent custom XYZ source using the canonical cache identity, and store-aware undo for Polygon-to-Fence conversion. WMS/WMTS and MAVFTP/Write Fast can follow later. Main work has moved to SETUP as requested.

## SETUP next phase

The SETUP phase must compare the whole MP10 navigation model with the Qt backstage model, then classify every route as working, partial, stub or missing. Prioritize pages that can be made end-to-end functional on existing exact-target/parameter/command services and already implemented widgets.

The current navigation audit in `SETUP_INVENTORY_AUDIT.md` finds 53 MP10 pages versus 43 concrete Qt page factories. Both have the same three named group headings, for totals of 56 versus 46 navigation entries. Qt is missing 11 MP10 pages and adds the useful native `QML Plugins` manager, so this is intentionally not a one-to-one count. The modern Heli route is first under Mandatory Hardware and uses `H_SW_TYPE`, correcting MP10's stale `H_SWASH_TYPE` gate without aliasing the two schemas; the current and explicitly named legacy Frame Type pages follow it and no longer depend on a late firmware-version callback to create their content. The current `Compass` route follows Accel Calibration and precedes the explicitly named `Compass (Legacy)` widget; its parameter/priority, onboard and Large Vehicle workflows use exact-target services, the dedicated `Compass/Motor Calib` route is now active under Optional Hardware, only OfflineMagFit remains absent from the Compass family, and the legacy global dialogs stay quarantined. The Flight Modes route after ESC Calibration now shares CONFIG's native exact-target six-row page instead of the global-UAS legacy widget. The SETUP `OSD` route uses the dedicated `ConfigHWOSDView` legacy MinimOSD telemetry helper; the separate CONFIG `Onboard OSD` route now uses its own phase-one layout editor, never the unrelated old `OsdConfig`. One shared `displayview` service preserves all 35 MP10 SETUP feature flags; 31 currently gate matching factories and the four flags for missing FFT, Joystick, PX4Flow and REPL routes remain dormant. SETUP persists/restores `setup_lastpage`; missing routes remain missing and useful Qt-only/legacy pages remain honestly classified.

`setupviewroutes_tests` now runs inside the BUILD_TESTING production binary and constructs the real hidden `SetupView`: it locks the exact 43-page/three-group order, every real factory, click selection, stack ownership, semantic non-empty content, Optical Flow reset/recreation and late shared-action recovery without invoking page controls or lifecycle network/hardware actions. This audit exposed a real shutdown UAF and the saved Advanced-page startup regression; page teardown now occurs while services remain alive, and action-backed pages are rebuilt after the TOOLS catalogue appears. Connected/vehicle/profile visibility matrices remain a separate follow-up.

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with ten working shared tools (MAVLink Inspector, Mavlink Mirror, NMEA, Cursor-on-Target / TAK, DataFlash Spectrogram, External Guided, Follow Me, Moving Base, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Six Advanced actions remain explicitly disabled. Developer Tools exposes the exact 32-action inventory with working byte/MAVLink/hardware-ID parsers plus the shared Device Operations, 3D Terrain and OSD Video windows: 5 of 32 actions are usable and the other 27 are explicitly disabled. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

The first-activation SETUP stall is fixed: `ApmCustomFirmwareConfig` yields before serial enumeration, downloads the manifest asynchronously through a dedicated reply, uses progress-based inactivity watchdogs, and keeps firmware download/upload state isolated. Flash actions retain an immutable selected-device snapshot and remain locked until a terminal upload outcome; destruction aborts replies and stops uploader timers. The still-large uncompressed manifest, GUI-thread JSON parsing, serial enumeration and complete MP10 firmware-selector parity remain explicit follow-up work.

`ESP8266 Setup` is now routed after Parachute and uses a dedicated exact-link client for `MAV_COMP_ID_UDP_BRIDGE` instead of changing the globally selected autopilot component. It waits for the 18 MP10 settings, displays the exact control set/defaults, serializes all 22 parameter writes and ACK-gates storage, reboot and reset. Unit/UI/transport coverage includes wrong-link replies, retry, partial response, target invalidation, reentrant cancellation and destruction; live bridge hardware, profile gating and cross-platform visual evidence remain.

The next audited SETUP packages are CubeID and Secure. CubeID is a target-aware firmware updater, not another HW-ID presentation; its CubePilot messages are absent from the current generated dialect and require a coordinated MAVLink update. `ConfigSecureView` manages bootloader public-key slots with `SECURE_COMMAND`, while `ConfigSecureApView` generates Ed25519 keys and signs bootloader/firmware files; MAVLink link signing remains a separate Advanced Tools workflow. The security pages need a reviewed cross-platform Ed25519 dependency before implementation.

The three Antenna Tracker routes are audited in `SETUP_ANTENNA_TRACKER_AUDIT.md`. The `Antenna Tracker (Serial)` and `Antenna Tracker (Live)` routes now exist after `ESP8266 Setup`: one shared `AntennaTrackerUIViewModel` (owned by `SetupView`, so the tracker loop survives page resets and navigation like MP10) drives `ConfigAntennaTrackerView` and `AntennaTrackerUIView` on top of `AntennaTrackerSerialService`, `AntennaTrackerGeometry` and a `UasAntennaTrackerTelemetrySource`; `antennatrackeruiviewmodel_tests` (11 cases) and `antennatrackerviews_tests` (4 cases) cover MP10 texts, validation order, settings, the loop, manual mode, live trim/reverse, failure recovery, the trim sweep and shutdown. `RadioStatusMonitor` now consumes both MAVLink radio-statistics messages per physical link and reproduces MP10's raw-unit SNR formula, 50 percent read-driven EMA and one-second hold; the telemetry source reads only the current exact target's link. The pages were smoke-tested on real X11 under Xvfb with a local SITL: both routes render, a real `QSerialPort` connect to a local UART reached `Connected (Maestro).`, Vehicle Az updated live and the application closed cleanly while connected. Deviations SETUP-018..022 record the shared instance, the Home / Center quirk, the intentional cancellable trim state machine, gating/tracker-home gaps and GUI approximations. The active sequence is the PLAN "Tracker Home" action, then the exact-target 25-field parameter page and remaining route/profile gates. The old `AntennaTrackerConfig` remains until these replacements are routed and verified.

Legacy APM Planner binary plugins are not a requirement. Where MP10 calls something a plugin/page/tool, reproduce the user-visible function with a native Qt page/service or the trusted QML extension system; do not restore the old ABI.

## Settings/CONFIG audit baseline

The active Settings path is `MainWindow -> ConfigView`; the compiled
`ApmSoftwareConfig` is not the production surface. MP10 and the truthful Qt
shell now both register 15 ordered concrete CONFIG factories. `MAVFtp` uses
the same functional native remote-file browser in CONFIG and SETUP, and legacy
`Heli Setup` now opens its own parameter/live-setup page. `Onboard OSD` opens a
dedicated, non-empty 30x16 phase-one layout
editor rather than the unrelated legacy MinimOSD stream-rate helper. It parses
complete EN/X/Y triplets, stages drag/toggle/coordinate edits, confirms refresh
when local changes exist, submits one exact-target batch and retains ownership
through partial failure, item cancellation and timeout until terminal batch
completion. Helicopter detection combines MP10's
`H_SWASH_TYPE` marker with an early heartbeat fail-safe and hides Copter Basic
rather than opening the wrong widget. The shared firmware
family now gives Plane Basic to VTOL and Rover Basic to surface boats. Flight
Modes is a native shared CONFIG/SETUP page; useful legacy GeoFence, vehicle
tuning and Planner factories remain under their reference headers with a
visible `Legacy` badge. Plane `QP Extended Tuning` uses its native page, while
the deliberately narrower GeoFence vehicle restriction composes
with MP10's profile gate. The QtCore-only `ConfigRouteProfile` records all 15 MP10 routes in
reference order, separates reference visibility from vehicle/capability
actionability and tests offline/advanced, Copter, legacy/current Heli, Plane/VTOL,
Rover and per-feature profile gates. `ConfigView` consumes that policy instead
of maintaining a second set of vehicle lambdas. The deterministic JSON
`DisplayViewProfileService` supplies all 11 CONFIG and 35 SETUP flags, with 31
of the SETUP flags currently consumed by existing navigation factories,
preserves unknown Custom fields, starts Advanced when no profile is stored,
applies MP10's startup parameter-list overrides and keeps the old Advanced Mode
action synchronized as an exact Basic/Advanced preset switch. Existing
`QGC_MAINWINDOW/ADVANCED_MODE` state is migrated once when no profile exists.

The legacy Planner page now exposes the shared Basic/Advanced/Custom layout
selector and MP10's Startup UDP Listener controls while its native replacement
is built. The exact three MP10 keys default to independent ports 14550/14551,
validate 1..65535, collapse duplicates and take effect after restart.
LinkManager creates each configured listener unless that exact port already
exists, and excludes automatic listeners from the saved manual-link array, so
disabling them is effective on the next launch. A single hostless legacy 14550
definition is adopted once; manual definitions with outbound hosts are never
discarded. Failed automatic binds stop after one attempt, and editing an
automatic link explicitly converts it to a persisted manual definition.

The persistent blank Settings content path through vehicle/parameter resets is
fixed at the shared backstage boundary. Re-enabling automatic selection with
an empty stack now synchronously materializes the first visible concrete page,
while `ConfigView::resetVehiclePages()` preserves a still-visible current route
and the constructor's deliberately disabled lazy-selection state. Programmatic
visibility fallback is guarded so it does not overwrite the user's saved route.
The regression test proves a non-empty page ID, current widget and stack index
after the formerly blank reset ordering without eagerly constructing an earlier
factory. A broader production `ConfigView` matrix across Copter, Plane, Rover,
Heli, advanced mode and target changes is now covered at the pure production
route-policy boundary; a full singleton-backed `ConfigView` fixture remains
unnecessarily broad. The first truthful route/profile correction is now in
place. The next Settings package should add the common unit service/speed
controls and OSD text/background color, followed by the remaining Planner
speech consumers and then the later full-canvas/tuning-slot OSD
phases documented in `OSD_AUDIT.md`. The parity ledger retains the exact
per-route classification so missing pages cannot be mistaken for working
settings.

## Architecture constraints

- Product baseline is Qt 5 with a forward-compatible Qt 6 path; do not copy Qt 6-only QGC UI dependencies into the required core.
- All mission, terrain, distance and altitude data remains canonical SI internally; unit preferences convert only at presentation boundaries.
- All map backends share the same provider identities and canonical cache root.
- New transport work must retain exact link/system/component identity, target generations, ACK correlation and cancellation on target change.
- Dense editors/shell/core stay in C++/Qt Widgets. QML is appropriate for plugins and dynamic tools and receives direct trusted core access.
- DATA and PLAN are fixed in-page layouts; no floating/docking framework is required.
- Fresh 3.0 namespace means no legacy settings, layout or plugin compatibility work unless a current file format is itself part of Mission Planner interoperability.

## Build and verification safety

The coordinating agent owns the build schedule. A delegated agent may build only while holding an explicit lease for one exact command. Use one build process globally across all workspace repositories, maximum `-j12`; never use a bare `--parallel` or `-j`, and do not modify source files until the leased build finishes.

Immediately before every configure or build, execute this exact standalone command:

```sh
pgrep -af '(^|/)(cc1plus|clang\+\+|g\+\+|c\+\+)( |$)' || true
```

Do not combine it with the configure/build command. Do not launch if other compilers are active. On 2026-09-02 a separate GTU build used bare `cmake --build ... --parallel`; GNU Make expanded it to unbounded jobs, the OOM snapshot contained 153 `cc1plus` processes using roughly 13 GiB resident memory, and the desktop session failed. Always supply the numeric limit even when only one agent appears active.

For each broad slice, perform proportionate unit/integration tests, then a single full build and full test pass. UI/navigation/shutdown changes also require a real-X11 smoke when available. Commit the verified slice and update this handoff.

## Source-of-truth files

- `AGENTS.md`: operational constraints every agent must follow.
- `PORTING_PLAN.md`: architecture and acceptance contract.
- `MISSION_PLANNER_SCREEN_PARITY.tsv`: authoritative full screen/function inventory.
- `PORTING_DEVIATIONS.tsv`: separate functional/GUI discrepancies and disposition.
- `DOCKING.md`: fixed DATA/PLAN panel decision.
- `QML_PLUGINS.md`: trusted QML extension API and policy.

Reference source trees are listed in the root `AGENTS.md`.
