# MAVLink Signing — transport and key-store foundations

The **local key manager and initial provisioning path are implemented**;
key change, disable and reconciliation are not. Advanced Tools has14 complete
workflows plus this one partial workflow. TOOLS and SETUP share the modeless `MavlinkSigningWindow`; it selects
a key for an already-provisioned OFFLINE connection through `configureSigning`.
Required profiles persist and restore locked before connection. Ordinary
unconfigured links are still unprotected: a signature's presence alone
does not imply authentication. Bootloader Ed25519 signing is a different workflow.

## Initial provisioning contract, 2026-09-05

Initial provisioning is distinct from offline Use locally. It requires a settled
selected ArduPilot autopilot1 target, an exact physical/profile/revision/target/
instance/epoch snapshot, a disarmed heartbeat at most3 seconds old and exactly
one observed autopilot on a dedicated serial/TCP-client/UDP-client route.
Host listeners and obvious multicast/broadcast UDP destinations are refused.
A RADIO/RADIO_STATUS frame also latches refusal before RX observers for the
physical epoch; serial-port type alone is not evidence of a direct cable.
A signed autopilot heartbeat observed in the current epoch latches refusal
before any RX observer callback. This is conservative signature-flag evidence,
not authentication of an unprotected connection. Absence of a signature cannot
prove the vehicle has no stored key (USB and per-channel options differ).
Operator consent must explicitly identify an unprovisioned vehicle and a trusted
private channel: the secret is sent in clear and changing it affects the vehicle's
channels. Exact target fields do not encrypt or establish physical exclusivity.

Before any possible provisioning write, the connection identity is persisted,
then the required SHA256 fingerprint and strict `initial-unconfirmed-v1` marker
are published together under `MAVLinkSigning/Profiles/ID` (`fingerprint` and
`provisioning`). The required hint is also saved before transmission. A failed or
uncertain publication blocks unsigned use in the current process. Restart and
re-add restore a locked required profile plus the marker; no implicit retry,
policy reset, key change or unsigned fallback is available. This marker means
preparation may have happened, not proof of transmission or vehicle effect.

The manager's separate first-live-install API requires an existing exact epoch
and no protected binding. It preserves that epoch and shared-key replay history.
The payload initial timestamp comes from the shared clock; its durable floor is
advanced to initial+60 seconds before the first signature, matching ArduPilot's
load offset once, not twice. A retained clock more than1 hour ahead of local wall
time or insufficient48-bit headroom is refused. Wrong local wall time itself,
vehicle GPS-time comparison and sudden-power-loss durability remain release risks.
The shared60-second advance is cumulative: roughly60 rapid initial provisions
can reach the1-hour guard, including preparations that later fail allocation.
This is a bounded first implementation for one vehicle, not a fleet-key utility;
do not reset clock files to bypass its guard.

Exactly one SETUP_SIGNING frame is signed with the newly installed local key;
this is only for an operator-known unprovisioned vehicle. An already protected
non-USB vehicle may refuse it; USB/per-channel exceptions may accept an overwrite,
which is why this is not a supported rekey method. Generic structured exact and
legacy sends now refuse SETUP_SIGNING before signing/sequence/writer invocation.
Only the private LinkManager typed path can reach the sequencer. It publishes no
packet observer and best-effort clears owned staging buffers; trusted raw APIs,
transport/platform copies and trusted in-process extensions are not a sandbox.

The UI always reports **submitted / outcome unconfirmed**, never ACK or success.
The persisted warning survives close/reopen, restart and offline key restoration.
Receiving authenticated new-key traffic does not clear it or claim durable
vehicle storage. No automatic resend, dual-key transition, key-change/disable or
operator reconciliation is implemented in this slice. Full Signing remains
partial pending those workflows and native-platform release evidence. Initial
provisioning on an isolated non-COMM_0 SITL channel is verified below; it does
not verify the still-unimplemented key-change/disable transitions.

## Initial provisioning verification, 2026-09-05

