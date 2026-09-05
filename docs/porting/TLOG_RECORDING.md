# Telemetry log recording — RX and typed TX

Checkpoint: 2026-09-06. Mission Planner 10 records outbound frames through
`MAVLinkInterface.generatePacket` / `SaveToTlog`. Qt now records both accepted
incoming MAVLink frames and successfully submitted typed outgoing frames while
the recorder is active. Older RX-only files cannot gain missing packets.

## Boundaries and file ownership

`LinkManager::writeSequencedFrame` captures the active logging-session token
before calling the physical writer. Only a successful return with the same
manager, link object and physical epoch can append the final wire bytes,
including MAVLink2 signatures. Public submitted observers run afterwards.
A transport callback that replaces the file cannot transfer an already admitted
frame into the replacement session. Raw passthrough, mirror write-back and
bootloader bytes are not typed TX and do not enter this logger.

RX records the original accepted frame after authentication and secret
exclusion, before negotiation and presentation callbacks. A local immutable
frame snapshot prevents nested receives from replacing those bytes. Rejected
MACs, replay, unsigned protected vehicle traffic and diagnostic-only unsigned
radio do not enter the file. The last category intentionally differs from
MP10 radio history and is still available through RadioStatusMonitor.

SETUP_SIGNING bypasses generic TX parsing/observation/logging entirely; RX and
the private logger also refuse it. No redacted secret-frame substitute is
written. Retained records are big-endian uint64 wall-clock microseconds followed
by one complete MAVLink1/2 frame. Timestamp plus frame use one QFile write;
file order is preserved, but wall-clock corrections can make timestamps go
backwards. There is no direction flag in this standard TLOG format.

Each successful file open allocates a non-reused session id. Stop retires the
id before closing. Failed open/write retires the failed state before emitting
diagnostics, so a callback can start a valid replacement without the old attempt
disabling it. A failed log write does not change successful transport submission
into send failure. An already open recorder is not switched by startLogging;
callers must explicitly stop before choosing another file.

## Limits

- Recording still starts through the existing ArduPilot-heartbeat/enabled
  mechanism, not automatically at physical connection as in MP10. Earlier TX,
  the heartbeat that first starts recording, and sessions that never start a
  recorder can be absent. This remains an explicit parity gap.
- Submitted UDP means queued for transport, not delivered or acknowledged.
  A synchronous disconnect can make the transport result uncertain and prevent
  logging even after a possible physical write. TLOG is not proof of delivery.
- QFile buffering is retained. A detected short write stops recording with an
  incomplete-tail warning; this is not per-record flush, checked close/flush or
  power-loss durability. Original log files are append-only, not atomic outputs.
- Mixed-direction logs can contain private outgoing coordinates and embedded
  payloads. Anon Log drops five audited opaque/secret classes and ambiguous
  commands, but remains only a partial privacy transform; see ANON_LOG_PORT.md.

## Verification

Qt5/audio build and full **240/240 tests pass (29.84 s)**; focused **8/8 pass
(13.17 s)**. Production runtime coverage includes exact MAVLink1 and signed
MAVLink2 bytes, RX/TX ordering, secret/raw/rejected-frame absence, logger changes
from receive/submitted/physical-write callbacks, open failure, real Linux
`/dev/full` write failure with callback replacement, synchronous link removal,
and outbound GPS_INJECT_DATA/GPS_RTCM_DATA extraction round-trip.

Privacy tests cover all-sender and zero-offset drops, bounded aggregate warnings,
malformed dropped frames, explicit global command coordinates, retained audited
non-coordinate command bytes and ambiguous-command removal. No real vehicle
key, parameter, mode or command was changed.

Both production transport and Developer Tools audits also pass on X11 (exit0).
The latter uses the real file pickers, verifies seven extracted bytes, displays
the updated older-log/recording-enabled warning, then exercises the six isolated
vehicle actions. All fixtures are in process and are removed on completion.

Evidence: `/tmp/apm-tlog-tx.JioGvl/`, `build-3.log`, `focused-2.log`, `full.log`,
`signing-x11.log`, `developer-x11.log`, `developer-x11.png`.
The first build found a test-local name collision; the first focused run found
a test selecting its file without closing a heartbeat-started auto recorder.
Both harness issues were fixed without weakening production safety checks.
Claude TCP c210/c179 supplied independent REVIEW-OK and remaining limitations;
Codex agents supplied privacy work, production tests and a separate core review.
Root scheduled every build/test. Windows/macOS and reference visual parity remain
unverified; this slice does not complete Developer Tools or Signing transitions.
