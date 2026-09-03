# APM Planner 3.0 — Mission Planner 10 porting plan

The current execution order, complete prioritized backlog and per-wave gates are
maintained in `MASTER_PORTING_BACKLOG.md`. This file remains the architectural
contract; the master backlog is the operational plan.

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
   Legacy profile/data migration is also out of scope: APM Planner 3.0 starts
   from a fresh settings and data namespace.
9. New extensions use the trusted in-process QML contract in `QML_PLUGINS.md`.
   One shared engine exposes direct application-owned core objects; there is no
   sandbox or capability layer. API compatibility is tracked only by major
   version, and plugins are expected to be updated for a new major.

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
└── fixed QWidget/QSplitter panel tree (no floating)
    ├── deterministic MP10 geometry + stable object identities
    └── per-view schema-v2 JSON size/visibility restore

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

QmlPluginManager
├── one shared QQmlEngine
├── APMPlanner.Core 1.0 singleton -> direct core managers
└── trusted toolPage plugins -> QQuickWidget
```

## Canonical Mission Planner 10 identities

These names are layout keys and test contracts, not cosmetic labels:

| Surface | Stable Qt identities |
|---|---|
| Header | `MenuFlightData`, `MenuFlightPlanner`, `MenuInitConfig`, `MenuConfigTune`, `MenuSimulation`, `MenuHelp` |
| DATA | `FlightDataView`, `FlightDataLayoutGrid`, `HudHost`, `Hud`, `FdTabs`, `QuickHost`, `QuickGrid`, `MainFlightSplitter`, `MapVideoLayout`, `FdMap` |
| PLAN | `FlightPlannerView`, `PlannerLayoutGrid`, `Map`, `HorizontalDockSplitter`, `WaypointPanel`, `VerticalDockSplitter`, `ActionPanel`, `ActionScroller`, `ActionItemsPanel` |
| PLAN elevation graph | `elevationGraphToolStripMenuItem`, `ElevationGraphWindow`, `MissionElevationDisplay` |
| Backstage | `SetupView`, `ConfigView`, `BackstageView`, `BackstagePage` |
| Remaining roots | `SimulationView`, `HelpView` |

The reference navigation order is always DATA, PLAN, SETUP, CONFIG, SIMULATION,
HELP, then TOOLS, ARDUPILOT and the connection controls. Renaming an existing Qt
implementation is allowed because the legacy plugin API is not supported; new
names must match the corresponding Mission Planner 10 concept.

DATA uses `MainFlightSplitter` with a 6-pixel handle, a 2:3 left/map ratio and
240-pixel minimum column widths; its nested vertical splitter has a 4-pixel
handle and a 1:1 HUD/tabs ratio. PLAN uses 4-pixel splitter handles, a fixed
168-pixel action column and a fixed 210-pixel waypoint row. These are in-page
`QSplitter` layouts: DATA and PLAN have no `QDockWidget`, embedded `QMainWindow`
or floating-panel mode.

The shell reference is the clean-profile Mission Planner 10 `Emerald` palette:
header `#121614`, panel `#202623`, control `#1a201d`, input `#161b18`, deep
background `#0d1210` and accent `#34d399`. The window starts at 1280x800 with a
1120x720 minimum. The header is 64 logical pixels high, or 7 while auto-hidden
and not hovered. Navigation uses the matching Font Awesome Free glyphs; their
attribution is kept beside the SVG resources. These are screenshot-test tokens,
not approximate theme suggestions.
The authoritative Qt Widgets implementation remains at
`files/styles/style-outdoor.css` as the stable Emerald theme resource, and
`tests/test_missionplannerstyle.cpp` locks its tokens and representative dark
surfaces.

`HelpView` is a real center-stack perspective, separate from the modal About
dialog. Its stable and beta buttons share one guarded updater instance and must
always end in available, no-update, or failure state. Release selection is exact
by platform and channel and independent of manifest order. The legacy unsigned
download prompt remains transitional: production-equivalent self-update still
requires an APM Planner 3.0-owned HTTPS manifest, signing key, signed metadata,
and package hash/size verification before an installer may be executed.
The Help shortcut list still needs to add the newly wired Developer Tools,
Plugin Manager and Map Tile Cache shortcuts. NMEA Output and DataFlash
Spectrogram remain explicitly disabled parity work instead of inert shortcuts.

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
- Parameters are cached by exact `(link, sysid, compid)` endpoint and name.
  List refreshes are staged and replace the committed cache only after every
  ordinary index is present; sentinel index/count `65535` never fakes progress.
  Mission transactions correlate mission type, expected ACK and outstanding
  item indexes.