Qt5/audio configure/build pass; final full suite **237/237,22.96 seconds**.
Production tests use real LinkManager/protocol/transmitter/signing state with
isolated in-process physical links. They cover pre-observer signed/radio latches,
stale/armed/multiple-autopilot/revision refusal, one exact signed SETUP_SIGNING,
secret-observer suppression, write-time link removal, persistent unconfirmed
required profiles and no second attempt. Widget tests cover default-Cancel,
consent/key-delivery mutations and production-theme wrapped status at720x786
and560x520, including shrink/reopen. Short windows scroll without clipped text.

Manual X11 used only a separate ArduCopter sys233 fixture, instance71, with
SERIAL1 TCP127.0.0.1:61980 and private temporary storage. The real TOOLS window
created a vault/key, exercised Cancel without policy change, then sent once.
The app received authenticated telemetry but correctly retained SUBMITTED —
UNCONFIRMED. Independent pymavlink authenticated980 packets on signature link1;
an unsigned read-only AUTOPILOT_VERSION request was ignored and a signed one
answered. After a fixture restart without wiping storage, the same checks pass
again with the persisted vehicle key. This external test evidence is deliberately
not an automatic UI reconciliation rule. No user/network SITL keys were changed.
The original GUI TLOG contains27,090 valid frames,5,402 independently checked
MACs, no SETUP_SIGNING, no raw fixture key and no BAD_DATA.
Final-build X11 then restarts the application, restores the locked/pending
profile, unlocks the vault and selects the same key locally without resending.
Reconnect receives thousands of authenticated packets with the uncertainty
warning intact and fully readable, including at560x520. The production signing
runtime independently passes under X11 with exit0.
Closing/reopening preserves pending state and the unlocked vault; application
shutdown with the window open exits0. The fixture and test windows are stopped.

The new immediate UAS rediscovery test initially exposed a real shared-link
destruction crash and additional stale pointers in AP2DataPlot2D, UASRawStatusView,
QGCMapWidget and the decoder's first field. GDB evidence drove lifetime fixes:
detach before LinkInterface destruction, retire exact UAS/companion identities
before DeferredDelete, disconnect retired ingress, guard callbacks/consumers and
resolve decoder state before field zero. A test itself also needed to deliver
queued ingress metacalls without draining the deferred deletion it exercises.
The first wrapped-height implementation could grow but not shrink; unconstrained
measurement plus a resize reentrancy guard fixes both geometry rows.

All evidence: `/tmp/apm-signing-initial.g6VlP9/`. Claude TCP c205/c206 reviewed
firmware/routing/no-ACK/clock contracts; c163 reviewed logical lifetime teardown.
The corrected firmware reading honors exact target routing, but routers can
still forward cleartext: it does not replace the private-channel requirement.
Three Codex streams supplied backend/widget/runtime work with root-owned builds.

## Reference and deliberate corrections

MP10 `ConfigAdvancedViewModel.ManageSigningAsync` offers Add, Use, Delete,
Disable and Close, with stored-key count, current name and signed-packet count.
`MAVAuthKeys.AddKey` derives 32 bytes as SHA-256 of the UTF-8 seed. Its
`setupSigning` sends SETUP_SIGNING twice, with no ACK, then immediately reports
enabled/disabled. ArduPilot refuses provisioning while armed; enabling applies
to the vehicle's channels, not just a local GCS dialog.

