# Developer Tools — Extract GPS Corrections

Reference: Mission Planner 10
`ViewModels/GCSViews/ConfigurationView/ConfigDeveloperToolsViewModel.cs`,
`ExtractGpsCorrectionsAsync` and `ExtractGpsCorrections`.

The existing action opens a telemetry-log picker, then a save picker suggesting
`<input-name>-corrections.dat`. No vehicle connection is needed. The selected
log is processed off the GUI thread; progress and cancellation remain available.

## Exact data semantics

The output is a raw byte stream: append `data[0..len)` from every
`GPS_INJECT_DATA` and `GPS_RTCM_DATA`, in log order. Keep every sender, duplicate,
fragment and zero-length terminator. Do not add timestamps or a file header,
filter identity, reorder or reassemble RTCM fragments. A zero-message result
successfully creates an empty file, as in MP10, with an explicit count in the UI.

`TlogReader` supplies timestamped MAVLink1/2 records and local CRC checks;
generated decoders restore legitimate MAVLink2 zero-trimmed payload bytes.
Declared data length beyond the message array fails the operation, matching
MP10's bounds failure rather than silently clamping the correction stream.
CRC-invalid records are skipped during reader resynchronization; skipped bytes,
rejected frames and truncated tails are reported. This is extraction, not RTCM
validation, delivery assurance or cryptographic authentication of a signed log.

Current Qt logs contain received packets only (`MAVLinkProtocol.cc` receive
logging). They omit this station's transmitted correction packets. MP10 records
outbound packets in `MAVLinkInterface.generatePacket` through `SaveToTlog`, so
its logs can contain those corrections. The UI names this producer limitation:
zero extracted messages need not mean no corrections were sent. Outbound Qt
logging is a separate transport parity gap and must preserve exact final frame
bytes while excluding secret-bearing `SETUP_SIGNING`; it is not implemented by
this file reader.

## File ownership and intentional differences

The worker retains only copied paths and shared cancellation/progress state,
never a widget or vehicle. It streams bounded records rather than loading the
whole log, and the GUI polls progress instead of queuing one event per packet.
One local extraction/prompt occupies the Developer page's operation gate.

`QSaveFile` stages the result and disables direct-write fallback. Cancellation,
malformed correction lengths, read/write failures or commit errors must not
publish a partial replacement. MP10 can leave a partial file after an exception;
the Qt atomic behavior is an intentional safety improvement. Same/canonical
input-output aliases and symbolic-link output entries are refused. Existing
destinations are selected through the save dialog's overwrite confirmation.

Mixed senders or missing/out-of-order fragments can produce a stream unusable
by a downstream RTCM consumer. The default intentionally matches MP10 instead
of promising to repair it; sender filters and strict reassembly are not part of
this reference action. Signed-log MAC verification is likewise not claimed.

## Acceptance evidence

Qt5/audio build, focused4/4 and full240/240 tests pass (29.70 seconds).
Evidence: `/tmp/apm-gps-extract.jZo3TF/` (`full.log`, `focused-2.log`, `x11.log`,
`developer-gps-x11.png`), also recorded in `CURRENT_STATE.md`.
Core byte fixtures cover both message
types, v1/v2, zero padding, duplicate/order behavior, corruption, cancellation,
atomic replacement and path protection. The production Developer route audit
drives the actual offline file pickers before attaching its vehicle fixture.
Both offscreen and X11 compare7 exact output bytes from two senders, then
exercise the six existing vehicle actions and exit0. The harness types the
visible filename editor and asserts exact selection, including spaces in paths;
`selectFile()` alone had left the visible focused editor empty.
Native Windows/macOS, high-DPI and reference screenshot matching remain release
gates; this action does not complete the other Developer or Settings workflows.