- The current DroneCAN parameter UI is deliberately read-only: GetSet requests
  enumerate by index until an empty response and display value, minimum, maximum
  and default metadata. Parameter writes remain a separate parity gate even
  though the transport codec can encode them.
- Reentrant command completion, vehicle deletion and multi-link parsing require
  dedicated regression tests.

The receive side now keeps MAVLink framing, version detection, warning state and
packet sequence accounting per physical link. This removes corruption when
partial frames from independent links arrive interleaved. `VehicleEndpoint` and
`VehicleTargetManager` preserve the exact `(link, sysid, compid)` identity,
including duplicate ids on independent links. They promote only the three
message kinds used by Mission Planner's visible MAV list, keep target-generation
leases for asynchronous consumers and atomically invalidate a selected target
when its link is removed. The registry is available to trusted QML plugins as an
explicit discovery/selection authority.

The MAVLink Mirror consumes a link-ID-only complete-frame signal after successful
per-link framing. It never observes arbitrary transport chunks and never retains
a `LinkInterface *`; reverse traffic is raw by design and re-resolves the pinned
physical link for every write. Secondary transports stay event-driven on the GUI
thread and apply one total bounded queue so a slow serial/TCP peer cannot block
or grow memory on the protocol path. Every modeless window owns and stops its own
session.

`ExactLinkTransmitter` now owns MAVLink version and sequence state once per
physical link. It rejects v2-only messages on MAVLink 1 and clears the checksum
bytes left in a generated encoder's trimmed payload before link-specific
re-finalization. `VehicleCommandService` uses that shared transmitter with an
immutable exact-target lease and exact source/link/command ACK correlation.
Native `UASActionsWidget` operations use it whenever the selected endpoint
belongs to the active UAS and fail closed when that lease becomes stale or
unavailable.

`ParameterService` uses the same transmitter for exact `PARAM_REQUEST_LIST`,
`PARAM_REQUEST_READ` and `PARAM_SET`. Responses are accepted only from the
leased `(link, sysid, compid)` and read/write acknowledgements additionally
match name, type and value. Its application-owned `ParameterStore` isolates
equal MAV ids on different links, preserves the last committed snapshot across
cancel/fail/partial refresh, and cancels late work when target generation
changes. Parameter writes now run through one application-owned FIFO: one initial
send plus at most three 700-ms retries, tokenized timeouts, activation-time
encoding/schema/no-op revalidation, immutable target identity, exact-only
acknowledgement, explicit failure/cancellation, no-op skipping and
prevalidated deterministic batches with `*ENABLE` last. List refresh forms a
queue boundary and never overlaps a write ACK. `QGCUASParamManager`, CONFIG,
SETUP, PLAN radius writes, the legacy waypoint dock, `AP2ConfigWidget` and the
single transitional `QGCParamWidget` consume this service; reachable
SETUP/CONFIG/PLAN code no longer sends parameters through `UAS::setParameter`,
and the widget no longer owns a competing write-retry timer. Exact-target
changes invalidate parameter-owned pages before the new committed snapshot is
replayed, preventing staged values or pending UI sequences from crossing
vehicles. Target selection now has explicit generation-changed and settled
phases: cancellation handlers may queue work for the new exact endpoint, but
that work cannot emit lifecycle/value signals until every facade and UI
consumer has invalidated the previous generation. Same-turn failed list
requests are coalesced only for the same endpoint. The service and
current endpoint cache are available to trusted QML as
`APMPlanner.parameterService`, while `APMPlanner.vehicleCommandService` exposes
exact `COMMAND_LONG`.

`ConfigBatteryMonitoring2View` is now a transport-free Qt page rather than an
`AP2ConfigWidget` consumer. `SetupView` supplies its authoritative BATT2
snapshot, exact-link BATTERY2/BATTERY_STATUS telemetry and one-item serialized
write batches, including non-primary autopilot components. Metadata drives the
Monitor/pin choices and Capacity range, checked value conversion prevents
integer overflow, and the page remains locked until its own batch completes.
The MP10 low-battery setting and defaults feed the application-lifetime
`SpeechAnnouncer`, with the original voltage/percent predicate, phrase tokens,
armed-only gate and 30-second cadence; the legacy QGC emergency loop yields
while this alert is enabled. Facade invalidation is keyed only to
`targetGenerationChanged`, so a link/component display-name refresh cannot
discard edits or break an active parameter-list transaction.
Full Parameter List modal editors close safely if their owning target page is
destroyed or its live schema changes. An explicit user-approved out-of-range
value retains that approval through final write-time validation only while the
same metadata catalog remains authoritative. Staged-set revisions also prevent
a modal confirmation from submitting values changed after the user reviewed
the write, and switching component clears all staged values and approvals.

