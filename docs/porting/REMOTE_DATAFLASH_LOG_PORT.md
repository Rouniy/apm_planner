# Remote DataFlash logging port

Status: Qt5/audio configure and final build, focused **6/6**, final full
**254/254 (46.33s)** tests and production X11 audit (exit 0, zero failures)
passed. No complete-flight-log guarantee is claimed.

## Reference and scope

Mission Planner 10 references:

- `ViewModels/GCSViews/ConfigurationView/ConfigDeveloperToolsViewModel.cs`,
  `StartRemoteLogAsync` / `StopRemoteLog`: Developer actions and safe-start check.
- `ExtLibs/ArduPilot/RemoteLog.cs`: `REMOTE_LOG_BLOCK_STATUS` START/STOP,
  `REMOTE_LOG_DATA_BLOCK`, random access at `seqno * 200`, and block ACKs.
- ArduPilot `libraries/AP_Logger/AP_Logger_MAVLink.cpp` and the vendored MAVLink
  `ardupilotmega` definitions resolve firmware routing and session behaviour.

Qt uses the application-owned `RemoteDataFlashLogService`, the dedicated-thread
`RemoteDataFlashLogWriter`, and the existing Developer Tools page. This is remote
live streaming, not the separate onboard log-list/download/erase workflow. It
does not write `LOG_BACKEND_TYPE`, enable logging, erase vehicle logs, or claim
that a transmitted START/STOP completed on the vehicle.

## Wire contract and ownership

START and STOP are `REMOTE_LOG_BLOCK_STATUS` packets with the ACK status and
special sequence numbers 2147483646 and 2147483645 respectively. They have no
protocol acknowledgement. Data packets carry a sequence number and exactly
200 bytes. There is no session nonce, EOF, final-block count or remote flush
acknowledgement.

ArduPilot data arrives from logging component **155**. The receiver matches the
selected vehicle system, physical link/session epoch, expected component, and
the packet's local GCS target identity. Outgoing START, STOP and block statuses
target the selected autopilot component **1**, not component155. This intentional
routing correction avoids sending status to an unrelated logger behind a MAVLink
router; copying the incoming source component blindly is not safe.

The writer opens staging before START. Receiving becomes established only after
sequence 0 is stored. At most 256 distinct pre-zero blocks are tracked; no ACK is
sent for them until zero is stored, after which their stored sequences are ACKed.
Identical retransmissions may be re-ACKed only after disk comparison. A status
of Receiving proves compatible packets were received, not exclusive ownership
or a fresh flight session.

There is no automatic takeover STOP. If sequence 0 never arrives, an existing
client may own the stream, including an earlier instance of this application
using the same GCS system/component identity. The start deadline reports this
uncertainty without issuing an ownership-assuming STOP. Nonempty captured data
is retained as unpublished `.part`. Operators must resolve competing clients
explicitly; reconnecting is not proof that the old remote session ended.

## Admission and active-session safety

Start requires a fresh, exact, disarmed autopilot target, a complete typed
parameter snapshot and an already-enabled MAVLink backend bit:
`LOG_BACKEND_TYPE & 2 != 0`. The confirmed plan freezes the target/instance,
physical epoch, local identity, destination and relevant parameter evidence.
There is no implicit parameter change. Enabled logging backends can participate
in firmware logging and arming checks, and streaming consumes link bandwidth.

The asynchronous default/Escape-Cancel confirmation identifies the target and
destination and requires a dedicated trusted connection. Established recording
may continue when the vehicle becomes armed; fresh target and physical-route
checks still apply to every relevant operation. Closing or changing the
Developer page does not stop an admitted application-owned recording. Reopening
shows the current session and its Stop/Save action. The service also has an
operation-tokened discard API; this page does not expose a separate Discard button.

One-peer UDP is allowed, not blanket-disabled: production uses the existing
peer/revision and physical-epoch checks. A new UDP sender or changed peer retires
the pinned route before further traffic is sent. Serial and eligible TCP/unicast
connections still require operator trust; route guards cannot authenticate an
unsigned sender or detect every transparent downstream router. Broadcast,
fanout and incompatible listener routes are not a substitute for a single-peer
capture.

Production route callbacks interlock this service with `ExactLogTransferService`
in both directions. An active remote capture prevents onboard log list/download/
erase admission, and an active onboard transfer prevents remote-start admission.
Both services publish busy state before calling the validators, so a nested
start cannot bypass the interlock. This is application integration, not a
separate duplicate log downloader or a blanket ban on unrelated telemetry.

The default sequence-zero deadline is 10 seconds, session limit 8 hours, and local
post-STOP capture window 250 ms. A 15-second stream-silence warning is advisory:
`LOG_DISARMED=0` can legitimately produce silence while disarmed. Silence alone
does not discard an otherwise live capture or prove a transport failure. The
short post-STOP window cannot guarantee all delayed packets were received.

## Disk and publication contract

