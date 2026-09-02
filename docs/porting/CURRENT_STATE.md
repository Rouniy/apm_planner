# APM Planner 3.0 current state and handoff

Updated: 2026-09-02. This is the short operational handoff for the Mission Planner 10 Qt/CMake port. Update it whenever a functional package is committed or the immediate priority changes.

## Goal and current milestone

The product goal is a cross-platform APM Planner 3.0 (`3.0.0`) that transfers the functionality and recognizable workflow of Mission Planner 10 to Qt/CMake on Linux, Windows and macOS. Existing QGroundControl and Hermes/GTU code may be reused where licensing and architecture fit.

The current milestone is **broadly usable functionality**, not final pixel parity. Small spacing, color, label and geometry differences must be recorded and deferred instead of consuming the main implementation stream. Missing or inert actions are functional gaps and remain high priority.

The immediate user-directed order is:

1. Treat the PLAN right-column audit as complete: preserve its working controls and defer its seven named advanced gaps to coherent PLAN packages.
2. Concentrate on SETUP: required pages, tabs, plugins and tools, reusing already implemented Qt services/widgets wherever possible.
3. Track functional and GUI inaccuracies separately in `PORTING_DEVIATIONS.tsv`.
4. Continue committing complete vertical slices rather than batches of disconnected UI stubs.

## Verified checkpoint

The latest functional checkpoint is the commit containing this handoff, following `590a820d` (`feat: port Mission Planner setup tools and defaults`), `de7fd269` (`feat: add Mission Planner command catalog editor`) and the broad parity checkpoint `b79e2f73`.

At this checkpoint:

- CMake configure and one `cmake --build build -j12` completed successfully.
- The complete test suite passed: **83/83 tests**.
- Real X11 smoke tests opened the app with title `APM Planner 3.0.0 (...) — APM Planner`, switched to SETUP without waiting for the firmware manifest, rendered the connected ADSB page, and closed the main window cleanly while SETUP network work was active.
- The worktree was clean after the commit.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 127 product rows: 30 `in-progress`, 49 `partial`, 48 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

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
- Advanced and Developer action inventories, working shared actions/parsers, Advanced Terminal and the user-facing trusted QML plugin manager.
- Cooperative shutdown ordering, including close with the modeless inspector open, verified by the real-X11 smoke above.

## PLAN action-column audit

The direct action inventory has been compared against MP10 `FlightPlannerView.axaml`. MP10 has 24 direct layout items, including 19 interactive controls (12 buttons, two checkboxes, two combo boxes and three editors). The Qt panel has 38 direct items and 29 interactive controls, adding the working Polygon group, transfer cancellation/progress and another editor. No direct MP10 action is absent by count.

Seven Qt actions remain deliberately disabled because their end-to-end workflow is not yet ported: Grid display, View KML, WMS, WMTS, Inject Custom Map, Use MAVFTP and Write Fast. The remaining controls are working or conditionally disabled according to connection/transfer state. The lower blank area follows the same top-aligned scrolling layout behavior as MP10 and is a deferred GUI difference, not a missing dock or hidden action group.

The next coherent PLAN package should combine Grid, KML preview/export, a persistent custom XYZ source using the canonical cache identity, and store-aware undo for Polygon-to-Fence conversion. WMS/WMTS and MAVFTP/Write Fast can follow later. Main work has moved to SETUP as requested.

## SETUP next phase

The SETUP phase must compare the whole MP10 navigation model with the Qt backstage model, then classify every route as working, partial, stub or missing. Prioritize pages that can be made end-to-end functional on existing exact-target/parameter/command services and already implemented widgets.

The original navigation audit found 53 MP10 pages and 28 Qt routes. The current backstage registers 35 Qt routes; this is not a one-to-one count because the trusted QML manager is a native replacement/extension surface. The onboard OSD route remains miswired to the legacy telemetry-rate page, and many MP10 workflows are still absent or partial.

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with three working shared tools (MAVLink Inspector, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Developer Tools exposes the exact 32-action inventory and working byte/MAVLink/hardware-ID parsers. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

The first-activation SETUP stall is fixed: `ApmCustomFirmwareConfig` yields before serial enumeration, downloads the manifest asynchronously through a dedicated reply, uses progress-based inactivity watchdogs, and keeps firmware download/upload state isolated. Flash actions retain an immutable selected-device snapshot and remain locked until a terminal upload outcome; destruction aborts replies and stops uploader timers. The still-large uncompressed manifest, GUI-thread JSON parsing, serial enumeration and complete MP10 firmware-selector parity remain explicit follow-up work.

The next audited SETUP packages are CubeID and Secure. CubeID is a target-aware firmware updater, not another HW-ID presentation; its CubePilot messages are absent from the current generated dialect and require a coordinated MAVLink update. `ConfigSecureView` manages bootloader public-key slots with `SECURE_COMMAND`, while `ConfigSecureApView` generates Ed25519 keys and signs bootloader/firmware files; MAVLink link signing remains a separate Advanced Tools workflow. The security pages need a reviewed cross-platform Ed25519 dependency before implementation.

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

The coordinating agent owns the build schedule. A delegated agent may build only while holding an explicit lease for one exact command. Use one build process globally, maximum `-j12`; no other agent builds until the lease is released.

Immediately before every configure or build, execute this exact standalone command:

```sh
pgrep -af '(^|/)(cc1plus|clang\+\+|g\+\+|c\+\+)( |$)' || true
```

Do not combine it with the configure/build command. Do not launch if other compilers are active. This rule exists because simultaneous agent builds previously created roughly 30–40 compiler processes and exhausted system memory.

For each broad slice, perform proportionate unit/integration tests, then a single full build and full test pass. UI/navigation/shutdown changes also require a real-X11 smoke when available. Commit the verified slice and update this handoff.

## Source-of-truth files

- `AGENTS.md`: operational constraints every agent must follow.
- `PORTING_PLAN.md`: architecture and acceptance contract.
- `MISSION_PLANNER_SCREEN_PARITY.tsv`: authoritative full screen/function inventory.
- `PORTING_DEVIATIONS.tsv`: separate functional/GUI discrepancies and disposition.
- `DOCKING.md`: fixed DATA/PLAN panel decision.
- `QML_PLUGINS.md`: trusted QML extension API and policy.

Reference source trees are listed in the root `AGENTS.md`.
