# Developer Tools — Split DataFlash Log

Reference: Mission Planner 10 `ConfigDeveloperToolsViewModel.SplitDataFlashAsync`
and `ExtLibs/Utilities/DFLogBuffer.cs`, `SplitLog`.

## Operator workflow

The existing Split DataFlash Log action is an offline file tool shared by the
TOOLS Developer window and SETUP Developer page. Select a binary `.bin` or text
`.log`, choose 2–1000 pieces (default10), then review the default-Cancel prompt.
Output names follow `<input-path>_split0.bin`, `_split1.bin`, etc. The added
suffix is lowercase even for an uppercase `.BIN`/`.LOG` input. Text input
produces `.log` pieces instead of MP10's misleading binary suffix. No vehicle,
network connection, mode change or parameter write is needed.

Progress and explicit Cancel stay available while a worker scans and stages
the outputs. The worker owns only copied arguments and shared atomic state;
the widget polls progress rather than queueing one event per record. File
prompts and work share the page's existing exclusion gate with GPS extraction
and vehicle actions. Closing/destruction requests cancellation, not a forced
termination of filesystem I/O.

## Records and metadata

Parts are size-oriented ranges of complete data records, in source order.
Every non-metadata record belongs to exactly one part, including the final
division remainder. FMT/FMTU/UNIT/MULT definitions are prepended to each piece
so records can be interpreted independently. Optional metadata families need
not exist in older logs. PARM, VER, MSG and MODE are ordinary records, not
cloned state: a later piece is not a full parameter/firmware/flight-state
snapshot suitable for every downstream analysis.

Binary framing is read from FMT lengths without resynchronizing or guessing
unknown types, including MP10 float16 (`g`) raw fields. Text is split at complete
line boundaries and retains its actual format; blank lines are ignored with
a count, while string fields can retain embedded commas. Unknown or conflicting
schemas and malformed framing fail explicitly. One EOF-truncated known ordinary
binary record may be omitted with an explicit type/byte warning, as needed for
finalized/power-loss logs. Unknown headers/types or truncated metadata are not
silently discarded. The original file is never repaired in place.
Metadata is capped at16MiB, text lines at4MiB, and piece count at1000; no array
of every source record or whole log buffer is retained. Repeated consistent
metadata records are preserved in the shared prefix, grouped FMT then FMTU,
UNIT and MULT. TimeUS differences alone do not make equal mappings conflict.
A terminal metadata text line gains a newline before it is prefixed to data.
Piece sizes need not be identical because records and
required metadata cannot be cut to fit an exact byte boundary.

## Publication and intentional differences

All requested output paths are checked before processing; an existing file,
directory or symbolic link causes refusal, not overwrite. Every piece is
staged in a private temporary directory beside the input. Source consistency
and completed outputs are checked before publication. Cancellation before the
publication gate removes only private staging files and creates no final output.

Publishing several files is **not group-atomic**. Each final path is checked
again and receives a no-overwrite rename. Cancellation arriving after this
bounded publication phase starts does not interrupt its individual renames.
If a rename fails, already published files remain and their exact paths are
reported; unpublished private staging files are cleaned up. No rollback deletes
operator-visible outputs or overwrites files that appeared concurrently.
This is not a filesystem-wide transaction or a power-loss durability guarantee.

MP10's byte-range loop can omit the division remainder, cut the final record
and duplicate metadata in piece0. Its non-truncating File.OpenWrite can leave
old output tails, and its text picker feeds binary-length copies from text
offsets. Qt intentionally fixes these defects rather than reproducing corrupt
pieces. Refusing existing output names is stricter than MP10; select/copy the
input to another chosen directory if those names already exist.

## Verification

Qt5/audio configure/build, focused4/4 (7.46s), and full **241/241 tests
(29.88s)** pass. Tests cover successful2/3/7/1000-piece splits, whole record
order/remainder, missing/repeated metadata, schema conflicts, float16 raw bytes,
blank text/string commas/final delimiter, known incomplete tail, malformed
metadata and unknown types, metadata limits, existing paths/symlinks/parent
aliases, staged cleanup, input mutation, final cancellation gate and partial
publication collision. Widget coverage includes count/default-Cancel prompts,
result/error, GPS/vehicle exclusion and cancel/close/destruction.

The production Tools route passes both offscreen and X11 (exit0). It drives
the real file picker, count input and both Cancel/Accept paths, then independently
parses both binary pieces, checking complete metadata and all four data records
exactly once. The same audit reruns GPS extraction and the six vehicle actions
using only an isolated in-process fixture. No network SITL state was changed.

Real ArduPilot BIN `/home/alex/SRC/ArduPilot/logs/00000002.BIN` was copied to
an isolated evidence directory, not edited. Its5,275,648 bytes split into seven
parts, explicitly omitting the40-byte incomplete final XKF5 record. Independent
raw-record hashing proves all129,406 complete non-metadata records remain
byte-exact and in order. Independent pymavlink reads every part with zero
BAD_DATA. A separately derived5,275,608-byte complete-prefix fixture also splits
into three byte-exact parts without a tail warning; this is not represented as
an unmodified original log.

The105,000,089-byte synthetic BIN with7,000,000 data records splits into ten
parts in **6.78s**, maximum RSS **10,924KiB**. Independent hashing confirms all
seven million records exactly once and in order. Cancellation at25% returns
exit3 with no published paths, then the uncancelled run succeeds. This measured
RSS is for that fixture, not a universal maximum for16MiB metadata inputs.

Evidence: `/tmp/apm-dataflash-split.JoYPE7/` (`configure.log`, `build.log`,
`focused.log`, `full.log`, `x11.log`, `developer-x11.png`, `real-result.json`,
`real-independent.json`, `pymavlink-real.json`, `real-complete-result.json`,
`large-result.json`, `large-independent.json`, `large-time.log`,
`large-cancel.json`, `verify_split.py`). Claude TCP c211/c185/c186 supplied
independent review; three Codex streams implemented backend, UI and runtime
coverage. Root reviewed and scheduled every configure/build/test.

The manual `dataflash_log_splitter_probe input pieces [cancel-percent]` uses
the same offline backend and reports JSON with
success/cancellation/error, published paths, warnings and byte/record counts.
It never connects to a vehicle. Reference pixel comparison, high-DPI and native
Windows/macOS remain separate gates; the Developer page is now13/32 working,
not complete.
