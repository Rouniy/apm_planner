# APM Planner 3.0 current state and handoff

Updated: 2026-09-02. This is the short operational handoff for the Mission Planner 10 Qt/CMake port. Update it whenever a functional package is committed or the immediate priority changes.

## Goal and current milestone

The product goal is a cross-platform APM Planner 3.0 (`3.0.0`) that transfers the functionality and recognizable workflow of Mission Planner 10 to Qt/CMake on Linux, Windows and macOS. Existing QGroundControl and Hermes/GTU code may be reused where licensing and architecture fit.

The current milestone is **broadly usable functionality**, not final pixel parity. Small spacing, color, label and geometry differences must be recorded and deferred instead of consuming the main implementation stream. Missing or inert actions are functional gaps and remain high priority.

The immediate user-directed order is:

1. Audit and fill the visibly sparse right-hand action column in PLAN. Compare the exact MP10 control/action count and order, prove which current controls are functional, and wire the high-value missing actions.
2. Then concentrate on SETUP: required pages, tabs, plugins and tools, reusing already implemented Qt services/widgets wherever possible.
3. Track functional and GUI inaccuracies separately in `PORTING_DEVIATIONS.tsv`.
4. Continue committing complete vertical slices rather than batches of disconnected UI stubs.

## Verified checkpoint

The latest functional checkpoint before this handoff is commit `de7fd269` (`feat: add Mission Planner command catalog editor`), following the broad parity checkpoint `b79e2f73`.

At that checkpoint:

- CMake configure and one `cmake --build build -j12` completed successfully.
- The complete test suite passed: **75/75 tests**.
- A real X11 main-window smoke test opened the app with title `APM Planner 3.0.0 (...) — APM Planner` and closed it with exit code 0.
- The worktree was clean after the commit.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 127 product rows: 25 `in-progress`, 49 `partial`, 53 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

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
- Cooperative shutdown ordering verified by the real-X11 smoke above.

## Known immediate PLAN problem

The PLAN right action column has visible unused vertical space. The current action panel contains working mission/polygon/transfer controls mixed with disabled placeholders. Known disabled or incomplete actions include Grid display, View KML, WMS, WMTS, custom map injection, MAVFTP and fast mission write. A complete comparison against MP10 `FlightPlannerView.axaml`, its code-behind and `FlightPlannerViewModel` is in progress.

Treat this as a functional inventory problem first:

- preserve the MP10 action names/order where applicable;
- connect capabilities that already exist but are not routed;
- implement high-value missing workflows rather than adding enabled-looking stubs;
- hide or clearly disable functionality that genuinely cannot work yet;
- use the lower space for real action groups, not cosmetic stretch removal alone.

After the audit, record every remaining difference in the deviations ledger and move the main implementation stream to SETUP as requested.

## SETUP next phase

The SETUP phase must compare the whole MP10 navigation model with the Qt backstage model, then classify every route as working, partial, stub or missing. Prioritize pages that can be made end-to-end functional on existing exact-target/parameter/command services and already implemented widgets.

Initial known opportunities include routing existing Map Tile Cache, Proximity and MAVLink Inspector functionality through an MP10-like `Advanced Tools` page, then expanding `Developer Tools` with guarded working operations. `Elevation Sources` and `Mission Command List` already have new Advanced routes. This is only a starting hypothesis; the active audit must establish the full ordering and gaps before implementation.

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

Only the coordinating agent may configure/build/test. Delegated research agents must not build. Use one build process, maximum `-j12`.

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
