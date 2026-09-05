# Advanced Tools — Param gen

Reference: MP10 `ConfigAdvancedViewModel.RegenerateParameterMetadataAsync`,
`ParameterMetaDataParser`, and the APM/PDEF metadata repositories. This action
is separate from the existing automatic current-firmware PDEF updater.

## Native workflow

The shared Advanced action opens a real modeless Parameter Metadata Generator.
Regenerate uses the exact MP10 confirmation, defaulting to Cancel. One
application-owned service downloads the nine reference C++ roots (five master,
four stable), traverses their annotated library dependencies, generates a
five-family fallback catalog, and force-refreshes all nine prepared products:
SITL, AP_Periph, ArduSub, Rover, ArduCopter, ArduPlane, AntennaTracker, Blimp,
Heli. There are no vehicle reads, parameter writes, subprocesses or C++ execution.

Progress, bounded logs, per-artifact results and explicit cancellation are
visible. Closing the observer leaves the application-owned job running; reopening
restores its snapshot. Application shutdown aborts outstanding network replies.
Terminal success waits for all work, unlike MP10's unawaited PDEF Task.

Artifacts live under `cache/parameter-metadata/regenerated` in the writable
application data directory: `generated-source.pdef.xml` and `<Product>.pdef.xml`.
The native PDEF representation intentionally replaces MP's legacy generated XML
format. Successful files are individually replaced with QSaveFile. An incomplete
required source graph never replaces the old generated artifact. Partial PDEF
success and cancellation retain and report any files already published; this is
not an all-or-nothing ten-file transaction.

## Parsing and network contracts

The pure parser preserves unknown metadata, conditional vehicle annotations,
ordered duplicate precedence, group prefixes, comma-separated paths, and nested
class stems. Later matching conditional fields override earlier ones. Root order
makes master definitions win, with stable definitions filling missing names.
Traversal order is deterministic, independent of HTTP completion order.

Intentional corrections include excluding other-vehicle conditional fields,
correct Tracker aliases, stopping metadata blocks at the next Param or Group,
preserving multiple anonymous groups and empty fields, and processing all nested
macros. Anonymous groups are needed by current AP_Vehicle declarations. Explicit
self references are harmless; multi-file ancestry cycles fail closed. Missing
inferred nested siblings can be optional when HTTP returns 404: real ArduPlane
sources provide the base library separately through explicit @Path annotations.

Three downloads run concurrently, with finite request timeouts/retries, per-file
and total source limits, graph depth/node/URL bounds and fresh per-run caches.
Recursive `.cpp` and `.h` paths stay within the exact allowlisted repository ref.
Arbitrary URLs and redirects are not followed. Prepared products use official
plain XML instead of gzip. Rover uses the canonical APMrover2 transport path;
the stable source root uses APMrover2-stable because MP10's Rover-stable URL
returns HTTP 404. These are fixed official aliases, not arbitrary redirect trust.
Heli validates the real Helicopter section. SITL is a supplemental library
catalog: its current official XML contains a Blimp vehicle section, which is
ignored rather than imported as SITL vehicle parameters.

## Safe application and explicit remaining gaps

Repository lookup retains exact versioned PDEF priority, then the newest valid
ordinary/latest or regenerated product, then packaged/resource metadata. Missing
fields fall back through SITL, AP_Periph and generated-source data. Explicit false
or empty fields are not overwritten by lower-priority data. A directory revision
invalidates both Setup and Config repository caches lazily after publication.
Previously returned page snapshots remain unchanged: no page reset, target switch,
parameter-list reload, calibration cancellation or staged-edit mutation is used.
Save/apply edits and finish operations before reconnecting/restarting to refresh
already-created parameter pages. Navigation alone can reuse a cached page.

Range provenance is currently catalog-wide. If unversioned fallback introduces
any range, the merged catalog becomes advisory, even when other exact-firmware
ranges remain intact. This conservative limitation must be replaced with
per-field provenance across all editors before strict metadata parity.

Blimp/Heli products are validated and cached, but the current active-vehicle
metadata facade still has five firmware-family mappings. A separate Heli/Blimp
selection policy, MP's ParameterMetaDataLocal.xml override, original legacy XML
interchange, asynchronous hot-refresh of safely idle existing pages, final visual
matching and native Windows/macOS evidence remain outside this slice.

## Verification

Focused parser, service, observer and repository regressions cover the new
contracts. The production Setup route audit additionally checks offline
Advanced/Basic/Custom visibility and an isolated Plane connect/disconnect cycle
before its independent 53-route factory inventory. It checks the actual Param gen
action, modeless surface, singleton observer and application-owned service without
starting a download implicitly.

Build, full-suite and real-network/X11 results are recorded in CURRENT_STATE.md
after execution: 216/216 tests pass, the manual probe and production GUI both
publish all ten artifacts, cancellation preserves the previous source hash,
and application shutdown during another active job exits 0. Evidence is under
`/tmp/apm-paramgen.h8Bphz/`. `parameter_metadata_regeneration_probe --download <cache>`
is an explicit manual official-network probe, not part of CTest.
