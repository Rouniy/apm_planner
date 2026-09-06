# Convert Shapefile to POLY

The existing SETUP / Developer Tools action (also reached from the main Tools
menu) converts a local shapefile without connecting to a vehicle. This is a
single-drone/offline workflow; it does not expand Swarm work.

## Contract and reference

The active MP10 reference is `Services/PlannerMapImportService.cs`
(`ReadSource`, `ReadProjection`, `Transform`, `ExportPolyFiles`, `WritePolyAtomic`)
and `ConfigDeveloperToolsViewModel.ConvertShapefileToPoly`. The bundled
NetTopologySuite.IO.Esri.Shapefile 1.2.0 reader was independently decompiled to
settle geometry ordering and DBF semantics. The initial Claude c271 assumptions
about deleted records and ring grouping were corrected, not adopted as parity.

One `poly-N.poly` is written beside the selected SHP for each non-empty feature
after WGS84 filtering. Empty/deleted features do not consume a number. Files
contain UTF8 without BOM, CRLF, the reference's exact misspelled header
`#Shap to Poly - Mission Planner`, then latitude, a tab, and longitude. Decimal
formatting is shortest invariant (including negative zero); closing vertices
and flattened multipart/hole order are retained. Older higher-numbered files
are deliberately not deleted. Point and line exports are supported even though
the separate PLAN polygon importer requires a valid polygon with at least
three distinct vertices; conversion does not silently impose PLAN's limits.
Qt and .NET may choose different exponent spelling/notation for extreme small
values; numeric round-trip, not every possible textual representation, is the
contract beyond the exact tested decimal fixtures.

Point, MultiPoint, PolyLine and Polygon XY/M/Z layouts are read from bounded
byte snapshots. SHX is neither required nor restored. No OGR shapefile driver
is opened: its grouping and index-restoration semantics differ. Polygon groups
use the reference's first-part shell, following CCW holes and next-CW new-shell
rule, then native GDAL/GEOS WKB validity. Missing validity support fails before
writing rather than silently exporting unchecked polygons. Z is used for
reprojection; M is not exported. A complete optional M block may be absent from
Z shapes as allowed by the ESRI layout and GDAL reader; partial blocks fail.

DBF III row values are validated even for deleted rows, as in NTS, before the
paired feature is omitted. A shorter DBF limits SHP enumeration; a longer DBF
fails the one-to-one record contract. An unexpected record shape type consumes
the paired row but is skipped with a warning. The CPG fallback currently uses
UTF8 rather than the DBF language-driver byte: attributes are discarded, but
exotic field-name decoding/duplicate validation can differ. The top-edge CCW
orientation determinant uses `long double`, not NTS double-double arithmetic;
near-collinear pathological rings remain a numerical parity limit, followed by
GDAL/GEOS validity checking.
Numeric thousands separators and extreme .NET Decimal parsing are not fully
replicated for discarded DBF attributes. Version 3, header-date, field-type/name
and ordinary numeric/date-value validation do follow the decompiled reference;
they are not arbitrary filters added because attributes are unused. A shorter
DBF now warns that remaining SHP records are not enumerated. Native GEOS
availability is checked with its version API when present and a known-valid
square self-test, so an unusable runtime is not reported as an invalid polygon.

## Projection and deployment

Case-insensitive same-stem PRJ/DBF/CPG sidecars are discovered before reading.
An absent/empty PRJ treats XY as WGS84 longitude/latitude and warns that the
actual datum is unverified. Nonempty PRJ must be WKT, not an authority string,
URL or arbitrary PROJ program. GDAL OSR imports/morphs/validates ESRI WKT and
obtains the display name. A separate PROJ context reads the original validated
WKT and normalizes CRS axes before constructing the WGS84 operation, with
`ALLOW_BALLPARK=NO` and `ONLY_BEST=YES`.

An actual regression exposed a lossy GDAL-WKT2-PROJ round trip for a non-Greenwich
frame with explicit zero TOWGS84 parameters: WKT2 reparsing changes the bound
transformation source's datum interpretation, losing its usable strict
operation. The original WKT1 preserves it. Parsing the original validated text
avoids serialization rather than adding a permissive fallback. Tests cover
ambiguous-frame rejection and explicit Paris/Rome/Lisbon transformations, plus
UTM north/south, Web Mercator, Lambert, Albers, angular units and a 100 m datum shift.

The private context disables network and grid cache. No process-global
`OSRSetPROJEnableNetwork`, environment, default context or global error handler
is changed. Missing required grids or unavailable datum operations fail;
invalid/out-of-domain coordinates are counted and omitted. A different datum
is never assumed to be a zero-shift identity simply because it is geographic.
GDAL/PROJ is not DotSpatial: supported WKT variants, operation selection,
grid availability and numerical results may differ. No survey-accuracy or
coordinate-epoch parity claim is made.
GDAL may normalize the displayed CRS name instead of retaining DotSpatial's
ESRI spelling. Invalid Unicode PRJ input is refused rather than substituted;
UTF8 and BOM-tagged UTF16 are supported. A non-Greenwich CRS without a usable
explicit datum operation may expose only a ballpark operation and is refused,
not silently longitude-shifted by an ad-hoc fallback. Missing grid/no-operation
errors abort the prepared export; they are not treated as disposable points.

