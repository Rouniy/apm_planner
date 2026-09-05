# Anon Log

Reference: MP10 `ConfigAdvancedViewModel.AnonLogAsync`,
`Services/DataFlashLogAnonymizer.cs`, `ExtLibs/Utilities/Privacy.cs`; original
`Plugins/AnonymizeBinlogPlugin.cs`. This is the first Advanced Tools action,
not an additional entry in the fixed top-level TOOLS menu or a Setup page.

## Workflow

The native modeless `AnonLogWindow` selects a BIN, text LOG or MAVLink TLOG,
suggests `<name>-anon.<extension>`, accepts separate latitude/longitude offsets
and shows progress, counters and errors. Blank offsets choose independent random
values with magnitude 0.5..2.0 degrees and random signs. An explicit
default-Cancel confirmation freezes the paths and concrete offsets. The input
cannot be the destination, including its canonical symlink alias; the output
retains the input format.

One application-owned `LogAnonymizeService` runs a job on a dedicated
single-thread QtConcurrent pool. Closing
the window detaches presentation; reopening observes the same job/result.
Explicit Cancel is token-scoped. Shutdown requests cancellation and joins the
worker before service destruction. Normal file reads and transforms are
bounded/chunked; this does not promise interrupting an operating-system read
stalled on a faulty filesystem.

The file facade hashes the open input before and after transformation and refuses
publication if its content changes. Backends write only an uncommitted staging
device. `QSaveFile` has direct-write fallback disabled; only successful final
commit reports success. Errors and cancellation before commit preserve both
the source and the previous destination. A cancellation arriving after the
commit boundary cannot undo a successfully published file.

## Meaning and limits

This is coordinate obfuscation, **not full sanitization**. A constant translation
preserves flight shape; one known location can reveal the offset. Altitude,
timestamps, identifiers, parameters, free text, relative tracks, embedded
RTCM/log payloads and unrecognized location fields can still disclose sensitive
data. The warning appears before work and in results. Never describe the output
as safe to publish without inspection.

BIN retains non-coordinate bytes and record framing, uses authoritative FMTU
D/U units per field and name fallback for absent/dash/question unit annotations,
with a warning for each such FMTU field. Explicit conflicting non-coordinate
units fail. This closes the reference's RGPJ/RBCH/AIS annotation gap and preserves zero
sentinels. FMT `I` coordinate bits use the signed e7 semantics used by real
ArduPilot DAL records, including crossing zero. The implementation streams metadata/rewrite passes instead of loading
the whole file. Changed FMT/FMTU redefinitions, malformed framing, unsupported
coordinate encodings and numeric overflow fail without publishing a misleading
partial output. A final partial known non-coordinate record can be retained
byte-exact with a visible warning, matching representative finalized SITL logs;
a partial coordinate record is not accepted. Identical definitions may repeat; late FMTU is applied to its
type. Altitude is not inferred from the ambiguous `lt` substring.

Text LOG preserves unrelated text and newline style rather than rejoining every
record as MP10 does. It transforms recognized Lat/Lng columns from FMT records.
Only those exact text column spellings are recognized; lowercase lat/lon and
OLat/OLng remain an explicit asymmetry with BIN. Unknown text record names are
copied without interpretation; a file without any FMT is rejected. Patched
numbers use 17 significant digits, not their original decimal precision.
TLOG preserves timestamps, source IDs, sequence and MAVLink version, patches
recognized coordinate fields and recomputes CRC without touching live protocol
channels. A changed signed frame loses its now-invalid signature and signed
flag; the result reports the count. This tool neither verifies nor regenerates
cryptographic signatures. Unknown-dialect, corrupt or truncated frames are an
explicit failure, not an invisible drop or opaque passthrough.
Known TLOG messages outside the current transform list that expose recognized
coordinate names remain byte-exact and receive a deduplicated warning naming
the message; they do not inflate the processed coordinate counter.

## Intentional differences

- Separate offsets and the BIN random rule apply to all formats. Legacy MP10
  TLOG/LOG uses one positive random offset below ten degrees for both axes.
- Native reusable file/progress window replaces the reference prompt chain.
- Source replacement is forbidden, atomic staging and cancellation apply to
  every format, and processing failures do not publish partial files.
- TLOG does not upgrade v1 to v2 or round timestamps to milliseconds. The
reference omission of `lng` is corrected. Unrecognized embedded locations
  remain outside the claimed coverage.
- Ambiguous binary field inference and unchecked overflow are not reproduced.

`coordinateFields` counts definitions for BIN and encountered field occurrences
for LOG/TLOG; `patchedValues` reports the backend's processed nonzero BIN values
or actually changed LOG/TLOG values. General BIN multiplier application remains
inherited type-based behavior (including the float magnitude heuristic), not a
claim of every future FMTU scale. Geographic ranges are not normalized or clamped.
The TLOG extension covers `lng` and explicit global location-bearing mission
items; local frames/non-location commands, COMMAND_LONG param5/6 and reference-
unlisted position messages remain outside claimed coverage.

## Verification

Full Qt5/audio/Concurrent configure/build and **225/225 tests** pass (16.70 s):
`/tmp/apm-anonlog.I5u4vQ/configure.log`, `build-final.log`, `tests-final.log`.
The first suite was 224/225: a text file with no FMT was accepted. It now fails
unpublished; the final full run passes. Claude c190-c193 and three disjoint
Codex streams supplied independent reviews, final REVIEW-OK after fixes; root
owned all builds/tests. The actual themed UI additionally exposed and fixed a
collapsed result pane (now scoped minimum 110px in the 760x740 window).

Production X11: `advanced.png` (restored offline 13/16), `confirmation.png`
(default Cancel), `window-final.png`, `bin-result.png`, `tlog-result.png`,
`log-result.png`, `reopened-busy.png` and `cancelled-final.png`. The first large
run completed before a late Cancel click; it truthfully reported Completed.
The repeated run closes/reopens and cancels while active, preserving destination
SHA-256 `223de3446378ae66ef756deaf9c8306682ecdcd4247928f4ac4c79542b43abd0`.
Another active-job application shutdown exits 0 and preserves the same hash.
A separate probe cancels mid-rewrite after staging 52,505,549 bytes (exit 3),
also without publication.

Real ArduPilot SITL BINs 90,112 and 5,275,648 bytes retain exact file lengths,
including 3-byte MSG and 40-byte XKF5 incomplete tails. Final larger-file result:
54 coordinate definitions, 16,568 values processed and eight unannotated-name
warnings. Independent pymavlink comparison verifies 1,211 GPS records shifted
by +0.25/-0.25 degrees, preserving altitude/speed/time/status; synthetic BIN
non-coordinate bytes, text bytes and TLOG CRC/timestamp/version/IDs are checked
separately (`independent-verification.json`). Neither original SITL log is edited.

The final-build 105,000,089-byte BIN probe processes 7,000,001 records and
14,000,000 values in **5.16 s**, maximum RSS **11,776 KiB**
(`large-final-result.json`, `large-final-time.log`). Memory is bounded, not
proportional to file length. The manual `log_anonymizer_probe` target accepts
`input output latitude-offset longitude-offset [cancel-percent]`; it never
accesses a live vehicle. An optional cancel percentage returns exit 3.

Real TLOG/text logs, broader field/scale coverage, reference visual comparison
and Windows/macOS packages remain separate parity gates. A byte-identical
source rewrite is intentionally accepted by content-hash verification.