`MainWindowHeader::cmb_sysid` now enumerates exact link/system/component tuples,
excludes component 190, synchronizes the selected link and legacy active UAS,
and requests an incomplete parameter snapshot unless Ctrl suppresses it. Some
legacy mission/command surfaces can still fan out through `UAS`, so overall
connection parity remains partial.

Application shutdown follows the same ownership principle: QML and map clients
are destroyed first, communication ingress is detached, DroneCAN is quiesced
while its exact link is still writable, every link/replay thread is joined
cooperatively, and only then are UAS, decoder and protocol objects destroyed.
The sequence is idempotent and rejects late queued MAVLink work. A real X11 run
with an incoming ArduPilot heartbeat and Alt+F4 exits cleanly with status 0.

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
   - Add `VehicleTarget`, an exact-endpoint typed `ParameterStore`, staged list
     refresh, immutable committed snapshots, exact parameter transactions and
     ACK-correlated commands on one shared per-link transmitter.
   - Establish a version-neutral product identity and fresh settings namespace.
   - Gate: component collisions, duplicates, cancellation and target switches are
     covered by tests.
3. **Shared infrastructure**
   - Complete the canonical map tile store, quota/lifecycle and atomic writes in
     the fresh APM Planner 3.0 data namespace.
   - Complete Qt paths/resources, audio/TTS, serial discovery and simulator lookup.
   - Gate: platform path tests and installed-tree smoke tests.
4. **Mission Planner shell**
   - Replace the old toolbar/menu/dock foundation with fixed header, stack,
     backstage and deterministic DATA/PLAN split layouts.
   - Use the fixed `QWidget`/`QSplitter` design in `DOCKING.md`; do not carry
     legacy `SubMainWindow` page ownership, `QDockWidget` or floating state into
     DATA/PLAN. Persist only the validated per-view schema-v2 layout tree.
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
   - PLAN terrain lookup uses configured local GDAL GeoTIFF/DTED data first and
     the shared `srtm` HGT cache second. Missing tiles are downloaded in the
     background from the official ArduPilot SRTM1/SRTM3 roots, validated and
     atomically installed; mission editing always keeps internal altitudes in SI.
   - PLAN altitude presentation uses the Mission Planner key `altunits` and
     values `Meters|Feet`. `WpRowData`, terrain, WPL/QGC Plan, MAVLink and
     persisted `FlightPlanner/TXT_*` values remain canonical metres; only the
     ViewModel/model/widget boundary converts by `1` or
     `3.280839895013123`. The CONFIG Planner selector applies live and is also
     exposed through typed properties on the trusted QML main-window API.
     Survey Grid altitude and
     takeoff editors plus backend-neutral PLAN marker tooltips use that same
     display-only policy without changing their exact SI values.
   - PLAN distance presentation independently uses the Mission Planner key
     `distunits` and values `Meters|Feet`. Route geometry, navigation radii,
     mission files, MAVLink and persisted planner values stay in metres; the
     table, radius editors and Dist/Home/Prev summaries switch live between
     `m|km` and `ft|miles`. The original WinForms GridUI incorrectly used
     `distunits` for survey altitude; the Qt implementation intentionally fixes
     that coupling. Survey spacing/trigger/geometry and polygon offset/area
     remain explicitly SI, matching the current Mission Planner port rather
     than reintroducing that cross-domain bug. The current-mission Elevation
     Graph is implemented as a separate PLAN parity gate: Mission, HOME and DEM
     values remain canonical SI and `altunits`/`distunits` are applied only when
     rendering the graph. Dedicated automated window/action and reference
     screenshot-diff evidence, plus the independent speed-unit policy, remain
     outstanding.
   - PLAN `Map Tool -> Measure Distance` keeps the original `ContextMeasure`
     identity and two-click interaction. Its state and Mercator-axis
     haversine/initial-bearing calculation are backend-neutral; concrete map
     adapters only project the two red markers and temporary green line. The
     result follows live `distunits` without mutating mission data. Context
     deletion is likewise marker-hit based: `Delete WP` is disabled over empty
     map space and deletes the exact clicked sequence rather than an unbounded
     nearest waypoint.
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
