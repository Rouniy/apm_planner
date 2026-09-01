# APM Planner 3.0 — Mission Planner 10 porting plan

This document is the implementation contract for the Qt/CMake reimplementation.
The screen inventory in `MISSION_PLANNER_SCREEN_PARITY.tsv` is the authoritative
feature list. Existing APM Planner widgets are reusable only after their target,
parameter typing, behavior, and visual layout have passed the same acceptance
criteria as the Mission Planner 10 reference.

## Conclusions fixed before implementation

1. This is a shell and application-architecture replacement, not a theme refresh.
   The old native menu, QML toolbar, and user-dependent dock layout cannot produce
   deterministic Mission Planner geometry.
2. Parameter and command traffic must be scoped by link, system and component.
   A page may become editable only after a complete, current-target snapshot is
   available. Switching targets invalidates page readiness.
3. Map rendering and tile persistence are separate concerns. Every map backend
   uses the same canonical tile store and provider identities; a backend must not
   create or purge a private cache.
4. Qt 5 is the product baseline. Hermes OPMap is the initial production map
   backend. The current QGroundControl FlightMap requires Qt 6 and private Qt
   Location APIs, so it cannot be a core or release dependency. QGC GeoMap is also
   still experimental. A second production backend must retain Qt 5 support;
   MapLibre Qt is a candidate to evaluate against that requirement.
5. Platform APIs are replaced by Qt facilities wherever Qt covers the required
   behavior. Small platform branches are permitted only behind a tested service.
6. Files are deleted only after their replacement passes its parity gate. An
   unbuilt or apparently obsolete module is not evidence that its user-visible
   function may be dropped.
7. Public Qt class names and stable `objectName` values follow the corresponding
   Mission Planner 10 views wherever the concepts match. The shell therefore uses
   `MainWindowHeader`, `FlightDataView`, `FlightPlannerView`, `BackstageView`,
   `SetupView` and `ConfigView`; implementation-specific suffixes are reserved for
   adapters such as map backends and platform services.
8. Legacy APM Planner plugin ABI/API and plugin-facing UI facades are explicitly
   out of scope. `SubMainWindow`, `VIEW_*`, legacy action names and Qt dock-state
   formats are transitional implementation details, not compatibility contracts.
   Migration of user missions, parameters, logs, settings and tile data remains a
   separate data-integrity requirement.

## Target architecture

```text
MainWindow
├── MainWindowHeader (DATA/PLAN/SETUP/CONFIG/SIMULATION/HELP, TOOLS, connection)
└── QStackedWidget
    ├── FlightDataView
    ├── FlightPlannerView
    ├── SetupView -> BackstageView
    ├── ConfigView -> BackstageView
    ├── SimulationView
    └── HelpView

FlightDataView / FlightPlannerView
└── KDDockWidgets host (optional floating mode)
    ├── deterministic Mission Planner layout is always the default
    └── per-view affinity + versioned JSON restore

VehicleSession(link, system, component, revision)
├── ParameterStore + metadata + load/transaction controllers
├── CommandTransactionService
├── Mission/Fence/Rally repositories
└── Telemetry and log services

IMapView
├── HermesOpMapBackend
├── QgcFlightMapBackend (optional, Qt 6)
└── future MapLibreBackend
    └── SharedTileStore (one root, provider IDs, quota and lifecycle)
```

## Canonical Mission Planner 10 identities

These names are layout keys and test contracts, not cosmetic labels:

| Surface | Stable Qt identities |
|---|---|
| Header | `MenuFlightData`, `MenuFlightPlanner`, `MenuInitConfig`, `MenuConfigTune`, `MenuSimulation`, `MenuHelp` |
| DATA | `FlightDataView`, `FlightDataLayoutGrid`, `HudHost`, `Hud`, `FdTabs`, `QuickHost`, `QuickGrid`, `MainFlightSplitter`, `MapVideoLayout`, `FdMap` |
| PLAN | `FlightPlannerView`, `PlannerLayoutGrid`, `Map`, `HorizontalDockSplitter`, `WaypointPanel`, `VerticalDockSplitter`, `ActionPanel`, `ActionScroller`, `ActionItemsPanel` |
| Backstage | `SetupView`, `ConfigView`, `BackstageView`, `BackstagePage` |
| Remaining roots | `SimulationView`, `HelpView` |

The reference navigation order is always DATA, PLAN, SETUP, CONFIG, SIMULATION,
HELP, then TOOLS, ARDUPILOT and the connection controls. Renaming an existing Qt
implementation is allowed because the legacy plugin API is not supported; new
names must match the corresponding Mission Planner 10 concept.

The shell reference is the clean-profile Mission Planner 10 `Emerald` palette:
header `#121614`, panel `#202623`, control `#1a201d`, input `#161b18`, deep
background `#0d1210` and accent `#34d399`. The window starts at 1280x800 with a
1120x720 minimum. The header is 64 logical pixels high, or 7 while auto-hidden
and not hovered. Navigation uses the matching Font Awesome Free glyphs; their
attribution is kept beside the SVG resources. These are screenshot-test tokens,
not approximate theme suggestions.

`HelpView` is a real center-stack perspective, separate from the modal About
dialog. Its stable and beta buttons share one guarded updater instance and must
always end in available, no-update, or failure state. Release selection is exact
by platform and channel and independent of manifest order. The legacy unsigned
download prompt remains transitional: production-equivalent self-update still
requires an APM Planner 3.0-owned HTTPS manifest, signing key, signed metadata,
and package hash/size verification before an installer may be executed.
The Help shortcut list intentionally exposes only commands that are currently
wired globally; Developer Tools, NMEA Output, and DataFlash Spectrogram remain
explicit parity work instead of advertising inert shortcuts.

