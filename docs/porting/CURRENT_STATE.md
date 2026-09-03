# APM Planner 3.0 current state and handoff

Updated: 2026-09-03. This is the short operational handoff for the Mission Planner 10 Qt/CMake port. Update it whenever a functional package is committed or the immediate priority changes.

## Goal and current milestone

The product goal is a cross-platform APM Planner 3.0 (`3.0.0`) that transfers the functionality and recognizable workflow of Mission Planner 10 to Qt/CMake on Linux, Windows and macOS. Existing QGroundControl and Hermes/GTU code may be reused where licensing and architecture fit.

The current milestone is **broadly usable functionality**, not final pixel parity. Small spacing, color, label and geometry differences must be recorded and deferred instead of consuming the main implementation stream. Missing or inert actions are functional gaps and remain high priority.

The complete prioritized route from the current checkpoint to functional and
release parity is maintained in `MASTER_PORTING_BACKLOG.md`. The primary stream
now ports the principal SETUP/TOOLS plugins and fills them with end-to-end
functionality. Qt Widgets and the trusted QML extension API are both available;
minor visual matching is recorded and deferred until the useful workflows exist.

The immediate user-directed order is:

1. Keep the committed PLAN row actions, multi-instance MAVLink Inspector and Antenna Tracker foundations green.
2. Continue the Antenna Tracker vertical slice: the shared serial/live pages and the exact-link `RADIO_STATUS`/legacy `RADIO` SNR consumer are implemented; next are the PLAN "Tracker Home" action and the exact-target parameter page.
3. Replace disabled Advanced/Developer actions and missing high-value SETUP/TOOLS pages with complete workflows in the master-backlog order; widen the trusted QML API where a real plugin workflow needs it.
4. Return to complete DATA HUD telemetry and CONFIG Onboard OSD after the main plugin surface is usable; do not spend the primary stream on pixel polish.
5. Track functional and GUI inaccuracies separately and keep committing complete slices rather than disconnected stubs.

## Verified checkpoint

The pre-Wave-1 functional checkpoint was `fb4f08b5` (`feat: port MinimOSD telemetry helper`), following `4290221f` (`feat: add antenna tracker output protocols`) and `e6fab89e` (`feat: port ESP8266 setup workflow`). The later verified checkpoints include `9d69cebb` (PLAN wrapper geometry), `9b180eba` (waypoint row actions), `e6d9e75b` (independent Inspector windows), `fbeacd6e` (Antenna Tracker pages) and `e7d5e58b` (their parity-ledger checkpoint).

At this checkpoint:

- CMake configure and one `cmake --build build-codex-qt -j12` completed successfully; the link-scoped radio monitor and its application integration also build in `build-claude`.
- The complete test suite passed after the radio-monitor integration: **94/94 tests**.
- Real X11 smoke tests opened the app with title `APM Planner 3.0.0 (...) — APM Planner`, opened two simultaneous Inspector windows, verified that Enter/Escape do not invoke dialog semantics, closed one independently and then closed the application cleanly with the other open.
- The recovered PLAN row actions, explicit multi-instance MAVLink Inspector lifecycle, `AntennaTrackerGeometry`, raw-serial tracker service and shared tracker pages are committed. Any later working-tree changes must be reviewed and checkpointed as their own coherent slice.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 127 product rows: 35 `in-progress`, 47 `partial`, 45 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

## Implemented foundations worth reusing

These are working foundations, though their parity rows may remain partial because visual, hardware or cross-platform evidence is outstanding:

