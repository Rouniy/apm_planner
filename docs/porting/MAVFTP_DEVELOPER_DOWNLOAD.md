# Developer Tools — Download MAVFTP File

Reference: Mission Planner 10
`ConfigDeveloperToolsViewModel.DownloadMavFtpFileAsync`, lines386–419.

## Workflow

The existing Developer button implements the reference's direct-file workflow:
enter a remote path (default `@SYS/threads.txt`), select the local Save filename,
then download with modeless progress and Cancel. It does not require that the
remote directory can be listed. The suggested filename is the final remote
path segment, normalizing backslashes only for that suggestion. The actual
remote path is sent unchanged after protocol validation. The Save picker asks
before replacing an existing local file.

The controller is injected into the shared TOOLS/SETUP Developer page. It uses
the existing application-owned MAVFTP service; there is no second protocol
client and no new item in the fixed24-item TOOLS menu. The full MAVFTP browser
remains available in SETUP and CONFIG. Its display-profile visibility does not
restrict this separate Developer action, matching MP10. Disconnected activation
reports the missing stable target without opening an empty widget or sending
anything. Page controls share one exclusion gate with GPS extraction, Split,
DashWare and guarded vehicle tools.

One exact `(link, system, component, target-generation)` lease is captured
before either prompt, checked between prompts and repeated at service admission.
The controller never silently switches to the newly selected drone. Changing
targets cancels the pending prompts/owned transfer; no armed/disarmed restriction
is added to this file-read operation. Existing transport/current-target checks
remain authoritative; parameters do not need to finish loading first.

## Shared ownership and lifetime

Every admitted service operation has a nonzero ID published to the caller's
out parameter **before any synchronous signal or wire callback**. Results retain
this ID after service state is cleared, and the new progress signal includes it.
Cancellation accepts an ID and refuses zero or a different current operation.
Both Developer direct download and the existing browser use these identities,
including repeated requests for the same path and target generation. An old
completion or Cancel cannot be attributed to a newly admitted operation merely
because its path and drone match.

The browser uses a generic owned admission method for its six operation kinds;
the Developer controller uses expected-target download admission. Existing
service entry points remain available to other callers, and ownership-aware
interface defaults fail closed. Admission and progress callbacks guard deletion
and replacement. Best-effort cleanup stops sending an old ResetSessions when a
new operation starts during the old TerminateSession callback.

The Developer controller is page-owned. Close/destruction cancels only its own
remote operation or private local-save worker; it never invokes global Cancel
on an unrelated browser transaction. Result handling tolerates synchronous
completion, delayed cancellation, page/service destruction and callback-driven
restart. A30-second controller deadline matches MP10's transfer timeout and is
separate from the service's bounded per-request retry/cleanup timeouts. Local
publication is not subject to that remote deadline.

## Output and known limits

Only successful, matching, uncancelled terminal bytes can enter local saving.
The local writer runs on a worker with copied data/output snapshot and shared
atomic cancellation, never a widget or live vehicle object. `QSaveFile` with
direct fallback disabled stages and atomically replaces the explicitly chosen
output. Output directories must resolve, leaf symlinks/nonregular outputs are
refused, and canonical-parent plus existence/size/mtime snapshots are checked
before writing and publication. This is stricter than MP10's direct overwrite.
The browser retains its separate collision-avoiding filename policy.

These checks are not a hostile-filesystem lock: same-size/same-mtime changes
and races after the final check are not a filesystem-wide transaction. A
cancellation arriving after the final commit gate cannot retract the file, and
atomic replacement is not a power-loss durability guarantee.

The existing service buffers at most64MiB and reads sequential80-byte chunks;
larger files fail explicitly. Streaming/burst transfer is not implemented, so
slow/large downloads may reach the30-second deadline. The reported remote size
and matched MAVLink responses are checked, but downloads do **not** perform a
whole-file remote CRC comparison. Success means bytes were written locally, not
an independently verified or authenticated remote file. Upload CRC behavior is
unchanged. FTP reset/terminate messages manage the remote FTP session; this is
not a zero-traffic local tool.

The follow-up browser slice now captures its own exact target/path/type before
asynchronous prompts and clears stale listings, closing the previously recorded
prompt-time target-switch gap. See `MAVFTP_BROWSER_TARGET_CONSENT.md` for its
separate tests and boundaries. Native platforms, capability gating, actual
hardware servers and MP10 screenshot parity remain open.

## Verification

Qt5/audio configure and full build pass. The six focused service/protocol/browser/
controller/Developer/runtime tests pass in7.75s; the full suite is243/243 in29.82s.
Controller tests cover prompt cancellation, exact target/operation filtering,
inline completion and restart/deletion, timeout and delayed cancellation, late
success after cancellation, page/service/target destruction, output preservation,
symlink refusal and changed-parent rejection. Browser/service tests cover old
same-path/same-generation results, foreign cancellation and callback replacement.

The real production TOOLS route also passes on X11 (exit0, zero audit failures).
Its actual path and Save pickers cancel without FTP traffic, then download127
bytes including NUL/high bytes through an isolated in-process sys234 endpoint.
The fixture checks exact GCS/target addressing, rejects wrong sender/destination,
and exercises Reset/Open/80-byte Read/47-byte Read/Terminate/Reset. The local file
is compared byte-for-byte. GPS/Split/DashWare and all six vehicle actions still
pass in the same runtime audit. The screenshot shows the non-empty Developer
page with its enabled Download MAVFTP File button.

Evidence: `/tmp/apm-mavftp-download.bQ9IqO/` contains `configure.log`, `build.log`,
`focused.log`, `full.log`, `x11.log` and `developer-x11.png`. Claude TCP c214/c196
provided independent review; three Codex streams implemented service ownership,
controller/tests and runtime coverage. Root reviewed and scheduled verification.
No network SITL state was changed; all runtime fixtures stopped. Native platforms
and physical-server behavior are not established by these tests. The broader
Mission Planner port remains incomplete.
