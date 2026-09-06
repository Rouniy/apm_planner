# SFTP DataFlash log download

This is an independent companion-computer SSH workflow, not MAVFTP, a selected
vehicle operation, or Swarm. The Developer Tools action opens the shared real
modeless `SftpLogDownloadWindow`, including while disconnected from MAVLink.
The complete Tools port is **not** measured by the Developer action count.

## Reference and workflow

MP10 sources: `Views/SftpLogDownloadWindow.axaml`,
`ViewModels/SftpLogDownloadViewModel.cs`, `Services/SftpLogSession.cs`,
`Services/SftpLogDownloadSupport.cs`, `Services/SshTerminalSession.cs`,
`Services/DataFlashLog.cs`, and Utilities `BinaryLog`, `DFLog`, `DFLogBuffer`.
The original credential-bearing SCP setting is deliberately not migrated into
the fresh APM Planner 3.0 namespace.

The 900x680 window (minimum760x560) supplies host, port, username, masked
nonpersistent password, absolute remote directory, four-column selectable log
table, Refresh, Download Selected/All, optional KML, Delete Selected/All,
Cancel, progress and bounded activity. Defaults are10.0.1.128:22, user,
`/home/user/dflogger/dataflash/`, KML enabled. One dedicated QThread constructs,
uses and destroys the persistent SSH session. Changing connection/list identity
invalidates the old rows. Every operation consumes the entered password even
when reusing an authenticated session; internal erasure clears Undo history.

Unknown or changed host keys stop **before password authentication**. The
default-Cancel consent displays endpoint, algorithm, bit length and SHA256
fingerprints. Explicit trust is persisted with checked sync/readback, then a
new connection must match that pin before authentication. The pin name matches
MP10: `SSHHostKey_` plus uppercase SHA256 hex of UTF8 lowercase `host:port`.
Fingerprints hash the raw SSH public-key blob, use unpadded Base64 and the
`SHA256:` prefix. No process receives credentials and no known_hosts is consulted.

Downloads stream to private staging, preserve complete BIN files even if later
conversion fails/cancels, derive LOG and optional KML, and report actual published
paths and warnings. Existing files are never replaced. A default-Cancel delete
plan lists every **frozen listed remote path**, size and UTC modification time;
the backend rechecks metadata before unlink. Confirmed individual deletions are
retained in partial results; interrupted submitted unlink has an unknown outcome,
not an automatic retry or an assertion that the remote file was untouched.
Application Close waits for owned cancellation; rejecting Close must not leave
a delayed application-close request attached to later standalone window Close.

## Dependency and offline builds

