# Geo Reference Images

## Scope and route

DATA → DataFlash Logs → Geo Reference Images opens one independent modeless
`GeoRefWindow`, with no vehicle or connection prerequisite. It is not a blank
embedded widget and is not an ordinary flight-track export. All eight actions
in this DATA hub now have implementations; this does **not** mean all Tools,
SETUP, DATA tabs or Settings have reached parity.

The functional reference is MP10 `ViewModels/GeoRefViewModel.cs`,
`Views/GeoRefView.axaml` and its window. The DATA action calls `OpenWith()` with
no cached log prefill. The executable oracle uses the actual shipped
`bin/Release/net10.0` assembly, not a transcription of its matching algorithm.

## Workflow

Select a DataFlash BIN/LOG and a nonrecursive JPEG/TIFF photo directory. CAM,
TRIG and time-offset modes, GPS2, shutter lag, AMSL/preferred GPS altitude,
base altitude adjustment and Estimate Offset are represented. EXIF capture
time is DateTimeOriginal, falling back to DateTimeDigitized; ordinary EXIF
wall time uses the host's local timezone, as in the reference.

CAM pairs time-sorted photos and records by position, warning and using the
shorter prefix on count mismatch. TRIG stops on a count mismatch. Offset mode
subtracts camera-minus-log seconds and selects GPS within five seconds, with
the later record winning an equal-distance tie. GPS and interleaved ATT obey
source order. Camera correction applies shutter lag before optional altitude
selection; base adjustment is last. Estimate uses CAM only in CAM mode, GPS
otherwise, and the reference's median of the first four/last three unique
indices of the shorter list. It is a suggested offset to review, not a proof
that the camera clock or photo/trigger correspondence is correct.

The reference's direct CAM/TRIG GPS-week clock subtracts 17 seconds whereas
the DataFlash GPS clock subtracts 18. This historical one-second inconsistency
is deliberately retained for compatibility, not asserted as correct GPS/UTC
conversion for arbitrary dates. No-GPS ancient-date/timezone fallbacks retain
the separately documented Qt/.NET limits of the shared DataFlash clock.

Read-only Prepare freezes log and photo hashes, the selected options, matches,
output directory and exact output paths. The default-Cancel dialog shows that
plan. Only confirmation starts writes. Outputs are `location.txt`,
`location.kml` and `<photo-stem>_geotag.<extension>`. Text reports include the
photo extension; KML placemark names omit it and include UTC timestamps and an
absolute-altitude path. An optional output directory is a Qt addition; default
is `<photo directory>/geotagged`.

## Files, cancellation and limits

No original is edited and no existing destination is replaced. Each output
uses owned staging, checked source identity/content and new-only publication.
Completed outputs remain on later error/cancellation; the UI lists exact
published and failed paths. Closing either the dialog or application cancels
and drains work. MainWindow requests both GeoRef and DataFlash-controller
cancellation immediately if both are active. There is one worker per dialog,
no QWidget captured by a worker, and no vehicle traffic.

Sources must be stopped files. Hash/path/identity checks are not an exclusive
filesystem lock, authentication or protection against arbitrary concurrent
same-process code. Cross-platform filesystem/link behavior and abrupt process
termination recovery remain separate gates. No durable job history is claimed.

JPEG is copied without pixel decoding or recompression; its EXIF APP1 is
replaced or inserted. Classic TIFF retains existing bytes and offsets, appends
GPS/replacement IFD0 and changes the header root pointer. Both endian variants
are supported. Orientation, MakerNote, thumbnail chains and other metadata
are preserved rather than deliberately stripped as in MP10. Old inactive GPS
bytes may remain: this is **not** a location-privacy scrubber. GPSVersionID and
GPSAltitudeRef are written, so negative altitude works where reference EXIF
publication fails. DMS/rational encodings differ; compare decoded coordinates,
not tag bytes. Empty private output devices are required; source hardlink aliases,
observer-induced cursor/size mutation and changed native file identity are refused.

Bounds fail explicitly: 1 GiB log, 20,000 photos, 16 GiB aggregate photos,
2,000,000 relevant log records; classic TIFF uint32 offsets/source size with
append headroom, 16 MiB metadata-read budget, 256 IFDs, 16 nested levels,
65,536 entries and JPEG APP1 payload at most 65,533 bytes. BigTIFF is not supported.
Structural corruption, duplicate tags, unsupported TIFF types and out-of-range
pixel/thumbnail offsets are refused, even where the reference is more lenient.
Nonstandard EXIF date spellings/types are not yet all accepted. Malformed old
GPS values must not discard an otherwise valid capture time. Image-codec and
vendor-specific MakerNote semantics are not validated by the metadata writer.

The MP10 picker advertises TLOG, but its current GeoRef view-model uses
DFLogBuffer for the path. Qt explicitly reports TLOG unsupported; it does not
claim the old WinForms TLOG/video/footprint/maps workflow is ported. Useful
legacy modules remain separate. Extension matching is case-insensitive in Qt,
unlike the reference's lowercase Linux directory patterns.

## Verification

Evidence directory: `/tmp/apm-georef-port.ew63oT/`.
Qt5/audio build3 and full308/308 pass (53.26s); four related suites each pass
ten repeats (8.21s). Production X11 has zero failures; native window10/10
passes (798ms). Root inspected the GeoRef window and exact-consent screenshots.
Eleven actual matching/report scenarios (ten UTC plus Asia/Nicosia) and three
estimates pass;65 tagged photos have independent decoded-pixel/selected-EXIF
checks, exact publication receipts and unchanged source SHA256. Two comparator
rejection tests pass. Fixtures here are valid synthetic ASCII LOGs, not a
claim of universal BIN/real-camera equivalence. Initial failed runs are kept:
F# dictionary enumeration in the harness, a closed-window reopening defect,
a wrong sparse-GPS test expectation and a no-op source-mutation test fixture
were corrected before the final green run.

Reusable tools:

- `tests/oracles/GenerateGeoRefFixtures.py`: real JPEG, little-/big-endian TIFF,
  EXIF date/orientation/MakerNote and DataFlash fixtures (Pillow).
- `tests/oracles/GeoRefOracle.fsx`: calls actual MP10 matching/corrections,
  offset estimation, report and EXIF workflows in isolated copied inputs;
  records assembly hashes, timezone, options, inputs and results.
- `tests/GeoRefProbe.cpp` and `tests/oracles/CompareGeoRef.py`: Qt execution and
  independent Pillow/XML verification of matching, reports, image pixels and
  preserved metadata. Numeric tolerances are explicit, not bit-exact claims.
- EXIF/service/window Qt tests and production DATA runtime audit: real pickers,
  estimate, default-Cancel, EXIF/report publication, repeat no-overwrite,
  standalone close and simultaneous offline-worker MainWindow shutdown.

Native Windows/macOS, broader camera-file compatibility, reference visual
comparison and full Tools parity remain open. The inventory stays in-progress.
