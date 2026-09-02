# Fixed DATA and PLAN panels

## Decision

APM Planner 3.0 uses a deterministic Qt Widgets panel layout for
`FlightDataView` and `FlightPlannerView`. `DockableView` composes ordinary
`QWidget` content with `QSplitter` and, when a caller requests tabbed content,
`QTabWidget`. DATA and PLAN contain no `QDockWidget`, embedded `QMainWindow`,
KDDockWidgets dependency or floating-panel mode.

The filename is retained because this document replaces the former docking
design. The `DockableView` API also retains its established panel/docking terms
so feature code does not need another interface rename, but `isPanelDocked()`
now means that an open panel is part of the fixed in-page layout. Panels cannot
be detached, floated or migrated between DATA and PLAN.

Legacy APM Planner plugin ABI and `SubMainWindow` layouts are not compatibility
requirements. Standard `QDockWidget` may remain temporarily on pages that have
not yet been ported, but it is not the DATA/PLAN architecture. User missions,
parameters, logs, settings and the shared map cache are independent of panel
geometry and remain protected.

## Integration contract

- Feature code supplies stable Mission Planner-oriented panel IDs to
  `DockableView`; it does not depend on splitter ownership or layout chrome.
- Generated containers are limited to `QSplitter` and `QTabWidget`. Splitters
  do not collapse children and use opaque resize. Tab pages are not
  user-movable.
- Required panels, currently the DATA and PLAN maps, cannot be hidden. No action
  may hide the last visible panel.
- DATA uses `HudHost`, `FdTabs` and `FdMap`. Its outer horizontal splitter is
  `MainFlightSplitter`, has a 6 logical-pixel handle, a 2:3 left/map ratio and
  240-pixel minimum widths. The left vertical splitter has a 4-pixel handle and
  equal-height HUD and tabs.
- PLAN uses `Map`, `WaypointPanel` and `ActionPanel`. Its horizontal and vertical
  splitters use 4-pixel handles. The action column is 168 logical pixels wide;
  the waypoint row is 210 logical pixels high; the reference map extents are
  932 by 430 at the 1100 by 640 design size.
- DATA/PLAN panel visibility and splitter sizes are stored in the existing
  view-specific QSettings key ending in `_DOCK_LAYOUT_V1`. The suffix is a
  historical storage-key name and does not identify the payload version.
- The canonical payload is `apmplanner-fixed-panel-layout` version 2. Its JSON
  envelope contains `schema`, `version`, `viewId`, the exact ordered `panels`,
  `panelStates`, and a recursive `layout` tree. Split nodes record orientation
  and positive sizes; tab nodes record the selected page.
- Restore validation is transactional. A corrupt envelope, foreign view,
  changed panel set, mismatched topology, hidden required panel or all-hidden
  state is rejected before visibility changes. Rejection restores every panel,
  and `MainWindow` removes the incompatible stored value.
- Version 1 Qt `QMainWindow::saveState()` envelopes, the older Qt fallback
  schema and KDDockWidgets payloads are intentionally obsolete and are not
  migrated. Their floating geometry has no valid representation in the fixed
  layout.
- Panel state contains layout and visibility only. Every map backend continues
  to use the one canonical shared tile cache.

## Mission Planner 10 presentation

The default desktop stylesheet is the Qt Widgets counterpart of Mission
Planner 10's `Emerald` palette. The historical resource name
`files/styles/style-outdoor.css` remains for settings compatibility, but it now
uses these authoritative tokens:

| Role | Color |
|---|---|
| Accent / hover accent | `#34D399` / `#10B981` |
| Control / application background | `#1A201D` / `#121614` |
| Panel / input background | `#202623` / `#161B18` |
| Border / deep background | `#2A322D` / `#0D1210` |
| Text / button text | `#E6EDE9` / `#06251A` |

The stylesheet covers splitter handles, tabs, tables, inputs, menus and the
stable DATA/PLAN object names. It is part of the geometry and screenshot
contract, not an optional light/dark preference.

The classic Qt Widgets primary flight display remains the Qt 5 default. The
legacy QML display is optional through `APM_USE_QML_PFD=ON` and is unrelated to
the fixed-panel layout.

## Acceptance gate

Automated coverage must prove that DATA and PLAN contain no `QDockWidget` or
embedded `QMainWindow`, retain their exact splitter identities, handle widths,
ratios and fixed PLAN extents, and render every default panel after stacked-page
switches. It must round-trip schema v2 and reject obsolete, foreign, corrupt,
topology-mismatched, hidden-map and all-hidden payloads. Style coverage must
verify the authoritative Emerald tokens and representative dark surfaces.

Linux, Windows and macOS packages still require native launch, resize, restore,
HiDPI and screenshot smoke tests. Floating-window tests are not applicable to
DATA or PLAN.
