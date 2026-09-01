# Docking and floating panels

## Decision

APM Planner 3.0 will use KDAB KDDockWidgets for optional floating/tabbed panels
inside `FlightDataView` and `FlightPlannerView`. It will not use docking as the
layout engine for `MainWindow`, `BackstageView`, `SetupView` or `ConfigView`.

The Mission Planner 10 geometry remains a programmatically constructed default.
Restoring a user layout is allowed only after that default has been built, and a
Reset Layout action must always reconstruct it without reading stored state.

Legacy APM Planner plugin ABI, `SubMainWindow`, `QDockWidget` casts and serialized
`QMainWindow::saveState()` data are not compatibility requirements. User mission,
parameter, log, settings and map-cache data are unrelated and remain protected.

## Source and license

Hermes/GTU vendors a modified KDDockWidgets 1.4.0. The local copy is useful as an
integration reference but must not be copied directly because it:

- compiles sources into `hermes-gui` rather than exporting a standalone CMake
  target;
- changes the export macro and default restore option;
- removes Qt Quick sources and does not carry the complete upstream license set;
- uses private KDDockWidgets headers in its widget factory.

The vendored dependency must start from a pinned upstream release and include its
license notices. Project patches are allowed, but each patch needs a rationale,
an upstream reference where applicable, and build/restore tests; the source must
not silently diverge from its recorded tag. KDDockWidgets is compatible with this
GPLv3 application under its GPL option. Every binary package must include the
selected upstream notices.

KDDockWidgets 1.4.0 is the currently verified bootstrap version: it builds with
the project baseline Qt 5.15 in the current environment. Upstream 2.4.1 remains
the production candidate after Linux, Windows and macOS packaging provides the
matching Qt private development modules and passes the test matrix. This version
choice is a dependency gate. Hermes patches may be reapplied only when comparison
with the selected clean upstream shows that the behavior is still required.

## Integration contract

- Build a standalone CMake target and expose it through an internal `DockHost`
  adapter. Application widgets must not include KDDockWidgets private headers.
- Use stable Mission Planner-oriented dock IDs.
- Set one affinity per view (`apmplanner-<view-id>`) on every dock and on every
  layout saver. A layout from one screen must never affect the other.
- Store the validated JSON envelope in the view-specific QSettings key ending in
  `_DOCK_LAYOUT_V1`; the envelope also records schema, view ID, affinity and the
  KDDockWidgets payload version.
- Missing, corrupt, incompatible, added or removed docks must fall back to the
  complete default layout, never a partially restored layout.
- Floating panels are controlled by an explicit user setting. The first-run
  presentation is always the deterministic Mission Planner layout.
- KDDockWidgets state contains geometry only. Map tiles continue to use the one
  canonical shared tile store.

## Bootstrap status

`FlightDataView` and `FlightPlannerView` are now direct `DockableView` subclasses;
they no longer inherit `SubMainWindow`. Their first deterministic layouts use the
Mission Planner object names `FdMap`, `HudHost`, `FdTabs`, `Map`,
`WaypointPanel` and `ActionPanel`. Existing Qt map, PFD, telemetry and waypoint
widgets are hosted
as transitional functional content while those screens are ported route by route.

The classic QtWidgets primary flight display is the Qt 5 default. The legacy QML
display is available only with `APM_USE_QML_PFD=ON`, because it additionally
requires the QtQuick runtime modules to be present in every target package.

## Acceptance gate

Required automated coverage: default geometry at 1120×720 and 1280×800,
float/tab/close/reopen, per-view affinity isolation, corrupt JSON, dock additions
and removals, Reset Layout, HiDPI and multi-monitor restore. Linux, Windows and
macOS packages require a native launch/restore smoke test.