All file opening, seeking, comparison, flushing, renaming and removal belongs to
the writer thread. The facade admits at most 256 unacknowledged blocks of 200 bytes;
input bytes are owned copies. File extent is capped at 512 MiB, forward holes at 4096
blocks per advance, and interval metadata at 65536 disjoint ranges. No whole-file
buffer or unbounded queued-per-block workload is used.

Out-of-order blocks are written at their sequence offsets. A duplicate is read
back and compared with the original block; a conflicting payload is never
allowed to overwrite previously captured bytes. `blockStored` follows write and
`QFile::flush`. This is flushing to the operating system, **not fsync or a
power-loss durability guarantee**. Session generation/epoch fences and guarded
owner-thread notifications prevent callbacks from being adopted by a replacement
capture. Destruction aborts queued work and joins the worker.

The selected directory must already exist. Unique hidden `.part` staging uses
`QTemporaryFile`; no existing file is opened for truncation. Only explicit
Stop/Save can publish `<stem>-<UUID>.bin` through same-directory no-overwrite
rename. A saved capture with holes is explicitly zero-filled and named
`<stem>-<UUID>.partial.bin`. Results report distinct blocks, duplicates, captured
byte extent, highest sequence, missing count and inclusive missing ranges.
Extent includes holes; it is not the sum of valid payload bytes. Even a gap-free
`.bin` is a local captured prefix, not proof of a complete remote flight log.

Unexpected link/lifetime/protocol/queue/I/O failure and application shutdown
preserve nonempty staging as `.part`, with a recovery path and warnings but
`published=false`. Empty failures are cleaned up. Explicit Discard removes its
staging instead. A failed final rename preserves both any preexisting destination
and the captured staging bytes. An I/O failure can leave a partial final block.
Once a previously authorized final rename begins, concurrent cancellation cannot
retroactively revoke publication.

No recovery sidecar is written in this slice: exact missing ranges are returned
in the terminal report, not persisted separately. After application shutdown a
standalone `.part` therefore does not carry authoritative gap metadata. Preserving
it is a recovery measure, not a claim that it is ready for ordinary log analysis.

## Verification and remaining limits

Qt5/audio configure and final build passed. Focused tests passed 6/6 and the final
full suite passed 254/254 (46.33s). Coverage includes writer ordering/duplicates/gaps/caps,
preservation versus discard, no-overwrite publication, callback deletion and
generation reuse; service source/status routing, start/cancel/stop, deadlines
and the two-way classic-log interlock. The pending-block deduplication regression
includes 400 retransmissions without overflowing the writer queue (W1 fixed).

The production X11 audit exited 0 with zero failures. It exercised the actual
Developer prompts, source filtering, out-of-order blocks, a genuine lost-wire-ACK
retransmission/re-ACK, page destruction/recreation without stopping the session,
and Stop/Save while armed. The coordinator inspected the readable Stop consent
at `/tmp/apm-remote-dataflash.N5trJR/remote-stop-final.png`. The final X11 repeat
also exited0 with zero failures and byte-identical output. The saved capture was exactly
600 bytes, SHA-256
`7d40d47c29fef6f510d0c879be2c3ad96d8ee21182d72d3cd310d9308f134f28`.
Claude TCP reviews c240/c241/c242 returned REVIEW-OK. This evidence uses the
in-process vehicle fixture; it is not a real-vehicle or network-SITL claim.

The final rebuild and full suite include three review-driven wording corrections
for local-only shutdown and unconfirmed earlier/competing-session recovery bytes.

Functional limits: no session nonce or authenticated ownership proof in this
protocol; no automatic takeover; no EOF/remote flush proof; no automatic parameter
enablement; bounded disk/session limits; no resume/import of `.part`; no persisted
gap sidecar; filesystem flush is not power-loss durability. More specifically:

- One locally rejected block-ACK submission ends the capture fail-closed. This
  differs from an ACK lost after successful wire submission: a retransmitted
  identical block is verified and re-ACKed in that case.
- The 250 ms post-STOP drain cannot establish EOF or the size of an unknown tail.
- A conflicting duplicate already on disk currently reports `IoError`, whereas
  a conflict with a still-pending block reports `ProtocolError`; both stop and
  retain the recovery capture without overwriting its original bytes.
- Application shutdown stops local capture and preserves nonempty `.part`; it
  does not submit a remote STOP or claim the vehicle logger stopped.
- The last report/history is in memory. Missing ranges have no durable sidecar;
  recovering a standalone `.part` after restart needs separate inspection.

Hardware, broader packet-loss and native cross-platform validation remain
separate acceptance work.

GUI differences: Qt presents asynchronous named confirmations, live status,
owned Stop/Save controls and application-owned history on the existing
Developer page. Wording and layout intentionally expose uncertainty rather than
copying MP10's unconditional started/stopped text. Pixel-level parity is separate
from functional completeness and is not claimed by this implementation note.
