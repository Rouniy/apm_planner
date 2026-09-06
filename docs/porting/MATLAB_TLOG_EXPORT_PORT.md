# Tlog Convert: MATLAB export

## Scope and reference

The existing Tools > Tlog Convert / Extract route retains its independent,
modeless 460x340 window. Its Matlab button exports the selected TLOG through a
real Level-5 MAT writer. This does **not** implement the separate DataFlash
BIN/LOG `MatLab.ProcessLog` workflow or the missing DATA log tabs.

Primary reference: MP10 `ViewModels/MavlinkLogConvertViewModel.cs`,
`ExtLibs/Utilities/MatLab.cs` (`tlog`, `GetMatLabSerialDate`) and
`ExtLibs/Mavlink/MavlinkParse.cs`. Container format follows the
[MathWorks Level-5 specification](https://www.mathworks.com/help/pdf_doc/matlab/matfile_format.pdf).

Each numeric scalar becomes a double matrix named
`<field>_mavlink_<lowercase-message>_t`, with N rows and two columns: local
serial date and value. Input order and all systems/components are retained;
only GCS HEARTBEAT records are excluded. Arrays, including text and signing
secret keys, are omitted. MAVLink 2 omitted extension bytes read as zero.
Integer64-to-double precision loss and nonfinite values follow the reference.
Variables are sorted deterministically; MP10 Hashtable ordering is not retained.

Timestamp prefixes are unsigned big-endian UNIX microseconds, truncated to
milliseconds and converted to local wall-clock time. MP10 then adds one
calendar year and two days before subtracting DateTime.MinValue. This is **not
standard MATLAB datenum**: some dates gain an extra day across a leap year.
The port deliberately preserves that compatibility quirk, including the invalid
prefix fallback367, and performs the integer .NET-tick conversion before the
single double division. Both applications must use the same timezone for parity.

## Operation and limits

The window freezes the selected input before its default-Cancel sensitive-data
confirmation and Save As picker. Matlab suggests the full input name plus
`.mat`; unlike MP10, existing outputs are refused, including races at publication.
Every format now has visible worker progress and cancellation. Close cancels
and waits for its owned worker; callbacks cannot select another input for an
already admitted export.

Two decoding passes count and fill matrices. SHA256 checks before, between and
after the passes detect normal source changes. This makes five full input reads;
large files on slow storage can take substantially longer. Buffered scalar data is capped
at8MiB; input16GiB, variables4096, name127bytes, total output64GiB. Each Level-5
matrix is bounded by its unsigned32-bit element size, with int32 dimensions.
The writer tracks filled rows per column; NaN is legitimate data, not a missing
cell sentinel. Long reference names are kept intact, not silently truncated.

A private same-directory temporary file is flushed and published atomically
without replacing an existing file (Unix hard link; Windows non-replacing move).
Cancellation/error removes the owned stage, not prior completed outputs.
Path/size/time/birth checks plus hashes are mutation guards, **not** filesystem
locks or a guarantee against a hostile same-user writer exchanging paths between
checks. Network/native filesystem behavior remains a release validation gate.

The shared bundled MAVLink dialect predates MP10. Offline metadata supplements
decode24 actual numeric extension fields across10 shared messages without
altering live transport. Unknown messages and corrupt
frames are omitted by the existing reader; skipped bytes are reported. Damaged
log resynchronization need not produce the same salvage set as MP10. Offline
signed-frame decoding does not authenticate signatures. MAT names longer than
a consumer's identifier limit may require adaptation by that consumer.

The compiled `MAVLINK_MESSAGE_INFO` macro has276 messages, versus350 in MP10,
all276 shared by ID/CRC-extra;74 reference message types remain absent. Merely
scanning header directories incorrectly counts STATUSTEXT_LONG, whose file is
not included. This was caught during root review of Claude's inventory.
Notable absent types include MISSION_CHECKSUM, AIRSPEED, RELAY_STATUS, camera
tracking, gimbal-manager and AVAILABLE_MODES/CURRENT_MODE. Their CRC/schema
support requires a separate shared-dialect update, not fabricated export rows.
The complete ID groups from the corrected source audit are:

- 53,271,275–277,280–282,287–288,295,332–333,345,376,420,435–437,441;
- 220–224,8002–8016,11060,17000,17150,17151,17153–17158;
- 26900,50001–50005,52000–52001,60000,60010–60014,60020,60040–60041,
  60045–60047,60050–60053.

These are message IDs. See the source-audit report c309 for per-message names.

## Verification

Evidence: `/tmp/apm-matlab-port.98lQAK/`. Qt5/audio configure and build5 pass.
The first build caught only a Qt5 QStringList initializer ambiguity in the new
widget test; its explicit type fixes compilation. Full293 tests pass in52.13s;
ten repeats of writer/exporter/previous exports/window pass in3.51s. Production X11 reports zero
failures; root inspected both the actual window and default-Cancel consent.
The whole window test suite passes13 cases on X11. The production route test
opens the real Tools action, selects a file, cancels consent, writes20k records,
cancels a second export without output, closes and reopens independently.

Actual MP10 bundled .NET10 `MatLab.tlog` is invoked via a small FSI reflection
oracle with a sibling assembly resolver (no source/service reimplementation).
SciPy1.16.1 reads both outputs and compares names, dimensions and every float64
bit, including signed zero. Results:283 variables/32565 packets from a saved
real TLOG, and160 variables/34 packets in the extended fixture, all equal.
The latter covers all24 new extension fields in10 message types, including
nonzero and omitted raw payload bytes, long75-character names, unsigned
64-bit rounding and array omission, in UTC and Asia/Nicosia. Earlier runs
exposed the missing three MISSION_CURRENT and one gimbal extension field; the
isolated metadata supplements and zero-trim/nonzero-wire tests fix them.
The real input was read-only; no network SITL or physical vehicle was modified.

One million ATTITUDE records plus four fixture records produce112MB of MAT
output in11.71s, peak RSS14040KiB (Debug build, local filesystem). This measures
bounded memory, not a performance guarantee at the16GiB input bound.
The fixture CLI is `tlog_matlab_probe --fixture NEW.tlog [RECORD_COUNT]`, followed
by `TZ=UTC tlog_matlab_probe NEW.tlog NEW.mat`. Fixture extensions added after
the stress run add18 more records; its ATTITUDE load is unchanged.

Reusable `tests/oracles/TlogMatlabOracle.fsx` takes the MP10 build directory,
a separate output directory and input TLOG paths; it copies inputs before
invoking the real reference exporter. `tests/oracles/CompareMatlabFiles.py`
takes reference and Qt MAT paths with NumPy/SciPy installed. Both applications
must run with the same TZ; Python returns failure on any shape/name/bit mismatch.

Claude TCP c305/c307/c308/c309/c311 and three Codex streams provided independent reviews.
Final c311 independently checked all24 fields, constexpr lifetime/guards and
raw-byte test fixtures without finding another concrete defect.
The c307 claim of368 for DateTime.MinValue was disproved by the actual oracle
(367); its NaN-sentinel proposal was rejected because NaN telemetry is valid.
No broad format or native-platform completeness claim follows from route counts.
SciPy and .NET are verification-only dependencies, not application dependencies.
