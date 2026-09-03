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

1. Keep the committed PLAN, MAVLink Inspector and Antenna Tracker foundations green.
2. Continue the main `TOOLS` stream: replace the explicitly disabled MP10 entries with complete modeless windows, next with CoT/TAK Output and Device Operations now that MAVLink Mirror and NMEA Output are usable; no visible action may open an empty panel or silently do nothing.
3. After the principal Tools workflows are usable, focus the main stream on Settings/CONFIG as requested, including planner preferences and Onboard OSD.
4. Retain the deferred Tracker Home/parameter work and DATA HUD audit without allowing them to displace the current Tools → Settings order.
5. Track functional and GUI inaccuracies separately and keep committing complete slices rather than disconnected stubs.

## Verified checkpoint

The pre-Wave-1 functional checkpoint was `fb4f08b5` (`feat: port MinimOSD telemetry helper`), following `4290221f` (`feat: add antenna tracker output protocols`) and `e6fab89e` (`feat: port ESP8266 setup workflow`). The later verified checkpoints include `9d69cebb` (PLAN wrapper geometry), `9b180eba` (waypoint row actions), `e6d9e75b` (independent Inspector windows), `fbeacd6e` (Antenna Tracker pages) and `e7d5e58b` (their parity-ledger checkpoint).

At this checkpoint:

- CMake configure and `cmake --build build-codex-qt -j12` completed successfully; Claude's separately leased `build-claude` target also compiled and tested the NMEA sentence builder.
- The complete test suite passes with the Tools catalogue, Link Statistics, Tlog Convert / Extract, MAVLink Mirror and NMEA Output coverage: **105/105 tests**.
- Real X11 smoke verifies the exact 24-entry MP10 TOOLS order without late HIL/custom/panel actions, independent Link Statistics, Tlog Convert, MAVLink Mirror and NMEA Output windows from the main surface, modeless Inspector, Map Tile Cache, Plugin Manager and Log Download windows, Developer Tools navigation, and clean application exit with tool windows open. The Mirror smoke used a live UDP heartbeat source, received 64 framed bytes through TCP Host, observed Tx accounting and Listening after client disconnect, then opened a second independent window and exited with status 0. A separate write-back run sent the exact `WRITEBACK_MARKER_4096` bytes from the TCP peer through the pinned UDP vehicle link, showed Rx 21 with the checkbox enabled and exited cleanly. The NMEA smoke received 100 TCP Host sentences from a live local MAVLink source, verified the first GGA/GLL/HDG/VTG/RMC cycle and its five checksums, opened a second independent window and exited with status 0.
- The recovered PLAN row actions, explicit multi-instance MAVLink Inspector lifecycle, Antenna Tracker stack and first corrected Tools-menu/window slice are verified. Any later working-tree changes must be reviewed and checkpointed as their own coherent slice.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 128 product rows: 39 `in-progress`, 46 `partial`, 43 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

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
- An exact MP10 top-level TOOLS catalogue: real application tools are no longer mixed with view-dependent DATA/PLAN/SIM docks, unavailable workflows are visibly disabled with a reason, and late vehicle/custom-widget activity cannot append empty panels to the menu.
- A modeless MP10 Link Statistics window that follows the current exact target's physical link without retaining a stale link pointer, plus direct modeless entry points for Plugin Manager and MAVLink Log Download.
- A modeless MP10 Tlog Convert / Extract window with streaming, cancellable and atomic KML, GPX, CSV, text, parameter and complete-mission-snapshot exports; the unsupported Matlab button is disabled with an explicit reason.
- A modeless MP10 MAVLink Mirror window with independent per-window sessions, a complete-frame receive tap, immutable physical-link targeting, bounded Serial/TCP/UDP outputs and explicit live write-back control.
- A modeless MP10 NMEA Output window with independent per-window sessions, exact endpoint targeting, live 1/2/5/10 Hz selection and bounded Serial/TCP/UDP output of checksummed GGA/GLL/HDG/VTG/RMC cycles.
- Advanced and Developer action inventories, working shared actions/parsers, Advanced Terminal and the user-facing trusted QML plugin manager.
- Cooperative shutdown ordering, including close with the modeless inspector open, verified by the real-X11 smoke above.

## TOOLS current phase