- Mission Planner-like main shell and deterministic DATA/PLAN `QSplitter` layouts without KDDockWidgets.
- User-selectable compiled map backends through a common abstraction and one canonical filesystem tile cache.
- DATA HUD/map/telemetry surface and PLAN mission table/map/action surface.
- Typed Mission, Fence and Rally stores; mission file/transfer paths; QGC Plan and legacy fence/rally file handling.
- PLAN mission editing, context commands, store-aware undo, route geometry, polygon drawing/offset/fence conversion, Survey/Corridor import, terrain source, elevation graph and distance measurement.
- Exact `(link, system, component)` vehicle target registry, shared link transmitter, command service, parameter service and committed parameter store.
- Backstage SETUP/CONFIG infrastructure and a substantial set of hardware/parameter pages listed in the parity ledger.
- Shared trusted QML plugin engine/API (`docs/porting/QML_PLUGINS.md`); legacy binary APM Planner plugins are intentionally unsupported.
- Mission Command List editor and catalog, exposed to PLAN and QML.
- The MP10 Default Settings workflow: official frame catalog discovery/download/cache, compare/stage into the native raw-parameter editor, exact target-generation guards and operation-scoped cancellation.
- The MP10 HW ID page: `_ID`/`_DEVID` inventory, ArduPilot device-ID decoding and exact six-column sortable presentation.
- The MP10 ADSB page: metadata-backed `ADSB_`/`AVD_` parameter editing and batching, search, uAvionix flight-ID/registration read/write cadence and exact-target message filtering.
- The MP10 ESP8266 page: exact component-240 parameter loading, bytewise packed settings, 22 serialized writes, storage/reboot/reset commands and a target-safe Qt view lifecycle.
- The MP10 SETUP OSD page: the exact legacy MinimOSD telemetry helper surface and its ordered 24-parameter 2 Hz write batch, with committed-snapshot filtering, partial-vehicle reporting and exact-target transaction lifecycle guards.
- The MP10 Antenna Tracker output foundation: exact Maestro compact commands and ArduTracker/DegreeTracker text protocols with tested trim, reverse, clamp, wrap and tilt-flip arithmetic behind an injectable writer.
- Link-scoped MP10-compatible `RADIO_STATUS`/legacy `RADIO` snapshots and read-driven local/remote SNR filtering, isolated by physical `linkId` and cleared with link removal.
- Pure Antenna Tracker pointing geometry with MP-compatible AZ/EL/distance functions plus explicit validity, dateline and pole handling.
- The thread-confined Antenna Tracker raw-serial service: dedicated Qt SerialPort 8N1 transport, setup/centering pipeline, bounded latest-target-wins backpressure, write watchdog, generation-aware cancellation and fail-closed unplug/reconnect behavior.
- Visible per-row PLAN Up/Down/Delete controls that reuse the existing store-aware operations, including undo and DO_JUMP remapping.
- Explicit modeless multi-instance MAVLink Inspector windows and guarded replay multicast, with independent-close and application-shutdown coverage.
- Advanced and Developer action inventories, working shared actions/parsers, Advanced Terminal and the user-facing trusted QML plugin manager.
- Cooperative shutdown ordering, including close with the modeless inspector open, verified by the real-X11 smoke above.

## PLAN action-column audit

The direct action inventory has been compared against MP10 `FlightPlannerView.axaml`. MP10 has 24 direct layout items, including 19 interactive controls (12 buttons, two checkboxes, two combo boxes and three editors). The Qt panel has 38 direct items and 29 interactive controls, adding the working Polygon group, transfer cancellation/progress and another editor. No direct MP10 action is absent by count.

Seven Qt actions remain deliberately disabled because their end-to-end workflow is not yet ported: Grid display, View KML, WMS, WMTS, Inject Custom Map, Use MAVFTP and Write Fast. The remaining controls are working or conditionally disabled according to connection/transfer state. The lower blank area follows the same top-aligned scrolling layout behavior as MP10 and is a deferred GUI difference, not a missing dock or hidden action group.

The erroneous outer right strip is fixed separately from that legitimate inner
spacing: `DockableView::setPanelFixedExtent()` now constrains both the splitter
wrapper and its content. Production-order tests prove the exact 168-pixel
ActionPanel width, 210-pixel WaypointPanel height, full-height right column and
right-edge ownership across 1120, 1280 and 1600-pixel windows, including
hide/show and saved-layout restore.

The waypoint table now also exposes the MP10 Up, Down and Delete operations as
visible per-row controls. They select the clicked row and reuse the existing
QAction paths, so transfer locks, store-aware undo and DO_JUMP remapping remain
centralized; mouse and keyboard activation plus boundary rows are covered.

The next coherent PLAN package should combine Grid, KML preview/export, a persistent custom XYZ source using the canonical cache identity, and store-aware undo for Polygon-to-Fence conversion. WMS/WMTS and MAVFTP/Write Fast can follow later. Main work has moved to SETUP as requested.

## SETUP next phase

The SETUP phase must compare the whole MP10 navigation model with the Qt backstage model, then classify every route as working, partial, stub or missing. Prioritize pages that can be made end-to-end functional on existing exact-target/parameter/command services and already implemented widgets.

