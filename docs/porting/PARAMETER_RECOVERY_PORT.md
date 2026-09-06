# Restore Parameters (Recovery)

This port fills the existing Developer `RestoreParametersButton` and
`CancelParameterRestoreButton`; it does not add a Setup page or duplicate the
Full Parameter List editor. The service belongs to the application and shares
the exact parameter and command services, not a widget-owned transport.

## Reference

Mission Planner 10's `Services/ParameterRecoveryWorkflow.cs` and
`ConfigDeveloperToolsViewModel.cs:421` define the recovery workflow:

1. Parse a `.param`/`.parm` file and confirm the selected disarmed vehicle.
2. Prefetch the file's parameter names.
3. Write names containing `ENABLE` first, in file order.
4. Process all names in file order. Skip unchanged values; otherwise read the
   parameter, reset names ending `_ID` to zero, then write the desired value.
5. Report set/unchanged/failed counts; cancel or target changes stop later work
   without rolling back earlier writes.

The exact 15-name `ParamFile` exclusion list is retained. `SYSID_THISMAV`, serial
settings and additional mission/statistics names are **not silently excluded**.
Changing identity or the active transport may stop the restore; consent warns
about this. The `_ID` suffix rule is retained, so `COMPASS_DEV_ID2` does not get a
zero-reset merely because `COMPASS_DEV_ID` does. No reboot is sent automatically.

The common QtCore `ConfigRawParamsFileCodec` was extracted from the Full Parameter
List implementation. Its existing map API, uppercase-name normalization,
exclusions, separators and save format remain compatible. The added ordered API
preserves the first insertion position and last duplicate value, as required by
the recovery pass. Malformed/nonfinite values refuse the whole file; the legacy
codec's 4096-character/100000-line bounds remain. Recovery additionally limits
the file to 1MiB and 10000 entries and validates MAVLink parameter names.
Uppercase normalization is an explicit difference from MP10's verbatim names.
The byte limit is applied to an immutable buffered read, not only a pre-read
file-size check, so a growing source cannot turn parsing into an unbounded read.

## Exact execution and truthful outcomes

Preparation freezes the parsed values, source path, target generation and
vehicle instance. Confirmation does not reopen the source file or retarget the
plan. The two asynchronous dialogs are
`DeveloperParameterRecoveryFileDialog` and
`DeveloperParameterRecoveryConfirmation`; the latter shows the source, target,
count and ENABLE/reset/no-rollback warning, with default/Escape Cancel.
`DeveloperParameterRecoveryProgressDialog` and the existing Cancel action
operate on the admitted operation ID, not whichever job happens to be current
after a callback.

The service holds both command and parameter endpoint reservations for the
whole sequence. Exact reads establish runtime types and current values; a
missing warm-up read does not prevent retry after ENABLE exposes a dynamic
parameter table. There is no requirement for a complete pre-existing parameter
snapshot and no guessed REAL32 type. Normal ArduPilot disabled groups may still
answer by-name reads; the dynamic-module fixture is not a claim that every
disabled group is unreadable.

Desired ID values must be representable using the endpoint's actual encoding
**before** resetting the old value to zero. The same check at reset-write safety
gates protects against an intervening encoding change. C-style float roundtrip
loss is refused, while valid bytewise UInt32 IDs are preserved. Writes retain
the shared per-attempt exact-target/disarmed/freshness gates and retry policy.

MP10 accepts any same-name parameter echo as write success. Qt requires the
shared same-type/same-value acknowledgement. An echo of the old value is not
treated as proof of denial: it could be delayed telemetry from an earlier read.
An unacknowledged attempted write remains uncertain, stops subsequent recovery
writes, and uses the shared bounded isolation fence. Unsupported/missing reads
and incompatible values are reported; successful ENABLE writes and ID resets
have separate receipts even when the main pass never completes.

Cancellation cannot undo a transmitted write. The shared ParameterService now
tracks whether the writer was invoked across synchronous writer/notification
callbacks, so cancellation in that window cannot falsely report “not sent”.
The per-attempt flag is independently owned through the transmitter callback;
signing rejection before the writer remains a non-transmission outcome.

Nested event loops can deliver terminal reports before an exact submit call
returns. The shared parameter service publishes its operation token before
transmission; recovery pins that stack-local token during submission and
buffers only its matching operation/reservation/kind/name/vehicle report.
Foreign reports cannot displace the real acknowledgement or leave the restore
busy indefinitely. An independent Claude review identified the original
four-report-cap defect; the regression injects six foreign reports before the
real nested response.

Page closure invalidates outstanding consent and requests cancellation of its
own active restore. Terminal results remain in the application service and can
be shown on reopening. There is no rollback, durable journal across app exit,
or guarantee against arbitrarily delayed indistinguishable PARAM_VALUE packets.
Service destruction clears page ownership without touching dialogs or text
layout after QApplication teardown. The production route initially reproduced
a font/text-layout shutdown crash in that callback; the final runtime gate
includes clean process exit, not merely successful in-window assertions.
The complete workflow has a 30-minute deadline; service test seams shorten it
without changing production defaults.
This deadline is checked against a monotonic clock at execution gates; the
timer is only a wakeup, not permission to write after a delayed timer event.
Progress counts the main pass. During prefetch/ENABLE, the status names the
current read/write; the main-pass bar has not advanced yet. Sequential exact
prefetch adds latency, especially for missing names, and the main pass rereads
values because ENABLE can change both values and available schema. Incremental
history delivery and batched read-only prefetch are future performance work.

## Verification

Tests and the production X11 route use isolated in-process fixtures, not the
network SITL or physical vehicles. The runtime file deliberately puts a dynamic
`RECOVERY_GAIN` before `RECOVERY_ENABLE`, then verifies ENABLE1 → GAIN12.5 →
COMPASS_DEV_ID0 → COMPASS_DEV_ID202 and separate reset/enable receipts. Actual
file/confirmation cancellation sends nothing. A second fixture cancels a write
whose echo is withheld and requires an uncertain result with no extra writes.
The source files remain unchanged.

Verified Qt5/audio configure/build, full249/249 CTests (33.24s), and production
X11 exit0 with zero audit failures. Root inspected the actual readable consent
at `/tmp/apm-parameter-recovery.N3ZG4M/recovery-consent.png`; logs and the initial
shutdown GDB backtrace are in the same directory. See `CURRENT_STATE.md` for
the final checkpoint and corrected initial fixture/build failures.
Physical parameter restoration, all firmware families,
Windows/macOS packaging, reference screenshot parity, long-lived durable job
history and PARAM_ERROR dialect support remain separate gates.

Claude TCP c222 supplied a 12556-byte independent reference audit, SHA256
`abf6fbbb589413a96bd14b201711e3a984588158d1947da466bd10687241ab2e`.
Root retained the reference exclusions/suffix rule and rejected the suggested
extra exclusions and old-value-echo denial heuristic for the reasons above.