The header menu now has the exact 24 MP10 items, order, separators and shortcuts. Ten routes are enabled because they have a truthful presentation: Developer Tools, Plugin Manager, MAVLink Inspector, MAVLink Mirror, NMEA Output, Map Tile Cache, Link Statistics, Connection Options, Download Logs (MAVLink) and Tlog Convert / Extract. The other 14 entries are retained in their reference positions but visibly disabled as `not ported yet`; they cannot flip a check mark, open an empty dock or silently no-op. Installed trusted QML tools may append one clearly named extension submenu after the reference inventory.

`LinkStatsWindow` is a fresh 300×250 modeless window per invocation. Every refresh resolves the current exact target to its physical link again, converts the rolling Qt bit rate to bytes per second and reads per-link MAVLink received/lost counters. Link removal or absence shows zero/em-dash values and an explicit status. `LogDownloadDialog` now closes through normal dialog lifecycle, clears an interrupted vehicle's state/connections and presents an explicit no-vehicle state.

`MavlinkLogWindow` is a fresh 460×340 modeless window per invocation. Its private-buffer reader streams timestamped MAVLink 1/2 records without sharing a protocol channel, resynchronizes corrupt input, and cancellation or failure leaves no partial single-file output. KML/GPX use valid `GLOBAL_POSITION_INT` tracks, CSV/text decode bundled-dialect fields, parameter extraction preserves ArduPilot versus bytewise wire semantics, and mission extraction requires complete per-endpoint transfers and deduplicates snapshots. The source tlog can never be selected as an output, and automatically named mission siblings cannot silently replace existing files. All exports use an explicit sensitive-data confirmation whose default and Escape action is Cancel. Matlab remains visibly unavailable until a verified cross-platform MAT-file writer exists.

`SerialPassThroughWindow` is a fresh 480×380 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Start snapshots the current exact target's physical link; successful inbound MAVLink frames are reconstructed byte-for-byte after framing and forwarded without touching the original traffic. Serial uses 8N1/no flow control, TCP Host retains one newest client, and UDP Host learns and fans out to a bounded peer set but refuses a collision with an active vehicle UDP listener. A 256 KiB total mirror-owned backpressure bound drops newest complete frames with a visible counter instead of blocking the protocol thread. Peer bytes are counted and drained with write-back disabled by default; enabling it writes raw bytes through an ephemeral link-ID lookup and automatically disables the reverse path if the link rejects them. Closing one window stops only its own service.

`SerialOutputNMEAWindow` is a fresh 480×400 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Connect pins the exact `(link, system, component)` target and stops explicitly if that physical link disappears. `GLOBAL_POSITION_INT` drives position/altitude, with `GPS_RAW_INT`, `VFR_HUD` and `ATTITUDE` fallbacks/overrides matching MP10 precedence. Each timer tick emits one complete GGA/GLL/HDG/VTG/RMC cycle with checksums and CRLF through bounded Serial 8N1, newest-client TCP Host or learned-peer UDP Host outputs on port 14551. Nothing is written before the first usable `GLOBAL_POSITION_INT` or any `GPS_RAW_INT`; matching MP10 state semantics, a no-fix GPS report can still produce zero coordinates with GGA quality 0. Transport failures remain visible. Closing one window stops only its own service, and guarded service pointers make destruction order safe in both NMEA and Mirror windows.

