# MAVLink signing-key vault foundation

## Asynchronous application service (2026-09-05)

`MavAuthKeyService` now wraps this synchronous store in one dedicated worker
thread, lazily owned by `LinkManager`. Its default path is
`mavlink-signing/authkeys.vault` under writable application data. Construction
does not create or overwrite a vault. Create/unlock/add/remove/lock admit one
operation at a time; refusal returns token 0. Metadata-only completion signals
never carry keys. Explicit `requestKey` callbacks run on the owner thread after
the token returns, including when admission observers run nested event loops.
The service cleans its own key/input buffers; retained caller copies remain the
caller's responsibility. Foreign-thread mutation is refused.

Window closure does not destroy this application-owned service. Terminal
shutdown hides names, drains the one admitted operation, cancels pending secret
delivery and joins without terminating the worker or pumping GUI events.
Existing transport keys are separate from vault state: locking the vault cannot
silently make a protected link unsigned. Worker/vault tests are not evidence of
a completed vehicle-signing workflow. Modeless local key management is now
available through Tools/Setup; vehicle provisioning/change/disable remain
unimplemented. `requestKeyByFingerprint` scans at most128 worker-owned keys,
cleans nonmatches and exports only a match; the window uses explicit names.

`MavAuthKeyStore` is a standalone, thread-confined encrypted key repository.
It does **not** enable MAVLink signing, transmit SETUP_SIGNING, choose a vehicle,
alter the parser/transmitter or make the Advanced Tools Signing button usable.
Production transport/signature verification now uses an application-owned
manager described in `MAVLINK_SIGNING_PORT.md`; its modeless UI uses this store
only through `MavAuthKeyService`. Persisted fail-closed protection and offline
operator key selection are implemented; vehicle transitions remain separate.

Reference inspected: MP10 `ExtLibs/ArduPilot/Mavlink/MAVAuthKeys.cs` and
`MavAuthKeyStore.cs`, plus `MissionPlanner.Tests/MavAuthKeyStoreTests.cs`.
The reference creates a signing key as SHA256 of the seed's exact UTF-8 bytes.
The native store retains that interoperable derivation, but deliberately does
not reproduce the adjacent plaintext `authkeys.key` wrapping-key material or
legacy MAC-derived encryption/migration. A fresh 3.0 application namespace and
explicitly entered master passphrase are used instead.

## API and operational contract

Construct with an absolute vault filename in an already existing directory.
The directory is not automatically created and neither destination symlinks
(including dangling ones) nor directory destinations are followed. A canonical
parent path makes ordinary `.`/`..` and directory-symlink aliases share one
writer lock.

- `create(master, error)` creates an empty encrypted vault only if no file exists.
  An existing empty, corrupt or wrong-password file is never treated as missing.
- `unlock(master, error)` requires an existing authentic, strictly parsed vault.
  Failure leaves the object locked and the encrypted file bytes unchanged.
  Calling either operation on an unlocked instance fails without replacing its
  active state.
- `addSeed(name, seed, error)` derives SHA256(UTF8(seed)) and immediately persists
  one new named key. Exact duplicate names fail; there is no silent replacement.
  Different names may deliberately refer to the same key material.
- `removeKey(name, error)` removes only an existing exact name and immediately
  persists the remaining collection. Neither operation modifies active keys
  until atomic file publication succeeds.
- `keyNames()` returns exact, case-sensitive QString identities in sorted order.
  Leading/trailing spaces, case and Unicode normalization are not silently
  changed. A UI must make such names distinguishable.
- `key(name, output, error)` exports a new 32-byte secret copy. `deriveSigningKey`
  provides the standalone interoperable seed conversion. On failure these
  methods clear the output buffer rather than leaving a stale key to be used.
- `lock()` and destruction cleanse owned sensitive buffers best effort, clear
  all active keys/wrapping material and release the writer lock. Afterward the
  names list is empty and secret lookup/add/remove fail. An explicitly exported
  copy remains the caller's responsibility.

There are no callbacks or QObject signals. Calls are synchronous and must not
run concurrently on the same object. In particular, PBKDF2 deliberately has a
substantial cost; an eventual UI should perform create/unlock on a bounded
worker with clear busy state rather than blocking the GUI thread.

## Cryptographic format

All integers below are unsigned big-endian. Version 1 has a fixed KDF cost;
there is no caller-controlled weak setting or untrusted arbitrary iteration
count. Unsupported versions/costs and inconsistent lengths fail before KDF work.

| Offset | Length | Meaning |
|---|---:|---|
| 0 | 8 | ASCII `APMMAVK1` format/version magic |
| 8 | 4 | PBKDF2 iteration count, exactly 600000 |
| 12 | 16 | Random PBKDF2 salt |
| 28 | 12 | Random AES-GCM nonce |
| 40 | 4 | Ciphertext/plaintext length |
| 44 | declared length | Encrypted collection |
| after ciphertext | 16 | AES-GCM authentication tag |

The full 44-byte header is AES-GCM additional authenticated data. The wrapping
key is PBKDF2-HMAC-SHA256(master UTF-8, salt, 600000), producing 32 bytes.
AES-256-GCM is implemented through OpenSSL EVP; salt and nonce use `RAND_bytes`.
Every provider return value is checked. Authentication succeeds before any
decrypted collection is parsed or exposed. Header-valid wrong passwords and
authentication failures share a generic error; neither enables write access.

