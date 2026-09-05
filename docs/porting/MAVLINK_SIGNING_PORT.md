# MAVLink Signing — protocol and key-store foundation

This is **not an enabled vehicle-signing tool yet**. Advanced Tools remains
14/16. The standalone `apm_mavlink_signing` library is compiled and tested;
the production transport does not yet bind it. Never infer authenticated live
telemetry from the presence of a MAVLink signature before the integration gates
below are complete. Bootloader Ed25519 signing is a different workflow.

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
receive table. The future manager must coalesce identical secrets even when
stored under different friendly names and retain the context across link
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
  must deliver the unsigned exception to radio diagnostics only, never treat it
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

## Required next integration gates

1. Add an application-owned key-domain manager: identical-key aliases share
   one replay context, stable 0..255 signing IDs persist with connection identity,
   physical epoch changes invalidate bindings but never clear replay history.
   Locking/deleting a vault key cannot silently turn a protected link unsigned.
   RX history is currently RAM-only: process restart admits the usual one-minute
   new-stream replay window (characterized by a test). Persistent RX high-water
   or a documented release policy remains to be chosen before claiming stronger
   restart protection. Do not discard contexts on ordinary dialog close/reopen.
2. Route legacy `LinkManager::writeMavlinkMessage` through the same sequencer as
   exact services. Bind signing to the internal frame writer exactly once;
   block public raw/mirror write-back on protected links. Audit bootloader and
   other non-MAVLink raw consumers rather than silently signing arbitrary bytes.
   Reject v1 negotiation while protected. SETUP_SIGNING contains a secret and
   must never enter general logs, inspector signals or mirror streams.
3. Authenticate original ingress bytes before packetReceived, vehicle discovery,
   parameter/command consumers, normal TLOG logging and mirror fan-out. Preserve
   exact bytes for CRC/MAC checks, not reconstructed payloads. Queued epochs and
   callbacks must not cross rekey/physical disconnect. Offline replay must not
   mutate live clocks or receive history and must disclose unverified signatures.
4. Add the modeless native key manager and exact-target provisioning service.
   Unlock/KDF operations belong off the GUI thread. Separate local key selection
   for an already signed vehicle from sending a new key over a user-confirmed
   trusted channel. Add/Use/Delete/Disable/Close, counts, current-key status and
   explicit locked/corrupt/unknown-key states are required. Lost master password
   has no recovery; never imply that signing encrypts telemetry or provisioning.
5. Provision only on a fresh exact disarmed target with default-Cancel warning;
   SETUP_SIGNING has no ACK. Model uncertain/timeout outcomes truthfully, preserve
   the usable key until verified transition, and never claim success merely
   because bytes were queued. Key changes/disable need an explicit reviewed
   transition, not an unsigned fallback or destructive blind retry.
6. Prove every outbound funnel and rejected-ingress fan-out, reconnect and
   alias behavior with production tests. Exercise enable/use/change/disable on
   a non-COMM_0 SITL channel (ArduPilot accepts unsigned USB traffic), then real
   X11 modeless dialog/lifecycle and native Windows/macOS packaging/crypto tests.
   Only then increase the working Advanced action count.

The broader port objective remains unchanged: full functionality first,
recognizable MP10 visuals afterwards, Settings/CONFIG next and Swarm last.

## Verified checkpoint, 2026-09-05

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
