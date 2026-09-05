# Embed Defaults in APJ

The existing `EmbedDefaultsInApjButton` in SETUP / TOOLS Developer Tools now
opens a working offline workflow. It does not send MAVLink, modify the connected
vehicle, flash firmware or sign an image.

## Reference and implementation

MP10 `ConfigDeveloperToolsViewModel.EmbedDefaultsAsync` and `apj_tool.Process`
define two file selections and the exact output name: firmware pathname plus
`new.apj` (for example `copter.apjnew.apj`). Both Qt pickers expose the relevant
extensions and an All files fallback; the backend validates content, not suffix.
The output has no third Save picker. An existing output requires an explicit
path-specific Yes/Cancel confirmation with Cancel as default and Escape action.

Named asynchronous dialogs are `DeveloperApjFirmwareDialog`,
`DeveloperApjDefaultsDialog`, `DeveloperApjOverwriteConfirmDialog` and modeless
`DeveloperApjProgressDialog`. A worker captures immutable paths/options and
shared atomic cancellation/progress state, never a widget. Close/destruction
requests cancellation. Shared admission prevents overlap with GPS extraction,
DataFlash splitting, DashWare export, direct MAVFTP and vehicle actions.

`ApjDefaultsEmbedder` handles the local transformation:

- APJFWv1 JSON, unique decoded root keys, strict Base64 and one complete zlib
  stream. Bounds: 96 MiB JSON, 64 MiB inflated image, 1 MiB parameter input.
  `image_size` must match the actual image; a present `flash_total` must fit it.
- One exact packed `PARMDEF\0` plus eight-byte magic, unsigned little-endian
  capacity/active length and a complete reserved region. ASCII text is preserved
  in source order, all CRs removed, BOM-tagged UTF-8/16/32 decoded without lossy
  replacement. Empty input clears the active length. Non-ASCII/control content
  is refused, including non-ASCII comments; no parameter semantics are inferred.
- Only active defaults bytes and their length change. Inactive reservation
  bytes remain intact, matching both MP10 and ArduPilot's `apj_tool.py`; this is
  not erasure of old defaults. Unknown JSON value tokens remain byte-exact,
  including integers beyond 2^53. `image_size` is unchanged; `flash_free` is
  recalculated only when `flash_total` exists. External-image fields are retained,
  not decoded or patched; defaults located only in external flash are unsupported.
- An unsigned APP_DESCRIPTOR must be unique, complete, non-overlapping and
  describe the full image. Its original CRCs are verified before modification;
  both CRC fields are recalculated afterward. ArduPilot uses reflected
  0xEDB88320, initial zero, no final XOR, over `[0, descriptor+8)` and
  `[descriptor+24, image_size)`. This follows `chibios.py::set_app_descriptor`
  and `AP_CheckFirmware.cpp`, fixing an omission in MP10's simple marker patch.
- Signed JSON markers and signed binary descriptors are refused. Images without
  a descriptor remain supported, with an explicit warning that boot compatibility
  has not been verified. A pre-existing CRC mismatch is not silently repaired:
  the error asks for the original downloaded firmware.
- Source files and a confirmed existing output are hashed and rechecked before
  publication. QSaveFile has no direct-write fallback. New output is staged in
  a private sibling directory and published with QFile's no-overwrite rename;
  confirmed replacement uses QSaveFile commit. Cancellation and failures before
  publication preserve the source and do not publish a new output. Paths are
  canonicalized; output symlinks/directories and obvious aliases are refused.

## Verification and limits

Backend tests independently use zlib's complemented CRC API as an oracle, cover
both CRC regions, unsigned 16-bit capacity, BOMs, metadata precision, malformed
JSON/Base64/zlib/defaults/CRC, signing refusal, size caps/decompression bombs,
cancellation, source/output mutation, overwrite, collisions and symlinks.
Widget tests cover both pickers, overwrite default-Cancel, worker results,
signed refusal, cancellation/lifetime and operation interlocks. The production
runtime audit drives the actual Tools route with isolated settings and an
in-process fixture, never the network SITL; it checks output bytes independently.

Qt5/audio configure/build, focused7/7 (8.23s), full244/244 (30.22s) and production
X11 exit0 with zero audit failures pass. Existing GPS/Split/DashWare/MAVFTP,
browser target-consent and six vehicle-action checks also remain green. The
initial runtime failed because repeated `findChild` selected a hidden old
WA_DeleteOnClose picker. The corrected harness selects visible dialogs and
drains deferred deletes before the repeated workflow; no production latch fix
was needed. Native dialog/reference screenshot parity is not claimed.

Independent probe evidence lives in `/tmp/apm-apj-defaults.pwtEdH/`. A copied real
CubeOrange image (1,525,504 bytes; defaults offset 1,359,896, capacity 8192) accepts
80 bytes of defaults and preserves all other binary bytes and unrelated JSON.
A separate 16,384-byte unsigned fixture verifies both repaired CRCs with a Python
oracle. Signed, cancelled and missing-defaults fixtures publish no output.

This tool does not validate parameter names, firmware parser line lengths,
values, board compatibility, cryptographic provenance or physical bootability.
It buffers bounded images/JSON; cancellation is cooperative, not a rollback
after atomic publication. Path/hash checks are not an exclusive hostile-process
filesystem lock or a power-loss durability guarantee. Reference screenshot,
high-DPI, native Windows/macOS and physical firmware evidence remain separate
gates. No firmware was flashed in this slice.

Claude TCP c204/c205/c206 independently reviewed the reference format, code and bootloader
policy; three Codex streams implemented backend/tests, UI/tests and runtime.
The coordinator reviewed the code, integrated CMake/probe and scheduled builds.
