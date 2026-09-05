# Developer Tools — Create DashWare CSV

Reference: Mission Planner 10 `ConfigDeveloperToolsViewModel.CreateDashWareAsync`,
`ExtLibs/Utilities/DashWare.cs`, `DFLogBuffer.cs`, `DFLog.cs`, `BinaryLog.cs`,
`Services/FlightModeNames.cs` and the bundled parameter mode metadata.

## Operator workflow

The existing action in TOOLS → Developer Tools and the shared SETUP page works
offline. Select a `.bin`/`.log`, enter semicolon-separated message types, then
choose the CSV output. The default types are `GPS;ATT;NTUN;CTUN;MODE;BAT` and the
suggested filename is `<basename>-dashware.csv`. Names are trimmed, uppercased
and deduplicated; an empty selection includes every declared type. A filter
with no matches produces a header-only CSV, not fabricated data.

The file/type/output prompts are nonblocking; processing runs on a worker with
modeless progress and Cancel. GPS extraction, Split, DashWare and the guarded
vehicle actions share one page operation gate. Worker captures contain copied
arguments and shared atomics, never the widget. Closing or destroying the page
requests cancellation without synchronously waiting for file I/O. No vehicle
connection, telemetry command, parameter write or live SITL is required.

## CSV contract

`GLOBAL_TimeMS` is the first column, followed by selected `TYPE_Field` groups
in the reference's first-definition order. MP10 inserts an artificial FMT
schema immediately after the first actual definition when FMT is not already
defined; this affects the unfiltered header but does not fabricate FMT rows.
Every selected complete source record becomes one sparse CSV row in original
order; other types' cells stay empty. FMT records themselves are skipped, while
selected FMTU/UNIT/MULT records remain ordinary export rows. A trailing empty
CSV cell is retained as in MP10. There is no resampling or interpolation.

Time uses `TimeMS`, otherwise `TimeUS / 1000`, otherwise `T`, otherwise zero.
The exporter preserves backward jumps, resets and zero timestamps. Integer
microseconds are divided as exact decimal milliseconds rather than losing
low bits through double conversion at unusually large values. Timestamp fields
must parse as signed 64-bit integers, matching the reference input domain.

Binary scalar conversion follows the wire format: signed/unsigned integer
widths, half/float/double, `c/C/e/E` divided by100 and `L` divided by10^7.
FMTU display multipliers do not rescale these values. Floats use shortest
round-tripping decimals, so exponent spelling can differ from .NET while the
numeric value is retained. `n/N/Z` use ASCII with endpoint NUL trimming; embedded
NULs remain with a warning. `a` is a bracketed, space-separated array of32 signed
16-bit integers. Non-finite values are explicit NaN/Infinity, never replaced by
zero. Selected binary `A` fails explicitly: MP10's object decoder lacks that
legacy encoding. Unselected types still require valid raw framing/schema.

Mode `M` fields use the reference MSG/PARM firmware-detection predicates on the
first100001 such records, independently of the CSV filter. This preliminary
scan also resolves mode records that appear before the firmware message.
Copter, Plane, Rover and Tracker labels come from an immutable snapshot of
MP10's bundled metadata/Common.cs; unknown firmware/modes remain numeric.
User-downloaded MP10 metadata can change labels and is not mirrored dynamically.

Text values are already rendered/scaled and are preserved after trimming.
CSV commas, quotes and newlines are escaped correctly. Unlike MP10's *text*
`GetDFItemFromLine` path, colons do not split fields, empty cells are retained,
and ERR/EV names are not appended beyond the declared schema. Unquoted commas
in a final DataFlash string/array are joined back into that field; ambiguous
non-final string commas fail rather than shifting columns. These defects do
not apply to MP10's binary object path, which already keeps separate values.

## Bounded reading and output safety