GDAL with OSR/GEOS support and PROJ >=9.2 plus local `proj.db`/needed grids are
optional runtime dependencies, loaded through `QLibrary`. The GDAL candidate
policy is shared with ElevationSourceService. Explicit first candidates are
`MISSIONPLANNER_GDAL_LIBRARY` and `MISSIONPLANNER_PROJ_LIBRARY`; normal platform
names remain fallbacks. Plain no-PRJ point/line conversion does not require
either library. Packaging on Windows/macOS must ship compatible runtime/data
dependencies and remains a separate validation gate.

Primary ABI/layout references:

- GDAL 3.8.4 `ogr/ogr_srs_api.h`, `ogr/ogr_api.h`, `ogr/ogrsf_frmts/shape/shpopen.c`.
- PROJ 9.4.0 `src/proj.h`; direct pointer-array transformation avoids by-value
  `PJ_COORD` ABI assumptions.

## Review, publication and cancellation

The asynchronous picker starts read-only preparation. The worker parses and
transforms all features, freezes exact output bytes and snapshots sources and
destinations. A scrollable confirmation lists every exact path, Create/Replace,
point counts, source, directory, projection and warnings. Cancel is default and
Escape; only **Convert and replace** authorizes publication. No-output plans
finish without a destructive confirmation.

Inputs/sidecars are SHA256-rechecked before the first publication. Outputs must
still match the reviewed absent/ordinary-file state. Each output uses QSaveFile
with direct-write fallback disabled, guarded before staging chunks and commit.
Linked/non-regular outputs are refused because QSaveFile follows symlink
targets. Existing hardlinks are replaced through their one directory entry,
not modified in place. A source must have `.shp` extension, preventing a typed
`poly-N.poly` source from being overwritten by its own output.

Cancellation/failure is per-file atomic, not a transaction across all outputs:
already published files remain and results report actual published paths and
point counts. Only the current owned QSaveFile temporary is discarded. No
wildcard cleanup or deletion of stale files occurs. A completed export remains
successful if cancellation arrives after all commits. Page close cancels only
this controller's operation; workers capture value state, not QWidget pointers.
The shared Developer gate covers picker, preparation, confirmation and export.

These checks are not filesystem locks. Cancellation/progress callbacks are
trusted observers, not filesystem mutation hooks. Input SHA is checked once at
execution admission; later guards use type/path/size/mtime/birth stamps, not
repeated whole-input hashing. Same-stamp rewrites, changes/new sidecars after
admission, and the final check/rename race are outside the concurrent-writer
contract. Stop other writers before conversion. No exclusive reservation or
automatic rollback claim is made.

## Explicit bounds and differences

- SHP 256 MiB; DBF 64 MiB; PRJ 64 KiB; CPG 4096 bytes; 20000 features,
  two million points; 128 MiB prepared output; 200000 directory entries.
- Ambiguous case-insensitive sidecars, symlinked source/sidecars and unsafe
  outputs fail, unlike MP10's unspecified first match/link-following behavior.
  Ordinary ancestor aliases resolve once to the canonical source directory.
- Bounds, version/layout/truncation checks deliberately reject malformed data
  for which the reference reader can ignore short reads or trailing bytes.
  Native geometry validation cannot be interrupted inside a single GEOS call.
- All transforms happen before consent/publication. MP10 consents first, reads
  source geometry, then transforms and writes per feature. Qt therefore refuses
  a late transformation failure before replacing any file.
- Qt read-only-destination refusal and native dialogs/layout differ from .NET/
  Avalonia. The exact-path table is an intentional additional safety surface;
  final pixel matching and native-platform verification are not complete.

## Verification checkpoint

Qt5/audio configure and final build pass. Focused11/11 (28.92s), full272/272
(48.42s), ten repeats of each of4 suites (15.76s) and production X11 exit0/
zero failures pass. Actual navigation verifies default/Escape Cancel, exact
Create/Replace outputs, eight written points, unchanged sources/no SHX creation
and PLAN round-trip into two three-vertex polygons. The controller's10 cases
also pass under X11 (2.163s). Root inspected both final screenshots.

Evidence lives in `/tmp/apm-shapefile-poly.SLKkvi/`: configure.log, build3.log,
focused3.log, full.log, repeat.log, x11-final.log, controller-x11.log,
prime-meridian-oracle.log, consent.png and consent.png.page.png. No real vehicle
or source data was changed. Final verification details are in CURRENT_STATE.md.