## QGroundControl-derived communication safety

QGroundControl is the architectural continuation of APM Planner and is the first
reference for modern MAVLink, vehicle, parameter and mission behavior. Its Qt 6
UI dependencies are not copied into the Qt 5 baseline, but these transport rules
are mandatory:

- `MessageId` is `quint32` end to end; no array indexing or 8/16-bit narrowing.
- Each link owns a distinct MAVLink channel. Bytes enter one protocol thread by
  queued delivery before parsing and signal fan-out.
- Processing pins the link lifetime; stale queued bytes from a removed link are
  discarded.
- Vehicle removal is two-phase and timer/request callbacks use guarded pointers.
- Parameters are keyed by component and name. Mission transactions correlate
  mission type, expected ACK and outstanding item indexes.
- Reentrant command completion, vehicle deletion and multi-link parsing require
  dedicated regression tests.

## Qt-first platform policy

| Concern | Required implementation | Exception or gate |
|---|---|---|
| Audio files | Qt Multimedia | A release with audio enabled must fail configure if the module is absent |
| Speech | Qt TextToSpeech | Silent developer build must be an explicit option |
| Serial ports | Qt SerialPort and QSerialPortInfo | No `/dev/cu.*`, `ttyACM` or SetupAPI discovery in application code |
| USB firmware workflow | QSerialPortInfo VID/PID plus QObject/QTimer worker | No blocking polling on the GUI thread |
| Paths | QStandardPaths, QCoreApplication and GNUInstallDirs | Canonical MP tile-cache paths stay isolated and tested |
| Logging/files | QFile, QSaveFile and QTextStream | No narrow `std::ofstream` for user paths |
| Child processes | QProcess and QStandardPaths::findExecutable | User-selected executable path is stored in QSettings |
| Rendering | QWidget/QPainter or QOpenGLWidget | No QGLWidget/QGLFormat |
| Video/camera | Qt Multimedia | Backend-specific codecs remain deploy-time plugins |
| Game controller | `IJoystickBackend` | SDL2/SDL3 remains for arbitrary flight-stick axes; Qt Gamepad is optional |
| 6-DOF devices | Optional backend interface | Qt has no complete raw 6-DOF replacement |

## Delivery sequence and gates

1. **Baseline and inventory**
   - Freeze the 126-row parity manifest and reference commit.
   - Keep the Qt 5 build/tests green. An optional Qt 6 configure path is only a
     forward-compatibility diagnostic and is not a product-completeness gate.
   - Gate: manifest has an owner/status/evidence rule for every row.
2. **Core safety**
   - Add `VehicleTarget`, typed `ParameterStore`, latest-wins loading,
     immutable snapshots, staged writes and ACK-correlated commands.
   - Add one settings migration and stable product identity.
   - Gate: component collisions, duplicates, cancellation and target switches are
     covered by tests.
3. **Shared infrastructure**
   - Complete the canonical map tile store, quota/lifecycle, atomic writes and
     legacy-cache import.
   - Complete Qt paths/resources, audio/TTS, serial discovery and simulator lookup.
   - Gate: platform path tests and installed-tree smoke tests.
4. **Mission Planner shell**
   - Replace the old toolbar/menu/dock foundation with fixed header, stack,
     backstage and deterministic DATA/PLAN split layouts.
   - Introduce KDDockWidgets through the isolated design in `DOCKING.md`; do not
     carry the legacy `SubMainWindow` or `QMainWindow::saveState()` contract into
     the new views.
   - Gate: screenshot scenes S01–S14 at 1120×720 and 1280×800, geometry tolerance
     ±1 px and SSIM at least 0.98 after masking volatile content.
5. **SETUP and CONFIG**
   - Implement every page from the manifest through the new session services,
     beginning with full parameter editors and component-aware SERIAL/OSD pages.
   - Gate: no page marked complete without behavior, target-switch and visual
     evidence.
6. **DATA, PLAN and remaining tools**
   - Rebuild deterministic DATA and PLAN pages, then simulation, help and the full
     tools menu. Add map backend selection without changing the cache.
   - Gate: offline, connected and multi-vehicle fixtures match the reference.
7. **Qt 5 packaging**
   - Build the Qt 5 product on Linux, Windows and macOS through CMake presets and
     native deployment helpers. Removed/deprecated APIs may still be modernized,
     but the release baseline remains Qt 5.
   - Gate: CI build, package install, launch, QML/plugins/audio/SQL smoke and clean
     uninstall on all three platforms.
8. **Cleanup and final parity**
   - Remove qmake/Qt4 packages, native audio code, obsolete workers, old toolbar
     assets and vendored binaries only after their replacement gates are green.
   - Gate: no `not-started`, `in-progress`, `partial` or `blocked` row remains.

## Known high-risk items

- Old parameter code has component-name collisions, stale-target state and unsafe
  MAVLink ID handling; it must not be treated as the source of truth.
- The current joystick setup is not portable and may access an unopened SDL device.
- Firmware detection and simulator executable discovery contain Linux/macOS/Windows
  literals and blocking behavior.
- OPMap still contains Qt 4/5 OpenGL APIs; QGC's cache is incompatible with the
  canonical store and must never be allowed to delete it.
- A successful developer build without Multimedia, TextToSpeech or optional visual
  modules is a limited build, not product-completeness evidence.

## Commit policy

Commit after each coherent green gate (configure, build, tests and proportional
smoke check). Keep reference repositories read-only. Do not combine generated build
artifacts with source commits, and do not delete the previous implementation in the
same commit that first introduces an unproven replacement.