The salt is newly random when the vault is created and remains fixed for its
lifetime. An unlocked session retains only the derived wrapping key, not the
passphrase, so it cannot rederive a new salt without asking the operator again.
Every attempted encrypted rewrite gets a fresh random 96-bit nonce. A failed
rewrite does not reuse its generated nonce on the next attempt.

Decrypted collection:

1. Two-byte key count.
2. For each entry, two-byte UTF-8 name length, exact name bytes and 32 raw signing
   key bytes.
3. No trailing bytes.

Invalid UTF-8, invalid names, duplicates, zero signing keys, excessive counts,
short keys and trailing bytes are rejected even after successful authentication.
Names and signing keys are inside the ciphertext. Seeds and master passphrases
are never serialized. Public salt/nonce/version/length and the lock's ordinary
PID/host metadata are not wrapping-key material. There is no plaintext secret
sidecar, settings entry, log output or automatic legacy import.

## Bounds and identity validation

| Input/resource | Bound |
|---|---:|
| Encrypted file read | 65536 bytes; one extra byte detects oversize input |
| Named keys | 128 |
| Name | 1..128 UTF-8 bytes, non-blank, no controls/NUL or invalid surrogates |
| Signing seed | 1..4096 UTF-8 bytes, non-blank, no NUL or invalid surrogates |
| Master passphrase | 12..1024 UTF-8 bytes, non-blank, no NUL or invalid surrogates |
| Requested path | Absolute, at most 4096 UTF-16 code units, no NUL/invalid surrogates |
| Salt / nonce / tag / key | 16 / 12 / 16 / 32 bytes |
| Writer acquisition | Nonblocking `QLockFile::tryLock(0)` |

Cheap UTF-16 bounds run before UTF-8 allocation. These are byte/size limits, not
an entropy guarantee: a predictable twelve-byte passphrase remains predictable.
The master should be a strong, independently chosen secret and must not be
confused with the vehicle's signing seed. Whitespace-only/empty seeds are not
accepted; non-blank seeds preserve their complete whitespace and exact UTF-8
bytes for MP10 interoperability.

## Persistence, exclusion and failures

One `QLockFile` lease is held for the entire unlocked session, including read-only
key lookups. A second store cannot open the same canonical pathname for writing
until the first locks or is destroyed. Age-based stale-lock expiry is disabled;
Qt may still recognize a lock belonging to a process that has exited. No live
writer lock is explicitly removed or forced by this implementation.

`QSaveFile` has direct-write fallback disabled. The staged ciphertext and lock
file request owner read/write permissions. Staging permissions are restricted
before writing; a permission/write/flush/commit failure prevents active-state
publication. On POSIX the resulting file has owner RW and no group/other/execute
bits. Qt's Windows permission abstraction is **not** a complete NTFS ACL editor;
the containing user-private directory's ACL remains important and native Windows
and macOS evidence is still required.

The vault's ciphertext SHA256 revision is recorded at unlock/publication and
rechecked before staging and immediately before commit. Detectable external
replacement, deletion, symlink substitution, directory change or corrupt edits
fail rather than overwrite a file the session no longer owns. Add/remove work
on a private candidate collection, swap it into active memory only after commit,
and cleanse the rejected or superseded secret copies on destruction. A failed
mutation leaves the original active keys available; the caller must surface the
save error rather than claim the edit succeeded.

QLockFile coordinates cooperating writers, not malicious processes with access
to the directory. Qt pathname checks do not provide a race-proof sandbox against
a hostile same-user process replacing directory entries between the final check
and rename, removing lock files or rolling back an entire authenticated vault.
Keep the vault in a protected user directory. Anti-rollback storage, OS keychain
integration, master-passphrase rotation/recovery, optional encrypted backups and
an explicit legacy import flow are not implemented in this foundation.

## Secret lifetime limitations

Owned fixed-size signing/wrapping keys and temporary UTF-8/decrypted buffers use
`OPENSSL_cleanse` before release. Plain serialization reserves its entire bounded
capacity before appending secrets so growth does not abandon earlier plaintext
allocations. Encryption contexts are freed through EVP RAII on every exit.
The application never retains the master QString or seed after the call.

This is best-effort cleanup, not a locked-memory enclave: callers still own
their input QStrings, exported QByteArrays and any implicit-shared copies.
The caller must clear editors and best-effort scrub transport key copies when
their lifecycle ends. OS swap, crash dumps, allocator/runtime copies and process
debugging are outside this storage guarantee. Losing the master passphrase
means losing access; no adjacent recovery key is created.

## Verification scope

`tests/test_mavauthkeystore.cpp` covers SHA256's known `abc` vector, exact UTF-8
and whitespace behavior, encryption round-trip, absence of plaintext names,
seeds/master/key bytes, public header, fresh nonces/fixed salt, owner permission
flags, lock-visible empty API state, non-ASCII paths/names/passwords/seeds,
case-sensitive duplicate/deletion behavior, wrong passwords, seven authenticated
header/ciphertext/tag tamper locations, missing/corrupt/oversized files,
independently encrypted invalid collections (including zero keys), multiwriter
and path-alias rejection, failed mutations preserving active keys and file bytes,
128-key/input-size boundaries and destination symlink/directory rejection.

The filesystem failure test covers a changed ciphertext revision and an unusable
directory destination; it does not simulate every low-level filesystem crash or
power-loss timing. Actual configure/build/test results belong to the root-owned
slice handoff. No test in this source contacts or commands a vehicle, and passing
vault tests cannot be counted as production MAVLink signing evidence.
