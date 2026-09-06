# Download / Cancel Firmware Archive

Reference: MP10 `Services/FirmwareArchiveService.cs`,
`ViewModels/GCSViews/ConfigurationView/ConfigDeveloperToolsViewModel.cs` and
`MissionPlannerTests/Avalonia/MissionPlanner.Tests/FirmwareArchiveTests.cs`.
The original firmware manifest model clarifies the `options/Firmware` shape
and fields such as `url2560-2`; this tool is separate from firmware flashing.

## User workflow

The existing Developer Tools Download Firmware Archive button now opens a real
asynchronous parent-directory picker, including offline. It allocates a fresh
`MissionPlanner-Firmware-Archive-yyyyMMdd-HHmmss` UTC destination, with suffixes
`-2` through `-1000` for collisions. A named default/Escape-Cancel confirmation
shows that exact destination before any network request or archive creation.
It warns about large downloads, unsigned legacy HTTP, partial archives and
SHA-256 hashes not authenticating the firmware source. Download all firmware
starts the worker; it never flashes a board or changes the selected vehicle.

Download and Cancel participate in the existing Developer operation gate from
the first picker onward. Cancel only affects this page's archive workflow and
is disabled when idle. A modeless progress dialog and bounded/coalesced status
updates supplement the MP10 log. Closing/destroying the owning page requests
cancellation. Workers capture immutable values/shared state, not widgets;
completion cannot revive a deleted controller. A synchronous owner deletion
inside a Close observer is regression-tested and consumes the Close event.
The progress dialog's Cancel button retains a stable waiting state while the
worker drains; Escape also cancels, may hide the dialog, and cannot silently
continue downloading or reopen it on the next timer tick. Cancellation disables
the dialog button without deleting it inside its own click callback.

## Download and archive contract

Two HTTPS mirrors are tried in reference order: the ArduPilot binary GitHub
`Firmware/firmware2.xml`, then firmware.ardupilot.org's
`Tools/MissionPlanner/Firmware/firmware2.xml`. The first successfully downloaded
manifest wins; manifests are never merged. Invalid XML in that first downloaded
response is fatal, rather than silently trying a different manifest.

The parser rejects DTD/entities, credentials and non-absolute/non-HTTP(S) URL
fields. It examines every element whose local name starts with `url`, ignoring
case and empty values, including the root (MP10 calls XDocument.Descendants,
not XElement.Descendants). Namespaces and duplicate references are supported.
URLs are deduplicated by fully encoded absolute URI. Relative paths are
`files/<sanitized host>/<12-hex SHA256 of URI>-<sanitized filename>`; the original
URL, including its query, controls the digest. IDN and IPv6 hosts are handled.
Case-folded path collisions are refused; NFC normalization and Windows reserved
device names are additional Qt portability protections.

Up to four Qt worker threads stream firmware files. Original HTTP URLs try
HTTPS with its default port first, then the original HTTP URL only after a
network error/timeout. Policy, size, local I/O and cancellation failures never
trigger that fallback. Explicit HTTPS URLs never fall back to HTTP. Redirects
are manual, at most eight hops, and recheck URL/HTTPS policy at every hop.
Qt TLS errors are never ignored. Error/redirect bodies are aborted at headers
and never reach the file sink. Each fetch has a five-minute total deadline
across redirects and checks cancellation at chunks and 20 ms timer boundaries.

Limits: manifest 8 MiB; XML depth128/token count200000; distinct downloads20000;
firmware256 MiB each, enforced against declared and streamed bytes. Network
read buffers are128 KiB and chunks64 KiB. There is no smaller aggregate disk or
bandwidth quota: a large accepted manifest can still require enormous storage.
A fresh Qt network manager per fetch does not reuse connections across files.
Servers ignoring Accept-Encoding: identity can fail the conservative length
check; representative production HTTPS/TLS/proxy behavior is a separate gate.

Files are created exclusively in a private sibling `.partial-<UUID>` directory.
Network failures remain per-file results; local I/O/path-integrity failures
abort the archive. At least one successful firmware is required to publish.
Successful URL fields are rewritten to forward-slash relative paths, including
duplicates. Unavailable fields retain their network URLs. The archive contains:

- `firmware2.xml`: UTF-8 without BOM, canonical UTF-8 declaration and LF output;
  input Latin-1/UTF-16 declarations cannot mislabel the rewritten UTF-8 bytes.
- `checksums.sha256`: saved-byte SHA-256 hashes sorted by relative file path.
- `archive-report.txt`: source, downloaded/unavailable counts, saved bytes and
  each unavailable URL with a bounded single-line error.

The service independently hashes successful saved files again, then checks
the complete owned tree and publishes with one same-parent directory rename.
An existing destination is never intentionally overwritten. Cancel/failure
removes only registered still-matching files and empty owned directories;
there is no recursive deletion of foreign additions. Unsafe/incomplete cleanup
reports the last known staging path. If an ancestor was moved or replaced,
that path may no longer locate the retained bytes and must not be blindly used.

## Explicit limitations and GUI differences

Archive directories must not be edited by another program while this operation
runs. Canonical ancestry, available birth times, size/mtime and content checks
are not filesystem locks or protection against deliberate same-stamp swaps.
There are final-check-to-write/remove/rename races; filesystems without birth
times provide weaker identity checks. Injected transport/Cancel/Progress hooks
are trusted internal seams and must not mutate archive content: cancellation
callbacks remain callable during verification, so a malicious later callback
could change an already verified file while preserving its stamp. Production
callbacks only read cancellation state or update isolated progress state.

There is no fsync durability, crash resume, durable job history, source signature
verification, firmware compatibility or physical bootability guarantee. A
partial local manifest can still cause a later consumer to access the network.
Fresh per-file network managers and the full saved-file verification pass add
handshake/read costs. Cancellation is cooperative; XML parsing and some local
filesystem boundaries are bounded but not immediately interruptible.

Qt uses a themed non-native folder picker, explicit modeless progress, idle
Cancel disabling and an earlier operation gate; MP10 uses a platform picker
and primarily log progress. These are deliberate workflow differences, not
extra MP10 menu entries. Reference pixel/HiDPI and Windows/macOS runtime checks
remain separate from Linux functional acceptance.

## Verification

Final Qt5/audio build, focused9/9 (36.74s), full267/267 (47.78s), ten repeats
of each of4 suites (22.63s), production X11 exit0/zero audit failures and the
controller suite on X11 (14 passing cases, 4.206s) pass. Root inspected both
final screenshots. Evidence: `/tmp/apm-firmware-archive.UOhP7w/`; see
CURRENT_STATE.md for the exact final log filenames.
The four new suites cover manifest/path/encoding rules, parallel service and
filesystem behavior, real loopback HTTP and controller lifecycle. The production
`--firmware-archive-audit` runs actual Tools navigation with isolated settings,
two successful binary payloads (8 bytes total), one unavailable URL, both early
Cancel paths, HTTPS preference/fallback, deduplication and owned streaming
Cancel that leaves an earlier completed archive unchanged. Network SITL is not
subscribed to; official multi-gigabyte archives and real vehicles are untouched.
