# DataFlash BIN/LOG MATLAB export

## Reference workflow

MP10 FlightData's DataFlash Logs `Create Matlab File` uses the cached selected
BIN, prompting only when none is selected. `DataFlashLog.ExportMatlab` dispatches
BIN/LOG to `MissionPlanner.Log.MatLab.ProcessLog`; TLOG is a different algorithm
already covered by `MATLAB_TLOG_EXPORT_PORT.md`. LogBrowse also calls ProcessLog
for its current BIN/LOG path. This slice covers the DATA action; it does not
claim a complete MP10 LogBrowse replacement.

The exact derived name is **`<input path>-<logical record count>.mat`**, retaining
the `.bin`/`.log` suffix before the count. MP10 silently overwrites that name.
The Qt workflow prepares a read-only immutable plan, displays source/output/count
in default-Cancel consent and only publishes a new file. Progress/cancel and
MainWindow close/drain share the existing DataFlash controller.

## MATLAB contents

The output is uncompressed, little-endian Level-5 MAT, not renamed CSV:

- Each FMT contributes a `<type>_label` Nx1 cell of strings, beginning with
  `LineNo`; PARM labels are omitted as in MP10.
- Each accepted ordinary type/instance contributes a double matrix. Column 0
  is the one-based logical source record number; remaining columns follow FMT.
  Non-numeric strings, arrays and resolved mode names become numeric zero.
- Later FMTU instance metadata applies to earlier records too, matching the
  reference's full-file index before enumeration.
- MSG and ISBD retain numeric matrices and additionally produce MSG1/ISBD1,
  1xN outer cells containing Nx1 row cells, including the type token. Bracketed
  arrays become nested cells; numeric tokens become scalar doubles, text stays
  UTF16 char data.
- PARM is Nx2: final parameter names and numeric values. Seen is an Nx1 cell
  of base types, not instance-suffixed names. Reference Hashtable enumeration
  has no stable order; independent comparisons normalize only that order.

Empty strings use csmatio MLChar's 0x0 dimensions, not 1x0; nonempty strings use
1xUTF16-code-unit-count dimensions. This does not reinterpret source encoding:
the reference reads ASCII bytes, replacing each byte above 127 with `?`, so
UTF8 `café` becomes `caf??`. Qt deliberately preserves that source policy while
writing valid UTF16 MAT characters. A trailing LF in a bracketed ASCII token
can produce an empty nested text cell; it is not silently discarded.

Firmware is discovered from the first 100001 MSG/PARM candidates before data
conversion, matching the reference's prepass (including its final candidate).
Modes use the unchanged shared metadata snapshot. There is no special numeric
fallback for Copter 28: the shipped overlay resolves it to Turtle, whose text
then follows the ordinary non-numeric-to-zero matrix rule.

Top-level order is labels, first-encounter numeric matrices, first-encounter
MSG1/ISBD1, PARM, Seen. Source line ids count FMT and skipped records. MP10
excludes unterminated final ASCII lines; an explicit fixture pins this rule.
The x64 reference emits one output. Its 32-bit two-million-record splitting
is not a portable product policy and is not reproduced silently.

## Deliberate differences and remaining limits

The primary `StartsWith("FMT")` condition incorrectly treats FMTU as an FMT.
It produces bogus `<UnitIds>_label` variables, often invalid or duplicated names.
Qt distinguishes exact FMT from FMTU. Identical repeated schemas are deduplicated;
conflicts fail explicitly. Oracle exceptions must identify these exact bogus
reference variables, never hide missing legitimate matrices behind a wildcard.
FMTU still supplies instance routing but produces no numeric matrix or Seen
entry. Seen is deterministic first-seen order in Qt, not a new type filter.

A second verified reference defect is empty-Z recovery: BinaryLog's Aggregate
throws on an empty string, ReadMessage scans forward to the next record, then
DFLogBuffer rereads that record at its own indexed offset. The resulting export
fabricates duplicate flight rows with FILE/UNIT logical ids. Qt skips invalid
records and retains genuine source ids instead of reproducing corrupted data.
The comparison permits removal from the reference only after an independent
raw-BIN scanner proves an empty-Z source record, the immediately recovered type
and a bit-identical next-record payload. It removes 246 numeric rows and one MSG
cell on the real fixture; no arbitrary deduplication or source-row suppression
is allowed. This is intentional corrected behavior, not raw exact parity.