The shared `DataFlashRawReader` also backs Split. It reads one record at a time,
with at most256 type definitions, bounded metadata identities and a4MiB text
line limit. It never allocates the old plot parser's whole-log index, adds
synthetic timestamp fields, adjusts clock resets or applies presentation units.
Identical definitions are accepted; conflicting schemas/metadata or unknown
framing fail explicitly. Blank text lines and one known ordinary incomplete
EOF binary record are omitted with counts/type/byte warnings. Truncated metadata
and unknown tails are not silently repaired. Some permissive legacy logs accepted
by MP10 can therefore be refused; the original is never changed in place.

Two raw passes discover schema and stream output, followed by a final source
hash pass. The second pass must match the first schema before indexing CSV
cells, and all source hashes must agree before publication. Same/canonical
input-output paths, leaf output symlinks, missing parents and nonregular outputs
are refused. `QSaveFile` with no direct-write fallback preserves the old output
on observed failure/cancellation and atomically publishes a successful single
CSV. Explicit Save may replace an existing regular output after the picker
asks for overwrite consent. The public worker API assumes confirmed paths.

These are consistency checks, not a lock against hostile concurrent filesystem
changes. Cancellation after the final commit gate cannot retract a published
file, and atomic replacement is not a power-loss durability guarantee.

## Verification

Qt5/audio configure/build, focused5/5 (7.60s) and full **242/242 tests (29.85s)**
pass. Core coverage includes integer widths/scales, half floats, binary strings/
arrays/quoting, explicit non-finite values, raw time priority/resets/fractions,
all four firmware mode families with a later unselected MSG, artificial/late
real FMT headers, selected FMTU/UNIT/MULT sparse rows, duplicate headers, malformed
logs, cancellation, source mutation including schema growth before row indexing,
and path aliases/symlinks. Widget tests cover all three prompts/defaults,
successful/error results, cancellation/close/destruction and operation interlocks.

The production TOOLS route passes twice on X11 (exit0), driving actual
input/type/output pickers and both cancellation stages, then comparing the
100-byte sparse CSV with backward timestamps exactly. The same run verifies
GPS extraction, Split and six exact vehicle actions using an isolated in-process
fixture, not network SITL. No test fixture remains running. The screenshot
`developer-x11-after-export.png` shows the available action and successful export.

An unmodified copy of the5,275,648-byte real ArduPilot BIN produces5809 selected
GPS/ATT/BAT rows,36 columns and672450 bytes. A separately implemented streaming
Python reader compares every selected raw record, sparse cell, timestamp and
scaled/round-trip numeric field, confirms original ordering and checks the
40-byte incomplete final XKF5 warning. The105,000,089-byte synthetic BIN exports
all7,000,000 GPS records into147,000,039 bytes in **23.57s**, maximum RSS
**10,996KiB**. Independent streaming comparison verifies all seven million rows
and four columns with no tail. This measured RSS describes that fixture, not a
universal bound for maximum-width text/schema inputs. Cancellation at25% exits3
with no final CSV or private staging left behind. Originals and SITL were not
modified.

Evidence: `/tmp/apm-dashware.iZuEbc/` (`configure.log`, `build.log`,
`focused.log`, `full.log`, `x11.log`, `x11-second.log`,
`developer-x11-after-export.png`, `real-result.json`, `real-independent.log`,
`large-result.json`, `large-time.log`, `large-independent.log`,
`large-cancel.json`, `verify_dashware.py`). Claude TCP c212/c190–c192 supplied
independent review; root resolved disputed FMT/binary mode details against the
complete MP10 control flow. Three Codex streams supplied backend, UI, runtime
coverage, independent file validation and an exact bundled mode-table audit.
Root reviewed and scheduled all configure/build/test processes.

The manual file-only command is
`dataflash_dashware_probe input output semicolon-types [cancel-percent]`.
It reports success/cancellation/error, warnings and row/column/byte counts as
JSON and never opens a vehicle connection. Native Windows/macOS, reference
pixel matching, high-DPI and an actual DashWare installation remain separate
acceptance gates.
