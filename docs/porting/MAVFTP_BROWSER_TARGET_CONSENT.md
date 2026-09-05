# MAVFTP browser — target-bound consent

## Defect and scope

The existing shared SETUP/CONFIG browser used static modal file/confirmation
dialogs and admitted an operation against the selected target only after those
dialogs returned. The download path and delete file/directory type were also
read from live selection after the prompt. A target/selection change during the
nested event loop could therefore change the operation the user had approved.
Cached directory rows also outlived an idle target change. This is a safety
defect in the Qt browser, not a new item in the MP10 inventory.

The Developer direct-file workflow already pins its target before prompts.
Its implementation and the shared operation-ID boundary are documented in
`MAVFTP_DEVELOPER_DOWNLOAD.md`. This follow-up applies the target-consent rule
to the full browser, preserving its remote tree/table, lazy listing, download,
upload, delete, mkdir, upload CRC and local collision-avoiding filenames.

## Implemented contract

- Capture a settled exact `(physical link, system, component, generation)` lease
  and immutable remote path/name/type before opening any prompt. Changing away
  and back invalidates that lease even when the MAV IDs are identical.
- Reserve the browser's local operation state while prompting. UI selection
  changes cannot silently rewrite the pending operation. Upload/delete consent
  names the intended target and remote path; Cancel is the default.
- Use page-owned nonblocking dialogs with guarded callbacks. Recheck the lease
  between prompts and at service admission; a stale lease sends no request.
- Clear old listings when the selected target changes. Metadata-only updates
  must not clear a current listing. Do not refresh a new target from an old
  operation's queued completion callback.
- Tag cached directories and rows with the lease that produced them and reject
  a different current lease at click time, even if a notification was missed.
  The literal root remains usable without first requiring a directory listing.
- Keep operation-ID publication before any callback, scoped progress/results,
  and cancellation of only the browser's own operation. Closing/destroying a
  prompt or page must not cancel another consumer's transaction.
- The service exposes a current settled lease and expected-target admission
  for all six operation kinds. Interface defaults fail closed; no consumer may
  fall back silently to target-unbound admission.

Upload bytes are captured when the local file is picked and retained for the
subsequent exact-path/target confirmation. The existing bounded local read still
runs on the GUI thread; moving it to a worker remains a separate responsiveness
improvement. The remote service/protocol implementation is shared, not duplicated.

Browser-owned progress values are set with the progress bar's signals blocked,
then the changed value is published after Qt's setter has returned. This avoids
Qt5 repainting a progress bar deleted by a valueChanged listener. Callers still
check page lifetime and operation revision after publication. The regression was
reproduced under gdb before the fix and its regression now passes.

## Unchanged boundaries

This work adds no new menu items, SETUP/CONFIG routes or Developer action count.
It does not add remote rollback, streaming or burst transfer, capability gating,
download whole-file CRC, an exclusive local no-replace primitive, or protection
against hostile filesystem races. Transfers still use the existing64MiB buffer
limit and sequential80-byte reads. Upload/delete can change remote data only
after explicit consent; successful cancellation cannot undo an already executed
remote mutation. Native Windows/macOS, physical FTP servers and MP10 screenshot
comparison remain separate acceptance work.

Explicit service shutdown retains the existing no-terminal-result contract.
The browser receives target invalidation and retires its local flow; the separate
Developer controller finishes on owner/service destruction or its bounded remote
deadline. Normal application teardown destroys pages before stopping the shared
service. This is not a new promise that every public shutdown call emits a
cancelled operation result.

SETUP can destroy and recreate connection-owned pages on a component change.
The production audit must therefore tolerate either a cleared surviving page or
an already-destroyed page, reopen the real route and verify an empty new listing.
Any later Developer checks must reacquire their page too, not reuse a dangling
pointer from before the switch.

## Verification

Qt5/audio configure/build and the six focused tests pass (8.10s), including the
production Developer/Setup route audit. Initial compilation exposed two harness
errors (missing tree variable, Qt5 protected QFileDialog::accept), fixed before
the successful build. Initial tests exposed stale button state after retiring a
prompt and the progress-setter deletion crash; both now have passing regressions.
The full suite passes243/243 in30.20s. Production X11 also passes (exit0, zero
audit failures): actual SETUP browser route, file/directory listing with EOF
pagination, default-Cancel delete, actual upload file picker and consent, then
component1→2 changes during both Upload and Delete confirmation. The fixture
records zero destructive FTP requests. Switching back requires a new listing;
the audit reopens pages destroyed by SETUP's reset and requires explicit Refresh.
The same run retains the direct127-byte download, GPS/Split/DashWare and all six
guarded vehicle-action checks.

Evidence: `/tmp/apm-mavftp-consent.AgH20W/`: `configure.log`, successful
`build-3.log`, `focused-2.log`, `full.log`, `x11.log`, and `browser-gdb.log` for
the original crash. `browser-x11.png` captures the preceding Developer phase;
it is not a browser-dialog or reference screenshot comparison. Claude TCP
c197/c199/c201–c203 supplied independent design/service/page/final-fix reviews.
Three Codex streams implemented the service, browser/tests and runtime fixture;
root reviewed and ran verification. No network SITL state was changed and no
test fixture remains running. Counts remain Developer15/32, SETUP46 and CONFIG15.