UTF16 cells are added to the shared MatFileWriter without replacing the tested
TLOG numeric API. One provider supplies one bounded cell at a time in column-major
order; the writer retains no whole-log cell tree. A nested value is limited to
depth 16, 65,536 nodes and 8 MiB encoded data; each Level-5 element fits uint32 and the
complete output stays within 64 GiB. Provider failure discards unpublished output.
These are admission limits, not benchmarks of every maximum combination.

Preparation/export also cap source input at 1 GiB, records at 20 million, variables
at 4,096, parameters at 10,000 and each MSG/ISBD table at 10 million rows. Cancellation
and content hashes guard source passes and publication; these checks require
stopped writers and do not constitute filesystem locks or source authentication.
The shared numeric TLOG API is retained. Preparation and final-path consent do
not authorize replacing existing files; publication errors keep old files.
Staging ownership is pinned by an open handle; Linux publishes that owned inode
with linkat instead of trusting a replaceable staging pathname. Foreign path
replacements are preserved. This does not establish concurrent-filesystem locks,
authentication or universal native-platform race freedom.

Culture-dependent parameter ordering/custom cell-number parsing, platform path
rules, malformed logs, reference GUI/HiDPI and native Windows/macOS evidence
remain explicit compatibility gates. No airborne operations or parameter writes
are part of this tool. Old useful log viewers are retained.

## Verification

Build 5 and all 305/305 tests pass in 53.53 s. All 16 regenerated actual
Release-reference comparisons pass. Production X11 reports zero audit failures;
four related suites each pass ten repeats in 6.65 s. The native controller
suite passes 15/15 in 2067 ms on build 5.

The oracle environment is part of the proof. Claude c331 exposed a stale Debug
reference directory without the shipped ParameterMetaDataLocal.xml overlay.
That environment incorrectly suggested a Copter 28 numeric exception. The
exception was removed; the shared snapshot was not changed. The stale Debug
oracle now fails before creating output. Regenerated actual
Release oracles require the overlay, assert Auto for Copter 3 and Turtle for
Copter 28, and record the reference bin path and overlay SHA256. The valid 100001-candidate
firmware prepass fix remains. Claude c329 contributed useful F1/F3 findings,
but its correctness verdict missed later defects; review is not validation.

Evidence is collected at `/tmp/apm-dataflash-matlab.fBPe1Y/`; final comparisons
are in `compare-current-all.log`, using `qt-current` and `net-current`.
`tests/oracles/DataFlashMatlabOracle.fsx` loads the actual MP10 assembly,
initializes the GUI flight-mode resolver and invokes the real ExportMatlab on
private copies. It records output names/counts and progress. The Release oracle
catalogue passes all 16 cases: 14 small fixtures, one real BIN and one large
fixture. The small cases cover arrays, Unicode, parameters, late instance
metadata, numeric boundaries, MODE 0–31 for Copter/Plane/Rover and rows before
firmware MSG, duplicate FMT, empty input, parser edges and empty-blob recovery.

The real BIN has 129,851 logical records and 430 reference variables. Comparison
passes for 292 legitimate variables (94 numeric and 198 cells), excluding exactly
138 erroneous FMTU labels and the source-proven 246 numeric/one MSG duplicate
recovery artifacts described above. Shapes, numeric bits, text and nested cells
are compared; only Seen order is freely normalized.

The preceding Qt build 4 Debug benchmark processed 2,000,003 records into one
MAT in 7.69 s with 12,932 KiB peak memory. All five variables (one numeric,
four cells) matched the actual oracle without exceptions. The separate .NET
baseline was about 8 s / 695,916 KiB. These are one measured fixture, not a
benchmark or memory guarantee for every maximum-size schema/cell combination.

`CompareDataFlashMatlab.py` uses independent SciPy reading plus a raw Level-5
top-level scanner to check names/order/duplicates, shapes, numeric bits, text
and nested cells. Only Seen order and explicitly named reference bugs may be
normalized. Four negative/proof tests in `TestDataFlashMatlabComparison.py`
pass; they test the comparison rules rather than trusting the exporter itself.
The available isolated interpreter is
`/tmp/apm-matlab-port.98lQAK/python-env/bin/python` (SciPy 1.16.1).

Remaining full-port work includes Geo Reference Images, Telemetry Logs and
other missing single-vehicle dialogs, Signing transitions, SETUP/CONFIG and
native-platform acceptance. GeoRef needs actual EXIF-preserving image copies,
CAM/TRIG/time matching, offset estimation, location reports and explicit
multi-file publication receipts; ordinary flight KML is not a substitute.
Dependency packaging, supported image/log formats and host-local EXIF time
interpretation must be explicit. Swarm remains last. This feature cannot
establish full Tools or whole-application parity by itself.