The default is static [libssh2 1.11.1](https://libssh2.org/download/), using the
already required OpenSSL backend. CMake pins the official tar.xz SHA256:
`9954cb54c4f548198a7cbebad248bdc87dd64bd26185708a294b2b50771e3769`.
Its COPYING is installed in the application resource licenses directory.
Dependency tests/examples are disabled locally, without disabling APM tests.
No runtime library-name probing or SSH protocol reimplementation is used.

An offline build can supply a previously verified unpacked source tree:

```sh
cmake -S . -B build-codex-qt -DAPM_QT_MAJOR=5 \
  -DFETCHCONTENT_SOURCE_DIR_APM_LIBSSH2=/absolute/path/libssh2-1.11.1
```

Alternatively `-DAPM_USE_SYSTEM_LIBSSH2=ON` requires pkg-config libssh2>=1.11.1;
an older package is rejected rather than silently substituted. Native packaging
and offline distribution of this dependency remain release checks.

## Limits and deliberate differences

- 20s total connection deadline;30s operation inactivity;15s idle keepalive;
 64KiB transport buffers. Only password authentication is implemented.
- 20k BIN entries,200k scanned directory records,4096-byte remote paths,
 16GiB per downloaded file,100-character sanitized local basename and bounded
 collision suffix allocation. The free-space check covers listed BIN size,
 not subsequent text expansion; later disk-full errors preserve complete files.
- Listings accept UTF8 regular non-symlink BIN files. Download compares pre-open
 LSTAT, opened-handle FSTAT and post-open LSTAT; coherent append growth is
 permitted and listed/received size differences are warnings. SFTPv3 metadata
 exposes no inode or atomic no-follow/open/delete transaction: these consistency
 checks are not remote identity proof or a filesystem lock.
- Linux no-replace publication uses a pinned O_NOFOLLOW descriptor plus linkat;
 cleanup pins its own staging inodes against immediate reuse. Other Unix uses
 link; Windows uses MoveFileEx without REPLACE_EXISTING. Native source-identity
 and network-filesystem guarantees are weaker/unverified. Linux requires procfs
 and a filesystem supporting hard links. Concurrent directory replacement is
 checked, not prevented by a filesystem lock; group publication is not atomic.
- GPS filenames follow the first accepted primary GPS track point and board-time
 delta, not the minimum time of the log. Eighteen leap seconds matches the current
 MP10 rule and needs maintenance if that rule changes. Qt intentionally corrects
 the legacy GPS TimeMS/time-of-week-as-board-time bug and keeps the remote name
 when no valid post-1980 anchor exists, instead of creating a year0001 filename.
- BIN→LOG is streaming, preserves FMT order, CRLF, encodings and firmware mode
 heuristics; malformed/truncated tails are skipped with warnings rather than
 fabricating MP10's zero-padded final record. KML uses the existing bounded Qt
 exporter, not a claim of byte-identical MP10 KML/GPX output.
- A rare libssh2 SSH_MSG_DISCONNECT native cleanup leak was identified in upstream
 code. Do not switch this QTcpSocket-backed session to blocking mode or free
 opaque native state with an unproven workaround. Track native teardown separately.
- Reference-pixel/HiDPI comparison, Windows/macOS, representative SSH firmware and
 live companion-computer deployment remain open. Useful legacy log tools remain.

## Reproduction and evidence

Root schedules configure/build/test with the repository's single-build lease.
The latest verified suite counts and evidence paths are in `CURRENT_STATE.md`.
Focused suites: `sftplogsession_tests`, `sftplogdownloadsupport_tests`,
`sftplogdownloadwindow_tests`, `dataflashbintologconverter_tests`, and
`sftp_log_download_runtime_tests`.

`tests/SftpHermeticServer.py` is test-only Paramiko, bound to127.0.0.1 with an
ephemeral generated host key and explicit temporary filesystem root. It records
authentication attempts without passwords; it provides no shell or forwarding.
An isolated optional Python environment needs Paramiko (tested4.0.0). Production
APM has no Python/Paramiko dependency.

```sh
build-codex-qt/sftp_real_probe /path/to/test/python tests/SftpHermeticServer.py
DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software \
  APM_SFTP_AUDIT_PYTHON=/path/to/test/python \
  APM_SFTP_AUDIT_SCREENSHOT=/tmp/sftp-evidence/sftp \
  build-codex-qt/apmplanner3 --sftp-log-download-audit
build-codex-qt/sftp_real_probe --convert /path/input.bin /path/new-output.log
```

Without the optional Python variable the CTest production audit still checks the
real action route, singleton window and actual localhost silent-SSH cancellation.
With it, it additionally drives real host-key consent, folder cancellation,
BIN/LOG/KML download without overwrite and default-Cancel/confirmed remote delete.
Audit settings and startup UDP are isolated; no network SITL or vehicle is changed.

The actual MP10 oracle used the existing net10.0 release assemblies with F#
Interactive, invoking `DataFlashDownloadPostProcessor.ProcessAsync` on copies.
`Settings._GetRunningDirectory` must point to the MP10 install directory so
`FlightModeNames.Initialize` resolves bundled metadata; otherwise the oracle
incorrectly yields numeric modes because EntryAssembly is FSI. Metadata/settings
are isolated, with fresh empty pdef cache files to suppress automatic refresh.
Evidence and script: `/tmp/apm-sftp-oracle.NecQdU/`. Actual DLL hashes:

- MissionPlanner.dll: `3effbed52c8aebc6924eb52c98920a19195852e114728e7f04c4a33ed68b2211`.
- MissionPlanner.Utilities.dll: `cbe1320f6b0e123fe1035f52164c71a6cf200d9026422d60313c5a6a8644f149`.

Five small LOG goldens match byte-for-byte; the complete real BIN emits129602
records with SHA256`e44819bbdea81319f57ab182e3d966b4a8843f883b9327f95e556a2ef94b0d71`
in both implementations. This exposed and fixed Single ties-to-even rounding.
It is representative parity evidence, not exhaustive floating-point/mode/dialect
proof; broader Double edge cases and updated firmware modes remain acceptance work.
