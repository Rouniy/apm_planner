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
4. Hermes OPMap is the initial production map backend. The current QGroundControl
   FlightMap is a possible Qt 6-only adapter, not a core dependency: it is tightly
   coupled to QGC and Qt Location private APIs. QGC GeoMap is still experimental.
   MapLibre Qt is the preferred additional production candidate to evaluate.
5. Platform APIs are replaced by Qt facilities wherever Qt covers the required
   behavior. Small platform branches are permitted only behind a tested service.
6. Files are deleted only after their replacement passes its parity gate. An
   unbuilt or apparently obsolete module is not evidence that its user-visible
   function may be dropped.

## Target architecture

```text
MissionPlannerMainWindow
├── MpHeaderWidget (DATA/PLAN/SETUP/CONFIG/SIMULATION/HELP, TOOLS, connection)
└── QStackedWidget
    ├── DataPage
    ├── PlanPage
    ├── SetupBackstage
    ├── ConfigBackstage
    ├── SimulationPage
    └── HelpPage

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
   - Keep Qt 5 build/tests green while introducing a controlled Qt 5/Qt 6 bridge.
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
7. **Qt 6 and packaging**
   - Replace old QuaZip/QCustomPlot and removed Qt APIs; build Linux, Windows and
     macOS through CMake presets and native deployment helpers.
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
