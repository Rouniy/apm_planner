# Download Logs vertical slice — 2026-09-05

The production `TOOLS → Download Logs (MAVLink)` route now constructs
`LogDownloadWindow` and its window-owned `LogDownloadViewModel`. The previous
`uas/LogDownloadDialog` sources are retained, but MainWindow no longer opens them.

## Reference and behavior

Read-only MP10 references: `Views/LogDownloadWindow.axaml`,
`ViewModels/LogDownloadViewModel.cs`, `ExtLibs/ArduPilot/Mavlink/LogDownloadTracker.cs`,
`MAVLinkInterface.cs` GetLog/GetLogEntry and `Services/DataFlashLog.cs`.

- Modeless 540×440 window, minimum 420×320, wrapping six-control toolbar,
  single-selection Id/Date/Size grid and aggregate progress/status.
- Refresh includes all terminal list entries internally and hides size-zero rows.
  Selected suggests `log_ID.bin`; All uses local dated names with ID, or
  `log_ID.bin` without a timestamp. File and KML collisions require approval.
- One application-owned `ExactLogTransferService` owns each LOG operation.
  A window cancels only its token. List rows retain the exact vehicle instance;
  picker/batch transitions reject another vehicle instead of reusing old IDs.
- Each request/reply is scoped to link, physical session, system, component and
  vehicle instance. Serial/TCP/UDP client remain normal single-vehicle routes;
  listening UDP additionally requires exactly one peer. This does not relax
  the stricter Swarm route policy.
- List timeout is 5 s with four retries. Download starts with the streaming
  request, infers the actual end conservatively, then repairs one missing range
  of at most 50×90 bytes. Silence windows are 3 s/500 ms with a 30 s budget;
  duplicates do not reset progress. Advertised size is an estimate, not EOF.
- Coverage has a 4096-range cap. Far out-of-order bytes use a bounded deferred
  map, preventing an unsolicited far offset from immediately causing a huge
  sparse write beyond the advertised/frontier window. The deferred cap is
  368640 bytes (per-byte map overhead is larger and remains a performance note).
- QSaveFile replaces the destination only after complete durable coverage.
  Cancel, timeout, route loss and errors discard temporary output. END is
  attempted before another operation can acquire the protocol slot.
- Erase sends two exact LOG_ERASE frames only after default-Cancel confirmation.
  MAVLink supplies no acknowledgement, so the UI reports submission without
  claiming successful erasure and asks for Refresh List.
- Create KML runs the existing streaming DataFlash parsers on a joined worker,
  exports valid GPS fixes in log order, preserves format scaling, rejects
  non-finite coordinates and writes atomically. Cancellation is cooperative;
  the worker never captures the window or view model. Empty tracks are valid.
  Exporter-private parser subclasses preset TimeUS so the first/only FMT is
  handled immediately; the general log-analysis parsers are not modified.
  Input and output may not refer to the same source file.
- Heap-owned guarded file/confirmation dialogs tolerate parent destruction
  inside a nested event loop. The window disconnects model/table receivers
  before QWidget destroys children; shutdown joins KML before service teardown.

## UDP/lifetime corrections required by this slice

`UdpPeerState` owns synchronized peer snapshots and monotonic revisions. Learned
senders are recorded before stamped ingress is emitted. LinkManager rejects old
queued datagrams, retires the old epoch before associating the new revision, and
activates a listening UDP session only when a peer sends its first datagram.
MAVLinkProtocol rechecks the physical revision between observers and frames.

Production writes enqueue only for the revision associated with the current
GUI-side epoch. Queued envelopes retain their original destinations and are
dropped when stale at dequeue; they are never redirected to a newly learned peer.
An already-dequeued/syscall-bound datagram may still reach its original peer;
this is submission isolation, not a delivery acknowledgement. Peer changes are
not an authentication mechanism.

The runtime audit also exposed an existing queued `disconnected(LinkInterface*)`
use-after-free after `removeLink()` joined and deleted the worker. Factory
ingress/lifecycle lambdas now capture QPointer and check map membership before
calling manager/protocol slots. Settings serialization takes one peer snapshot.

## Verification

Full Qt 5 build with required Qt speech/audio and **206/206 CTest targets pass**.
Six new targets cover tracker, peer state, exact service, KML, view model and
window. The existing production route audit now also runs real Factory-created
UDP links: stale queued ingress, reentrant two-frame parsing, peer epochs,
outbound delivery, LOG_REQUEST_LIST/LOG_ENTRY and active-operation retirement.
Tests include pre-commit fingerprint mutation, atomic old-file preservation,
deferred far-data pruning, synchronous terminal replies and modal-owner deletion.

Real X11 evidence is in `/tmp/apm-download-logs.FmmrKU/`:

- `tools.png`, `log-window.png`, `list.png`: production menu and nonempty surface.
- `download-complete.png`, `all-complete.png`: actual Selected and All downloads.
  The local UDP fixture deliberately omits offset 90; the peer log proves the
  missing 90-byte repair was requested before completion.
- 9689-byte log 7 SHA-256:
  `c0c45a0a4084f4006fb101e29a8bfd25c971c01dc20bfa24e40ee34e9d106a5c`.
  96089-byte log 8 SHA-256:
  `fff0c08fef3ba6cd06698f2f78e16ae64d479443e5f2201a6450cb6a8bec14b4`.
  Selected/All match the fixture; parsed KML contains 400 and 4000 coordinates.
- `cancelled.png`: Cancel leaves the window open and preserves the previously
  existing `cancel.bin` marker. `second-window-busy.png`: independent window
  cannot acquire/cancel another window's protocol operation.
- `erase-confirmation.png`, `erase-submitted.png`, `empty-after-erase.png`:
  two erase frames sent only to the synthetic localhost peer, honest status and
  subsequent empty list. No physical vehicle was erased or commanded.
- `app.log`: ordinary main-window close with the tool open, process exit 0.
  The local fixture process was stopped after the run.

Physical flight-controller/serial/TCP transfers, real onboard binary-log exports,
native Windows/macOS and reference screenshot comparison remain separate gates.
No SFTP or full modern MAVLink dialect parity is implied. The UI disables a
second window while the shared service is busy; a richer owner/busy explanation
and final toolbar/status typography remain polish work.