The protocol reference is the [MAVLink signing specification](https://mavlink.io/en/guide/message_signing.html),
the vendored `mavlink_helpers.h` and local ArduPilot `GCS_Signing.cpp`.
MP10 is the workflow reference, but its MAC-only acceptance, random reconnect
link ID, fixed-IV key-store encryption and all-zero fallback signing key are
not security properties to reproduce. Claude TCP audit c196 and independent
Codex reviews identified these differences. The c196 suggestion to reset replay
history per physical epoch is explicitly **rejected**: ArduPilot itself shares
the stream table across channels, and reconnect must not rehabilitate a replay.

## Implemented contracts

`MAVLinkSigningSession` is thread-confined and noncopyable. It holds one
zeroized-on-destruction key, a shared persistent clock and a bounded 256-entry
receive table. The application-owned manager coalesces identical secrets even
when stored under different friendly names and retains the context across link
disconnects. Physical link IDs are not part of the replay key: the signed
stream is `(source system, source component, signature link ID)`.

- Signing accepts exactly one CRC-valid, unsigned MAVLink 2 frame of a known
  dialect message and bounded payload. It preserves sequence, source identity,
  compatibility flags and payload/extension bytes; it changes the signed flag,
  CRC and appends the protocol signature. LE48 encoding is explicit, independent
  of host endianness. No encoder re-trimming can reinterpret checksum bytes as
  payload. The independent pymavlink fixture is reproduced byte for byte.
- Verification checks framing and CRC, then SHA-256 with constant-time digest
  comparison, then timestamp/replay, then persistent forward-clock reservation.
  No rejected packet changes a stream or advances the clock. Typed results
  distinguish authentication, unsigned radio, replay, stale new stream,
  invalid frame/MAC, capacity and unavailable clock.
- Unsigned RADIO_STATUS/RADIO are classified separately from authenticated
  traffic. Incorrectly signed radio packets are rejected. Production integration
  delivers the unsigned exception to radio diagnostics only, never treating it
  as an authenticated vehicle or use it to negotiate a MAVLink 1 downgrade.
- Existing streams require a strictly increasing timestamp; new streams allow
  the mavgen one-minute window, inclusive at 6,000,000 ticks. Full capacity
  rejects new streams without evicting old replay history. Unknown message IDs,
  unsupported incompatibility flags and payloads larger than the current
  dialect are rejected; future dialect extensions require a coordinated update.

`MAVLinkSigningClock` owns an interprocess lock and a strict 52-byte versioned
state with SHA-256 integrity check. It reserves 100,000 ten-microsecond ticks
before issuing timestamps, so process restart skips unused reserved values.
Clock rollback cannot reuse a timestamp. An authenticated future timestamp
advances the floor only after a successful reservation. Invalid state, path
aliases/symlinks, lock conflict and external changes at a reservation boundary
fail without overwriting the unexpected file. The 48-bit maximum can be sent
once, after which TX is exhausted; an incoming maximum is rejected because the
clock cannot advance beyond it. Atomic QSaveFile replacement covers process
failure, **not guaranteed sudden-power-loss durability** on every filesystem.
The digest is a corruption check, not protection against a local malicious user.

`MavAuthKeyStore` implements a password-unlocked named-key vault through OpenSSL
AES-256-GCM and PBKDF2-HMAC-SHA256. No master password, seed or adjacent wrapping
key is persisted. Explicit create/unlock, authenticated parse, process locking,
private atomic replacement, duplicate-name refusal and transactional edits
preserve unreadable/external-modified files. Full format and limitations are in
`MAV_AUTH_KEY_STORE.md`. OpenSSL Crypto >=1.1.1 is a required cross-platform
build dependency; there is no silent plaintext fallback.

### Production parser defect fixed in this slice

The old generated helper can replace BAD_CRC with OK after consuming a signed
trailer, even with no configured key. `MAVLinkFrameParser` now finishes a
CRC-invalid trailer without invoking native signature verification. It reports
BAD_CRC, preserves receive-success and native replay/clock state, and recovers
for the following frame. Tests include a valid MAC computed over invalid CRC
bytes, with verification configured and unconfigured. This fix is active in
existing users of the application-owned frame parser; direct uses of generated
global-channel parsing, including legacy replay, remain separate audit scope.

## Production transport integration

`LinkManager` owns one thread-confined `MAVLinkSigningManager`. A protected
binding may be configured only while the registered physical link is offline;
replacement policies are refused. Epoch activation precedes queued ingress,
and disconnect invalidates writes without forgetting protection or replay state.
Removal is offline-only. Identical-key aliases share one context across every
physical link and reconnect. At most 256 distinct key contexts are retained for
the process lifetime; new keys fail at capacity, existing keys remain usable.
No eviction is permitted because it would rehabilitate old replay packets.

One stable 0..255 signature link ID is allocated per connection-profile identity
under `<writable application data>/mavlink-signing/`. The owner-only versioned
`signing-link-ids.state` registry has bounded parsing, an integrity digest,
revision checks and atomic publication; its writer shares the lifetime lease of
`signing-clock.state`. Neither file stores a signing secret or friendly key name.
Ordinary unprotected connections create neither file. New profiles fail once
all 256 IDs are allocated; existing profiles continue to work. IDs are not
recycled. Renaming an identity consumes a new ID and can consume another stream
on a vehicle with a smaller receive-table limit (16 in the inspected firmware).
Do not delete clock state to recover capacity: that loses the reserved timestamp
floor. There is no supported registry-reset/migration UI yet. A failed first
binding may still reserve clock capacity while acquiring the shared writer lease;
this skips timestamps safely and does not publish a link policy.

The exact transmitter finalizes each link's sequence/version before its sole
pre-writer signing hook. Only the SIGNED flag, CRC and trailer may change; malformed,
missing, stale or failing signing never invokes the writer (`SigningUnavailable`).
First-attempt command/parameter failure is therefore certain non-transmission,
not a spurious uncertain-write quarantine. Earlier possible transmissions still
retain their uncertainty. Observer messages contain the actual finalized wire
signature, not the caller's staging checksum. Legacy `writeMavlinkMessage` uses
this same sequencer; COMM_0 is always v2 payload staging so another v1 link cannot
truncate extensions before a protected v2 send. Each destination still negotiates
its own version, while protection pins v2. Public raw writes, including Mirror
write-back, are refused on protected links and before a physical epoch exists;
the private typed writer is the only protected path.

`MAVLinkFrameParser::lastFrame()` captures bounded original wire bytes.
Production ingress verifies those bytes before version negotiation, discovery,
packet signals, parameter/command consumers, normal TLOGs or mirror forwarding.
Rejected traffic cannot trigger the non-MAVLink reset heuristic on protected
links. Unsigned radio diagnostics reach only `RadioStatusMonitor`, never vehicle
discovery or a v1 downgrade; these diagnostic-only unsigned radio frames are
also deliberately absent from TLOG. Accepted logging/mirroring uses original bytes.

SETUP_SIGNING is excluded from TX/RX public observations and live logs/mirror;
decoder and offline replay also suppress it. CSV/text export retains a redacted
metadata row without raw/hex/decoded secret bytes. TCP/UDP debug traces expose
endpoint/direction/length only. There is not yet a redacted Inspector event for
provisioning. Existing binary logs are not rewritten. New Anon Log TLOG output
explicitly drops SETUP_SIGNING and reports the count, but this limited filter
is not a general secret sanitizer; original logs can still contain historical
keys. Current live TLOG recording includes final signed typed TX frames after
successful submission, while the private SETUP_SIGNING path bypasses the
logger entirely. Session replacement and logging failure do not leak that
frame or change a successful transport result; see `TLOG_RECORDING.md`.
Offline replay never authenticates traffic or
mutates the live signing clock; explicit unverified-signature UI remains a gap.

## Required next integration gates

1. Keep local key selection and persisted fail-closed restoration green while
   adding vehicle transitions. The current initial-provisioning UI must not be advertised
   as a complete vehicle-security workflow. Locking/deleting a vault key must not silently
   turn a protected link unsigned. RX history is RAM-only: process restart admits
   the one-minute new-stream replay window characterized by tests. Persistent RX
   high-water or a documented release policy remains before stronger claims.
2. Add reviewed key-change/disable and operator reconciliation UI.
   Exact initial provisioning, the local vault/key manager and off-thread KDF are implemented;
   local selection must stay separate from sending a key over a user-confirmed
   trusted channel. Explicit uncertain/failed vehicle-transition states are
   required. Lost master password
   has no recovery; never imply that signing encrypts telemetry or provisioning.
3. Provision only on a fresh exact disarmed target with default-Cancel warning;
   SETUP_SIGNING has no ACK. Model uncertain/timeout outcomes truthfully, preserve
   the usable key until verified transition, and never claim success merely
   because bytes were queued. Key changes/disable need an explicit reviewed
   transition, not an unsigned fallback or destructive blind retry.
4. Keep production outbound/fan-out/reconnect/alias regression coverage green.
   Exercise enable/use/change/disable on
   a non-COMM_0 SITL channel (ArduPilot accepts unsigned USB traffic), then real
   X11 modeless dialog/lifecycle and native Windows/macOS packaging/crypto tests.
   Only then increase the working Advanced action count.

The broader port objective remains unchanged: full functionality first,
recognizable MP10 visuals afterwards, Settings/CONFIG next and Swarm last.

## Local operator UI checkpoint, 2026-09-05

`MavlinkSigningWindow` is a modeless, delete-on-close observer of the application
vault service. It implements create/confirm, unlock/lock, Add, Delete and
explicit named-key Use locally. Password/seed inputs are bounded, masked and
cleared; optional seed visibility resets on submit/lock/close. Exact-case key
names have quoted escaped tooltips; keys themselves are never displayed.
Counts and selected profile/fingerprint/key status are metadata. The verified
packet counter is explicitly labeled shared-key, not falsely per-link.
Periodic refresh reads cached metadata only and does not rebuild unchanged
lists or perform vault I/O.

The first local activation warning names the physical connection, full profile
ID and selected key; Cancel is default. It explains signed-only persistence,
the already-provisioned-vehicle prerequisite, no supported local reset/rekey
yet, and that nothing will be sent to a vehicle. Deletion separately warns it
cannot disable vehicle signing or revoke loaded transport keys. Provision and
Disable remain disabled with explanations. Closing cancels pending activation
even before deferred deletion; reopening cannot reuse a closing observer.

An asynchronous activation revalidates QObject identity, physical id, persistent
profile id, offline state, epoch0 and a runtime revision changed by registration,
endpoint edits and connect/disconnect. A brief connect-disconnect or offline
address edit therefore cancels the stale selection. Revision is not persisted.
`requestKeyByFingerprint` is also available in the worker, bounded to128 keys
with nonmatch cleansing; this UI still chooses names explicitly.

Qt5/audio build, focused5/5 and full237/237 tests pass (18.67 seconds).
Production Setup runtime performs actual vault create/add and offline UDP-client
Use Cancel/Accept, persisted fingerprint, epoch0/no TX, vault Lock retention and
close/reopen. Both settings and app data are fresh per audit. Real X11 route audit
and manual Tools/create/add/Cancel/reopen/app-shutdown pass, exit0.
Evidence: `/tmp/apm-signing-ui.VUTMZC/`. The first focused run's stale closed
confirmation came from the audit's manual event loop not draining DeferredDelete,
not from production activation; that helper now matches QApplication's outer loop.

This intentionally differs from MP10's coupled Use/provision and Disable/zero-key
actions, raw Base64 key grid and duplicate-name replacement. The 24-item MP10
TOOLS inventory remains intact; Local keys is an additional Qt entry. Advanced
counts14 complete plus1 partial, never15 complete. Windows/macOS, high-DPI and
final reference visual matching remain release/polish work.

## Persisted-profile and asynchronous-vault slice, 2026-09-05

Manual connections persist canonical lowercase UUID `profileId` and a
`signingRequired` hint in `LINKMANAGER/LINKS`. Automatic UDP listeners use
`startup-udp-PORT`, with an independent presence-only
`MAVLinkSigning/StartupRequiredPorts/PORT` hint. The same QSettings file holds
`MAVLinkSigning/Profiles/ID/fingerprint`: exactly 64 lowercase SHA256 hex
characters, with no key, seed, master password or friendly vault alias.
Record/group existence means required. Missing hinted or malformed metadata,
duplicate identities and conflicting manual/startup-port definitions are
quarantined, not normalized to an unsigned connection. All saved rows are
validated before any factory auto-connects. Corrupt settings are not rewritten.
Legacy identity-less default-UDP adoption assigns the deterministic identity
before opening; an existing manual UUID is never silently relabeled or omitted.

`requireSigning` installs a locked binding without a key, timestamp-file write
or registry-slot allocation. Restart/re-add never implicitly reuses a retained
key context. Correct offline key selection activates the binding; wrong keys
leave it locked. Factory auto-connect, direct transport reconnect, session
activation, raw writes, exact/legacy TX and RX all check the requirement.
Publication failure leaves the current process blocked even when disk-write
durability is uncertain. No remove-policy/rekey/reset API is provided; deleting
a connection leaves its policy and allocated signing-ID record intact.
These settings are corruption/crash defenses, not protection against a malicious
same-user settings rollback or deletion of both identity and policy.

`LinkManager` lazily owns `MavAuthKeyService` at
`mavlink-signing/authkeys.vault`. The dedicated worker owns the synchronous store;
only one bounded operation is admitted, KDF/file work stays off the GUI thread,
and secret export uses an explicit owner-thread callback, not a Qt signal.
Completion cannot overtake the returned token even during nested event loops.
Shutdown drains the admitted job, cancels key delivery and joins the worker.
Active transport key contexts are independent of vault lock/window lifetime.
Fingerprint-based key choice, modeless Add/Use/Delete/Disable and exact fresh
disarmed no-ACK provisioning remain subsequent integration work. Advanced stays
14/16; no network SITL keys are changed by these isolated fixtures.

Verification: full Qt5/audio build, focused 7/7 and full 236/236 CTest pass
(18.61 seconds). Production runtime covers real settings reload, direct
transport reconnect, missing/corrupt policy, duplicate and startup/manual-port
collision before `newLink`, locked UDP/TCP factories and offline matching-key
activation. The same audit exits0 under X11. Ordinary network SITL smoke also
exits0: Quick current/Home and resize remain functional, with one Home reply,
625 positions, 418 SYS_STATUS and 418 BATTERY_STATUS records and zero BAD_DATA
in independent TLOG parsing. Evidence: `/tmp/apm-signing-profiles.jrT00E/`.

## Transport checkpoint, 2026-09-05

Qt5/audio/OpenSSL configure and full build exit 0. Final full suite passes
**234/234 in 56.41 seconds**. New manager and actual production-transport tests
cover same-key aliases, cross-link and reconnect replays, stable IDs, bounds,
failed signing before writer invocation, exact signed observer bytes and ingress
suppression. Expanded parser/export/command/parameter/Inspector tests pass.
The first run was 233/234: Inspector compared a staging checksum rather than
the finalized wire; the corrected audit now requires full wire-byte equality.

The production signing audit also passes on real X11 (exit 0), with two in-process
links and the actual LinkManager, protocol, transmitter, decoder and radio monitor.
It performs no physical-vehicle provisioning. A separate ordinary UDP14699 X11
run displays source233; its independent pymavlink peer sends 222 unsigned
heartbeats and parses 44 GCS heartbeats, 60 stream requests, three COMMAND_LONG,
three parameter-list and six mission-list requests without framing errors.
The resulting TLOG has 221 source233 heartbeats and zero BAD_DATA. Both the
application and peer exit normally. The final test-only lifetime adjustment keeps
the isolated temporary directory until application singletons release their
signing lock; a final X11 audit has no late lock-file cleanup warning.
Evidence and personally inspected screenshot: `/tmp/apm-signing-transport.j50T7t/`.

Claude TCP c198/c199 and c146 addendum are REVIEW-OK; the final report is
`/home/alex/SRC/claude-reports/c199-signing-integration-review.md` (9179 bytes,
SHA256 `d96fb685e8f761e27b88ec43ee339f32effe12ddee417149224985a4c79195fc`).
Three bounded Codex streams supplied disjoint implementation/tests and review;
root scheduled all builds/tests. Advanced stays 14/16 until operator workflow,
persisted fail-closed policy and non-COMM_0 provisioning gates pass.

## Previous foundation checkpoint, 2026-09-05

Qt5 with required Multimedia/TextToSpeech and OpenSSL 3.0.13 configured and
built successfully. The full suite passes **232/232 in 55.63 seconds**,
including three new test executables and the expanded production-parser test.
Real X11/UDP14698: forty invalid-CRC but valid-MAC heartbeats left the vehicle
selector empty/disconnected; the following valid signed source232 appeared
normally. Independent pymavlink parsing finds 124 source232 heartbeats and zero
BAD_DATA in the resulting TLOG. This verifies CRC filtering/recovery, **not
live signature authentication**, which remains unbound. Application exit 0
while the peer was active. Logs, isolated profile, peer script and personally
inspected screenshots: `/tmp/apm-signing.krlrz9/`.

Claude's c197 final addendum is REVIEW-OK (8722 bytes, SHA256
`864e641fc4bae88f20e017576b91b45706055e1d4a645a0f10aa11e7c9a5e133`).
Remaining design tradeoffs include authenticated far-future clock learning,
roughly one reservation write per second under continuous traffic, explicit
invalid-wall-clock failure, and a versioned KDF upgrade/passphrase-rotation
workflow. None is silently treated as a completed transport or release gate.