The original navigation audit found 53 MP10 pages and 28 Qt routes. The current backstage registers 36 Qt routes; this is not a one-to-one count because the trusted QML manager is a native replacement/extension surface. The SETUP `OSD` route now uses the dedicated `ConfigHWOSDView` legacy MinimOSD telemetry helper; the separate CONFIG `Onboard OSD` canvas/editor is still absent and remains incorrectly represented by the old `OsdConfig`. Many other MP10 workflows remain partial or missing.

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with three working shared tools (MAVLink Inspector, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Developer Tools exposes the exact 32-action inventory and working byte/MAVLink/hardware-ID parsers. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

The first-activation SETUP stall is fixed: `ApmCustomFirmwareConfig` yields before serial enumeration, downloads the manifest asynchronously through a dedicated reply, uses progress-based inactivity watchdogs, and keeps firmware download/upload state isolated. Flash actions retain an immutable selected-device snapshot and remain locked until a terminal upload outcome; destruction aborts replies and stops uploader timers. The still-large uncompressed manifest, GUI-thread JSON parsing, serial enumeration and complete MP10 firmware-selector parity remain explicit follow-up work.

`ESP8266 Setup` is now routed after Parachute and uses a dedicated exact-link client for `MAV_COMP_ID_UDP_BRIDGE` instead of changing the globally selected autopilot component. It waits for the 18 MP10 settings, displays the exact control set/defaults, serializes all 22 parameter writes and ACK-gates storage, reboot and reset. Unit/UI/transport coverage includes wrong-link replies, retry, partial response, target invalidation, reentrant cancellation and destruction; live bridge hardware, profile gating and cross-platform visual evidence remain.

The next audited SETUP packages are CubeID and Secure. CubeID is a target-aware firmware updater, not another HW-ID presentation; its CubePilot messages are absent from the current generated dialect and require a coordinated MAVLink update. `ConfigSecureView` manages bootloader public-key slots with `SECURE_COMMAND`, while `ConfigSecureApView` generates Ed25519 keys and signs bootloader/firmware files; MAVLink link signing remains a separate Advanced Tools workflow. The security pages need a reviewed cross-platform Ed25519 dependency before implementation.

The three Antenna Tracker routes are audited in `SETUP_ANTENNA_TRACKER_AUDIT.md`. The `Antenna Tracker (Serial)` and `Antenna Tracker (Live)` routes now exist after `ESP8266 Setup`: one shared `AntennaTrackerUIViewModel` (owned by `SetupView`, so the tracker loop survives page resets and navigation like MP10) drives `ConfigAntennaTrackerView` and `AntennaTrackerUIView` on top of `AntennaTrackerSerialService`, `AntennaTrackerGeometry` and a `UasAntennaTrackerTelemetrySource`; `antennatrackeruiviewmodel_tests` (11 cases) and `antennatrackerviews_tests` (4 cases) cover MP10 texts, validation order, settings, the loop, manual mode, live trim/reverse, failure recovery, the trim sweep and shutdown. `RadioStatusMonitor` now consumes both MAVLink radio-statistics messages per physical link and reproduces MP10's raw-unit SNR formula, 50 percent read-driven EMA and one-second hold; the telemetry source reads only the current exact target's link. The pages were smoke-tested on real X11 under Xvfb with a local SITL: both routes render, a real `QSerialPort` connect to a local UART reached `Connected (Maestro).`, Vehicle Az updated live and the application closed cleanly while connected. Deviations SETUP-018..022 record the shared instance, the Home / Center quirk, the intentional cancellable trim state machine, gating/tracker-home gaps and GUI approximations. The active sequence is the PLAN "Tracker Home" action, then the exact-target 25-field parameter page and remaining route/profile gates. The old `AntennaTrackerConfig` remains until these replacements are routed and verified.

Legacy APM Planner binary plugins are not a requirement. Where MP10 calls something a plugin/page/tool, reproduce the user-visible function with a native Qt page/service or the trusted QML extension system; do not restore the old ABI.

## Architecture constraints

- Product baseline is Qt 5 with a forward-compatible Qt 6 path; do not copy Qt 6-only QGC UI dependencies into the required core.
- All mission, terrain, distance and altitude data remains canonical SI internally; unit preferences convert only at presentation boundaries.
- All map backends share the same provider identities and canonical cache root.
- New transport work must retain exact link/system/component identity, target generations, ACK correlation and cancellation on target change.
- Dense editors/shell/core stay in C++/Qt Widgets. QML is appropriate for plugins and dynamic tools and receives direct trusted core access.
- DATA and PLAN are fixed in-page layouts; no floating/docking framework is required.
- Fresh 3.0 namespace means no legacy settings, layout or plugin compatibility work unless a current file format is itself part of Mission Planner interoperability.

## Build and verification safety

The coordinating agent owns the build schedule. A delegated agent may build only while holding an explicit lease for one exact command. Use one build process globally across all workspace repositories, maximum `-j12`; never use a bare `--parallel` or `-j`, and do not modify source files until the leased build finishes.

Immediately before every configure or build, execute this exact standalone command:

```sh
pgrep -af '(^|/)(cc1plus|clang\+\+|g\+\+|c\+\+)( |$)' || true
```

Do not combine it with the configure/build command. Do not launch if other compilers are active. On 2026-09-02 a separate GTU build used bare `cmake --build ... --parallel`; GNU Make expanded it to unbounded jobs, the OOM snapshot contained 153 `cc1plus` processes using roughly 13 GiB resident memory, and the desktop session failed. Always supply the numeric limit even when only one agent appears active.

For each broad slice, perform proportionate unit/integration tests, then a single full build and full test pass. UI/navigation/shutdown changes also require a real-X11 smoke when available. Commit the verified slice and update this handoff.

## Source-of-truth files

- `AGENTS.md`: operational constraints every agent must follow.
- `PORTING_PLAN.md`: architecture and acceptance contract.
- `MISSION_PLANNER_SCREEN_PARITY.tsv`: authoritative full screen/function inventory.
- `PORTING_DEVIATIONS.tsv`: separate functional/GUI discrepancies and disposition.
- `DOCKING.md`: fixed DATA/PLAN panel decision.
- `QML_PLUGINS.md`: trusted QML extension API and policy.

Reference source trees are listed in the root `AGENTS.md`.