The next Tools slices are CoT/TAK Output and Device Operations with immutable exact targets, explicit Start/Stop/cancellation and destruction tests. Settings/CONFIG becomes the main stream immediately after those principal Tools workflows, as requested.

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

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with five working shared tools (MAVLink Inspector, Mavlink Mirror, NMEA, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Developer Tools exposes the exact 32-action inventory and working byte/MAVLink/hardware-ID parsers. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

The first-activation SETUP stall is fixed: `ApmCustomFirmwareConfig` yields before serial enumeration, downloads the manifest asynchronously through a dedicated reply, uses progress-based inactivity watchdogs, and keeps firmware download/upload state isolated. Flash actions retain an immutable selected-device snapshot and remain locked until a terminal upload outcome; destruction aborts replies and stops uploader timers. The still-large uncompressed manifest, GUI-thread JSON parsing, serial enumeration and complete MP10 firmware-selector parity remain explicit follow-up work.

`ESP8266 Setup` is now routed after Parachute and uses a dedicated exact-link client for `MAV_COMP_ID_UDP_BRIDGE` instead of changing the globally selected autopilot component. It waits for the 18 MP10 settings, displays the exact control set/defaults, serializes all 22 parameter writes and ACK-gates storage, reboot and reset. Unit/UI/transport coverage includes wrong-link replies, retry, partial response, target invalidation, reentrant cancellation and destruction; live bridge hardware, profile gating and cross-platform visual evidence remain.

The next audited SETUP packages are CubeID and Secure. CubeID is a target-aware firmware updater, not another HW-ID presentation; its CubePilot messages are absent from the current generated dialect and require a coordinated MAVLink update. `ConfigSecureView` manages bootloader public-key slots with `SECURE_COMMAND`, while `ConfigSecureApView` generates Ed25519 keys and signs bootloader/firmware files; MAVLink link signing remains a separate Advanced Tools workflow. The security pages need a reviewed cross-platform Ed25519 dependency before implementation.

The three Antenna Tracker routes are audited in `SETUP_ANTENNA_TRACKER_AUDIT.md`. The `Antenna Tracker (Serial)` and `Antenna Tracker (Live)` routes now exist after `ESP8266 Setup`: one shared `AntennaTrackerUIViewModel` (owned by `SetupView`, so the tracker loop survives page resets and navigation like MP10) drives `ConfigAntennaTrackerView` and `AntennaTrackerUIView` on top of `AntennaTrackerSerialService`, `AntennaTrackerGeometry` and a `UasAntennaTrackerTelemetrySource`; `antennatrackeruiviewmodel_tests` (11 cases) and `antennatrackerviews_tests` (4 cases) cover MP10 texts, validation order, settings, the loop, manual mode, live trim/reverse, failure recovery, the trim sweep and shutdown. `RadioStatusMonitor` now consumes both MAVLink radio-statistics messages per physical link and reproduces MP10's raw-unit SNR formula, 50 percent read-driven EMA and one-second hold; the telemetry source reads only the current exact target's link. The pages were smoke-tested on real X11 under Xvfb with a local SITL: both routes render, a real `QSerialPort` connect to a local UART reached `Connected (Maestro).`, Vehicle Az updated live and the application closed cleanly while connected. Deviations SETUP-018..022 record the shared instance, the Home / Center quirk, the intentional cancellable trim state machine, gating/tracker-home gaps and GUI approximations. The active sequence is the PLAN "Tracker Home" action, then the exact-target 25-field parameter page and remaining route/profile gates. The old `AntennaTrackerConfig` remains until these replacements are routed and verified.

Legacy APM Planner binary plugins are not a requirement. Where MP10 calls something a plugin/page/tool, reproduce the user-visible function with a native Qt page/service or the trusted QML extension system; do not restore the old ABI.

## Settings/CONFIG audit baseline

The active Settings path is `MainWindow -> ConfigView`; the compiled
`ApmSoftwareConfig` is not the production surface. MP10 registers 15 ordered
CONFIG routes, while Qt currently registers 13 concrete factories. `Heli Setup`
and `MAVFtp` are absent. None of the 13 factories literally returns a null
widget, but this does not make the surface equivalent: `Onboard OSD` currently
opens the unrelated legacy MinimOSD stream-rate helper, and a helicopter passes
Qt's broad multirotor gate and therefore sees the Copter `Basic Tuning` page
instead of `Heli Setup`. Plane `QP Extended Tuning` is also absent, and the Qt
GeoFence/profile gates do not match MP10's `DisplayView` gates.

The most direct source of a persistent blank Settings content area is
`ConfigView::resetVehiclePages()`: it can remove the selected page while
automatic fallback selection is disabled, then rely on a deferred target
callback which may return early or reject the now-hidden route. The first
Settings slice must restore the invariant that one visible route always owns a
real current widget and add a production `ConfigView` navigation/gating test
that sweeps offline, Copter, Plane, Rover, Heli, advanced-mode and target-reset
states. Until the true onboard editor exists, the wrong `Onboard OSD` route
must not remain actionable; Copter Basic Tuning must likewise be hidden for
Heli. The next functional package is the phase-one Onboard OSD editor documented
in `OSD_AUDIT.md`, followed by native Planner preferences and a shared
`DisplayView` profile service. The parity ledger retains the exact per-route
classification so missing pages cannot be mistaken for working settings.

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
