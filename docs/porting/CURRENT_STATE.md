# APM Planner 3.0 current state and handoff

Updated: 2026-09-04. This is the short operational handoff for the Mission Planner 10 Qt/CMake port. Update it whenever a functional package is committed or the immediate priority changes.

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
2. Continue the requested Settings/CONFIG stream from the native Flight Modes page; Plane QP Extended Tuning is the remaining route-level mismatch and the incomplete controls inside the nine-section Planner page are the broadest Settings gap.
3. Keep the remaining specialized `TOOLS` entries in their exact MP10 positions but disabled until each has a complete modeless workflow; no visible action may open an empty panel or silently do nothing.
4. Retain the useful SETUP modules and the deferred Joystick/PX4Flow/FFT/REPL, Tracker Home/parameter and DATA HUD work without allowing them to displace the current Settings priority.
5. Track functional and GUI inaccuracies separately and keep committing complete slices rather than disconnected stubs.

## Verified checkpoint

The pre-Wave-1 functional checkpoint was `fb4f08b5` (`feat: port MinimOSD telemetry helper`), following `4290221f` (`feat: add antenna tracker output protocols`) and `e6fab89e` (`feat: port ESP8266 setup workflow`). The later verified checkpoints include `9d69cebb` (PLAN wrapper geometry), `9b180eba` (waypoint row actions), `e6d9e75b` (independent Inspector windows), `fbeacd6e` (Antenna Tracker pages) and `e7d5e58b` (their parity-ledger checkpoint).

At this checkpoint:

- CMake configure and `cmake --build build-codex-qt -j12` completed successfully; Claude's separately leased `build-claude` target also compiled and tested the NMEA sentence builder.
- The complete test suite passes with the Tools catalogue, Link Statistics, Tlog Convert / Extract, MAVLink Mirror, NMEA Output, CoT/TAK, Device Operations, CONFIG Onboard OSD, DisplayView profiles, Planner startup UDP, the native Planner Settings page, shared CONFIG/SETUP MAVFTP, both Heli Setup schemas, both current/legacy Frame Type pages, the native current Compass parameter/onboard/fixed-yaw workflow, the dedicated Compass/Motor route and shared native Flight Modes: **138/138 tests**.
- Real X11 smoke verifies the exact 24-entry MP10 TOOLS order without late HIL/custom/panel actions, independent Link Statistics, Tlog Convert, MAVLink Mirror, NMEA Output, CoT/TAK and Device Operations windows from the main surface, modeless Inspector, Map Tile Cache, Plugin Manager and Log Download windows, Developer Tools navigation, and clean application exit with tool windows open. The Mirror smoke used a live UDP heartbeat source, received 64 framed bytes through TCP Host, observed Tx accounting and Listening after client disconnect, then opened a second independent window and exited with status 0. A separate write-back run sent the exact `WRITEBACK_MARKER_4096` bytes from the TCP peer through the pinned UDP vehicle link, showed Rx 21 with the checkbox enabled and exited cleanly. The NMEA smoke received 100 TCP Host sentences from a live local MAVLink source, verified the first GGA/GLL/HDG/VTG/RMC cycle and its five checksums, opened a second independent window and exited with status 0. The CoT smoke opened the enabled menu route on a live UDP vehicle, selected TCP Host, delivered the same parseable CoT 2.0 event to two simultaneous clients, opened a second independent window and exited cleanly with the first session active. The Device Operations smoke opened the 760×520 window through TOOLS, opened a second independent instance through Ctrl+J, then opened a third through SETUP → Developer Tools and exited with status 0.
- A new real-X11 CONFIG smoke used a local UDP target with two OSD screens and nine committed parameters, clicked the production `Onboard OSD` navigation row, rendered the non-empty canvas plus ALT/BAT item editors and closed the application cleanly. Evidence is `/tmp/apm-osd-smoke.kLNaLd/osd.png` for this workspace run; reference screenshot diff and physical-vehicle writes remain.
- A real-X11 Flight Modes smoke used one isolated exact Copter endpoint with a complete nine-parameter snapshot and live RC5=1500. It selected both production rows independently through SETUP and CONFIG/Settings and verified Current Mode=Auto, numeric Current PWM, all six rows, Save/Refresh/help controls and the green active-row paint before a clean exit. Evidence is `/tmp/apm-flightmodes-smoke.qQEFJI/setup-flight-modes.png` and `/tmp/apm-flightmodes-smoke.qQEFJI/config-flight-modes.png`; physical Copter/Plane/Rover/PX4 writes, reference screenshot diff and native-platform evidence remain.
- A second real-X11 CONFIG smoke clicked the production `Planner` row, rendered the native scrollable Planner Settings page instead of the old generic widget, showed the Display, Speech, Flight Command Shortcuts, Waypoints / Connect, Startup UDP and Telemetry sections, scrolled successfully and exited cleanly. Evidence is `/tmp/apm-planner-settings-smoke.pSKCFA/planner-lower.png`; all nine section/order checks are also deterministic in `configplannerview_tests`.
- A real-X11 route smoke with a connected local UDP target clicked the production `MAVFtp` row in both CONFIG and SETUP and rendered the same non-empty native tree/table/toolbar page from each shell. Evidence is `/tmp/apm-mavftp-smoke.al49Kd/config-mavftp.png` and `/tmp/apm-mavftp-smoke.al49Kd/setup-mavftp.png`; live MAVFTP-server transactions remain separate hardware/SITL evidence.
- A real-X11 CONFIG smoke used an isolated MAVLink-v1 helicopter target on UDP 15550 with the exact legacy `H_SWASH_TYPE` capability, clicked the production `Heli Setup` row and rendered the non-empty swash/manual-control, curve and live-servo page. Evidence is `/tmp/apm-heli-smoke.vPSoO8/heli-setup.png`; the application exited cleanly, while physical legacy-heli writes remain separate evidence.
- A separate real-X11 SETUP smoke used an isolated current helicopter target with `H_SW_TYPE`, clicked the production `Heli Setup (4.0+)` row and rendered hydrated servo/swashplate controls on the non-empty five-section page. Evidence is `/tmp/apm-heli4-smoke.HOEZdm/heli4-setup.png`; the application exited with status 0, while physical current-heli writes remain separate evidence.
- A real-X11 SETUP smoke used an isolated Copter target with `FRAME_CLASS`, `FRAME_TYPE` and `FRAME`, opened both production frame routes and rendered the hydrated `OCTAQUAD / H` preview plus all six legacy choices instead of the old blank combined widget. Evidence is `/tmp/apm-frame-smoke.qhCYvE/frame-current.png` and `/tmp/apm-frame-smoke.qhCYvE/frame-legacy.png`; the application exited with status 0, while physical frame writes remain separate evidence.
- A real-X11 SETUP smoke used an isolated Copter target with legacy compass parameters, clicked the production `Compass (Legacy)` row and rendered its non-empty basic setup surface. Live, Onboard and CompassMot calibration buttons were visibly disabled; clicking Onboard created no top-level window, and the application exited with status 0. Evidence is `/tmp/apm-compass-legacy-smoke.A50E1u/compass-legacy.png`.
- A separate real-X11 SETUP smoke used an isolated Copter target with current compass priority/device parameters and a scripted MAVLink peer. The production `Compass` row rendered its non-empty priority table, use/learn controls and calibration surface; Start emitted exact `MAV_CMD_DO_START_MAG_CAL`, the peer returned a correlated ACK plus `MAG_CAL_PROGRESS`, and the UI entered `Calibration running` with Mag 1 at 37%. A separate lost-ACK run entered the explicit fail-closed `OutcomeUncertain` state. Evidence is `/tmp/apm-compass-smoke-current4/compass-ready2.png` and `/tmp/apm-compass-smoke-current4/compass-running.png`; physical compass motion, reference screenshot diff and native-platform evidence remain.
- A real-X11 SETUP smoke used a dedicated TCP Copter peer, clicked the production `Compass/Motor Calib` row and rendered the non-empty ready, default-No confirmation, live 37.5%/12.50 A/27% XYZ plot and proven-success states. The peer observed the exact param6=1 start followed by two Finish ACK frames. A separate run killed the same physical TCP session while active and rendered the critical unconfirmed-stop warning before clean application exit. Evidence is `/tmp/apm-compassmot-smoke.OMIs2D` and `/tmp/apm-compassmot-smoke.FdD2Hd`; propeller-free physical validation and stored-parameter reconciliation remain.
- The DATA HUD AOA/SSA overlay no longer leaks its opaque black brush into the following speed/altitude tape outlines. Those side tapes retain the same translucent fill with AOA off or on; `hudcontrol_tests` compares the complete left tape pixel region between both render paths.
- The recovered PLAN row actions, explicit multi-instance MAVLink Inspector lifecycle, Antenna Tracker stack and first corrected Tools-menu/window slice are verified. Any later working-tree changes must be reviewed and checkpointed as their own coherent slice.

Always re-check the current Git state and test count; these numbers describe the checkpoint, not a permanent guarantee.

The parity inventory currently has 127 product rows: 50 `in-progress`, 42 `partial`, 35 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. The duplicate legacy-Heli inventory row has been removed. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

## Implemented foundations worth reusing

These are working foundations, though their parity rows may remain partial because visual, hardware or cross-platform evidence is outstanding:

- Mission Planner-like main shell and deterministic DATA/PLAN `QSplitter` layouts without KDDockWidgets.
- User-selectable compiled map backends through a common abstraction and one canonical filesystem tile cache.
- DATA HUD/map/telemetry surface and PLAN mission table/map/action surface.
- Typed Mission, Fence and Rally stores; mission file/transfer paths; QGC Plan and legacy fence/rally file handling.
- PLAN mission editing, context commands, store-aware undo, route geometry, polygon drawing/offset/fence conversion, Survey/Corridor import, terrain source, elevation graph and distance measurement.
- Exact `(link, system, component)` vehicle target registry, shared link transmitter, command service, parameter service and committed parameter store.
- Backstage SETUP/CONFIG infrastructure, the phase-one Onboard OSD layout editor and a substantial set of hardware/parameter pages listed in the parity ledger.
- A native Planner Settings page with the exact nine-section MP10 topology, one shared application model across both entry points, live unit/profile/runtime controls, dual startup UDP policy and an explicitly bounded Legacy settings dialog.
- A native legacy CONFIG Heli Setup page with the exact 43 logical parameter rows, swash/manual controls, MP10 curve math and live RC/servo visualization; dangerous manual writes are disarmed, confirmation-gated, exact-target and exact-batch correlated. A conservative uncertainty latch survives lost ACKs/raw echoes until an exact successful `H_SV_MAN=0` batch, with best-effort deactivation cleanup.
- A separate native SETUP `Heli Setup (4.0+)` page with all five MP10 sections and 83 exact bindings: eight fixed servo rows, current swashplate, rotor-speed, governor and miscellaneous parameters. Missing firmware fields stay visible and disabled, metadata drives current enums/ranges with safe current-heli enum fallbacks, non-zero `H_SV_MAN` is disarmed and confirmation-gated, and writes complete only through their immutable exact-target batch. A conservative manual-override latch survives raw echoes/lost ACKs, deactivation requests an exact zero, and only the successful zero batch clears the latch.
- Separate native SETUP `Frame Type` and `Frame Type (Legacy)` pages in MP10 order. The current page carries all 14 frame classes, the exact dependent subtype table, preview and Refresh; the retained legacy page carries all six `FRAME` choices including V-Tail. Both render a complete unavailable state before any parameter/version callback, hydrate without writes, block frame changes while armed and use immutable exact-target result-aware batches. A partial two-parameter result clears the optimistic geometry, disables edits and requires a complete refreshed snapshot before edits resume.
- The native current SETUP `Compass` page mirrors MP10's 11-column priority/device table, missing-device discovery, exact aliases and physical-slot mapping, Use 1/2/3, Learn, nine advanced fields, declination and Pixhawk defaults. It always constructs a non-empty disabled surface before a complete snapshot, writes only while disarmed through one immutable exact-target batch, treats the three priority parameters as one zero-filled transaction, requires full refresh after any partial terminal outcome and clears reboot-required only on the exact accepted command ACK. Its application-owned calibration service pins link/system/component/generation, ACK-gates Start/Accept/Cancel/fixed-yaw, waits for the complete multi-compass mask, never accepts failed sensors, retains reboot requirements per exact endpoint, releases stale UI ownership on a fresh target generation and treats long/stalled onboard timers as warnings rather than cancelling vehicle state. The same service now owns the separate `Compass/Motor Calib` route: it serializes all compass operations, sends the exact param6 start and two-frame Finish ACK, exposes bounded exact-session status/plot/log data, and issues fail-closed motor stops on deactivation, watchdog, target change, physical disconnect and shutdown. Start requires a disarmed ArduCopter multirotor, a complete snapshot, one autopilot on an operator-confirmed dedicated ordered Serial/TCP link and a default-No propeller warning; listening UDP, UDP client, simulation and unknown transports are rejected. MAVLink 1 point-to-point links remain supported with zero ACK target fields and terminal firmware text, while MAVLink 2 target extensions are used only as phase evidence, never as shared-link isolation. An uncertain outcome poisons ordinary compass work on that endpoint until the operator proves power removal; a proven terminal outcome permits onboard/fixed-yaw work but prevents another CompassMot run in the same physical-link epoch. `Calibrate from Log` remains visibly disabled because the separate OfflineMagFit workflow is not ported. The inherited single-compass page remains separately exposed as `Compass (Legacy)` with all unsafe global calibration actions quarantined.
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
- A modeless MP10 CoT/TAK Output window with all six reference transport modes, preserved six-cell identity JSON, deterministic CoT 2.0 serialization and one event per positioned MAVLink endpoint on the pinned physical link.
- Advanced and Developer action inventories, working shared actions/parsers, Advanced Terminal and the user-facing trusted QML plugin manager.
- Cooperative shutdown ordering, including close with the modeless inspector open, verified by the real-X11 smoke above.

## TOOLS current phase

The header menu now has the exact 24 MP10 items, order, separators and shortcuts. Twelve routes are enabled because they have a truthful presentation: Developer Tools, Plugin Manager, MAVLink Inspector, MAVLink Mirror, NMEA Output, Cursor-on-Target / TAK Output, Map Tile Cache, MAVLink Device Operations, Link Statistics, Connection Options, Download Logs (MAVLink) and Tlog Convert / Extract. The other 12 entries are retained in their reference positions but visibly disabled as `not ported yet`; they cannot flip a check mark, open an empty dock or silently no-op. Installed trusted QML tools may append one clearly named extension submenu after the reference inventory.

`LinkStatsWindow` is a fresh 300×250 modeless window per invocation. Every refresh resolves the current exact target to its physical link again, converts the rolling Qt bit rate to bytes per second and reads per-link MAVLink received/lost counters. Link removal or absence shows zero/em-dash values and an explicit status. `LogDownloadDialog` now closes through normal dialog lifecycle, clears an interrupted vehicle's state/connections and presents an explicit no-vehicle state.

`MavlinkLogWindow` is a fresh 460×340 modeless window per invocation. Its private-buffer reader streams timestamped MAVLink 1/2 records without sharing a protocol channel, resynchronizes corrupt input, and cancellation or failure leaves no partial single-file output. KML/GPX use valid `GLOBAL_POSITION_INT` tracks, CSV/text decode bundled-dialect fields, parameter extraction preserves ArduPilot versus bytewise wire semantics, and mission extraction requires complete per-endpoint transfers and deduplicates snapshots. The source tlog can never be selected as an output, and automatically named mission siblings cannot silently replace existing files. All exports use an explicit sensitive-data confirmation whose default and Escape action is Cancel. Matlab remains visibly unavailable until a verified cross-platform MAT-file writer exists.

`SerialPassThroughWindow` is a fresh 480×380 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Start snapshots the current exact target's physical link; successful inbound MAVLink frames are reconstructed byte-for-byte after framing and forwarded without touching the original traffic. Serial uses 8N1/no flow control, TCP Host retains one newest client, and UDP Host learns and fans out to a bounded peer set but refuses a collision with an active vehicle UDP listener. A 256 KiB total mirror-owned backpressure bound drops newest complete frames with a visible counter instead of blocking the protocol thread. Peer bytes are counted and drained with write-back disabled by default; enabling it writes raw bytes through an ephemeral link-ID lookup and automatically disables the reverse path if the link rejects them. Closing one window stops only its own service.

`SerialOutputNMEAWindow` is a fresh 480×400 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Connect pins the exact `(link, system, component)` target and stops explicitly if that physical link disappears. `GLOBAL_POSITION_INT` drives position/altitude, with `GPS_RAW_INT`, `VFR_HUD` and `ATTITUDE` fallbacks/overrides matching MP10 precedence. Each timer tick emits one complete GGA/GLL/HDG/VTG/RMC cycle with checksums and CRLF through bounded Serial 8N1, newest-client TCP Host or learned-peer UDP Host outputs on port 14551. Nothing is written before the first usable `GLOBAL_POSITION_INT` or any `GPS_RAW_INT`; matching MP10 state semantics, a no-fix GPS report can still produce zero coordinates with GGA quality 0. Transport failures remain visible. Closing one window stops only its own service, and guarded service pointers make destruction order safe in both NMEA and Mirror windows.

`SerialOutputCotWindow` is a fresh 720×820 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. It reproduces the reference endpoint order/defaults, 0.1–3600 second interval, event/UID/callsign controls, six-column advanced identity grid, Connect/Stop, indentation and last-event preview. Connect pins the current physical link but emits for every exact system/component endpoint discovered on that link, matching MP10's one-comPort scope without collapsing equal IDs on other links. TAK multicast, UDP Client/Host, TCP Client/Host and Serial 8N1 are separate bounded transports; TCP Host broadcasts to up to 16 clients and UDP Host targets the newest sender. XML attribute/element order, precision, escaping, UTC start/stale and platform-native indentation match MP10, while invalid XML/non-finite state is rejected visibly. Closing one window stops only its session.

MAVLink Device Operations is now a fresh independent modeless window from TOOLS, Ctrl+J and SETUP → Developer Tools. Its event-driven service sends DEVICE_OP_READ/WRITE only on one generation-checked physical-link lease, rejects MAVLink 1, correlates replies by exact link/source/type/request ID, bounds all protocol fields and cancels safely on target change, link loss or destruction. The destructive ICM20948 write/read test requires a current disarmed heartbeat and refuses an edited destination outside the bound endpoint; ordinary register reads retain MP10's editable destination on the pinned link. Settings/CONFIG is now the main stream, as requested; the remaining specialized Tools stay visible but disabled until complete.

The shared MAVFTP remote-file browser is now routed from both CONFIG and SETUP. It provides the MP10 Refresh/Download/Upload/Delete/Mkdir workflow plus explicit cancellation, lazy `/` and `@SYS` browsing, progress, atomic `QSaveFile` downloads with collision-avoiding names, and a default-Cancel warning before a potentially replacing upload. Its application-owned service pins one exact physical-link target per operation, strictly correlates the MAVFTP response envelope, paginates directories, retries with per-operation bounds, closes and flushes upload sessions before a 30-second-budget CRC verification, and cancels safely on target/link changes. The current safety bound buffers files up to 64 MiB; exclusive no-replace local creation, burst/streaming transfer and live hardware evidence remain.

## Settings/CONFIG current phase

`CONFIG_SETTINGS_INVENTORY_AUDIT.md` is the count baseline. MP10 and Qt now
both have 15 ordered concrete CONFIG factories; Plane QP Extended Tuning is
the remaining route-level functional mismatch. Flight Modes now uses one
native exact-target page shared with SETUP. The useful legacy tuning and
GeoFence pages remain visibly classified instead of being deleted to improve
the raw count.

The shared native Flight Modes page reproduces the six MP10 rows and exact PWM
bands for Copter, Plane, Rover and PX4 parameter schemas, both Copter Simple
masks, Current Mode/PWM, active-row highlighting and the reference help URL.
ArduPilot choices come from packaged metadata with unknown-value preservation
and the MP10 `31: ModelCal` extension; PX4 slot enum values remain separate
from packed heartbeat custom modes. A read-only MAVLink adapter accepts
heartbeat and RC data only for one immutable link/system/component/generation;
stale telemetry clears the live presentation. Edits require a fresh disarmed
heartbeat and Save submits one changed-only ordered exact-target batch, with
full refresh reconciliation after any possible partial application. Ordinary
parameter refreshes preserve staged edits, while reconciliation replaces them
with the exact committed snapshot.

Legacy `Heli Setup` is exposed only with MP10's exact `H_SWASH_TYPE`
capability. It is a dedicated non-empty native page, not Copter Basic Tuning:
all 43 reference logical fields/aliases, CCPM/H1, six visible manual-servo
actions, collective/acro plot, RC3/RC4 inputs, servo-output-6 cursor, three
position readouts and manual-only range capture are present. Writes use one
immutable exact target and finish by exact batch ID rather than an unrelated
same-name PARAM_VALUE. Non-zero modes require disarmed state and a
default-Cancel blade-removal confirmation; unsupported mode 5 remains visible
but disabled. Once a manual write is submitted, neither a raw zero echo nor a
lost ACK can clear its conservative uncertainty state; only the exact
successful zero batch can do that. Hiding/deactivating the page attempts
`H_SV_MAN=0`, and target or link uncertainty produces an operator-visible
warning. Current `H_SW_TYPE` helicopters now use the separate five-section
SETUP `Heli Setup (4.0+)` page.

Planner Settings now uses `ConfigPlannerView`, not `QGCSettingsWidget`, on the
active CONFIG route and the standalone Settings dialog. Its exact nine
reference sections are always present and non-empty; MP10's 64 interactive
controls are audited by type. The safe first slice makes units, the shared
layout profile, exact startup UDP policy, map backend, audio/heartbeat/logging,
log directories, beta channel and proxy effective. Missing speech, shortcuts,
target-safe rates/identity, map overlays/ADS-B and advanced policies are stated
in place. Useful legacy themes, file paths and seven-rate editing remain in an
explicit Legacy dialog; controls with no consumer or unsafe split ownership
are hidden.

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

The current navigation audit in `SETUP_INVENTORY_AUDIT.md` finds 53 MP10 pages versus 43 concrete Qt page factories. Both have the same three named group headings, for totals of 56 versus 46 navigation entries. Qt is missing 11 MP10 pages and adds the useful native `QML Plugins` manager, so this is intentionally not a one-to-one count. The modern Heli route is first under Mandatory Hardware and uses `H_SW_TYPE`, correcting MP10's stale `H_SWASH_TYPE` gate without aliasing the two schemas; the current and explicitly named legacy Frame Type pages follow it and no longer depend on a late firmware-version callback to create their content. The current `Compass` route follows Accel Calibration and precedes the explicitly named `Compass (Legacy)` widget; its parameter/priority, onboard and Large Vehicle workflows use exact-target services, the dedicated `Compass/Motor Calib` route is now active under Optional Hardware, only OfflineMagFit remains absent from the Compass family, and the legacy global dialogs stay quarantined. The Flight Modes route after ESC Calibration now shares CONFIG's native exact-target six-row page instead of the global-UAS legacy widget. The SETUP `OSD` route uses the dedicated `ConfigHWOSDView` legacy MinimOSD telemetry helper; the separate CONFIG `Onboard OSD` route now uses its own phase-one layout editor, never the unrelated old `OsdConfig`. One shared `displayview` service preserves all 35 MP10 SETUP feature flags; 31 currently gate matching factories and the four flags for missing FFT, Joystick, PX4Flow and REPL routes remain dormant. SETUP persists/restores `setup_lastpage`; missing routes remain missing and useful Qt-only/legacy pages remain honestly classified.

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with six working shared tools (MAVLink Inspector, Mavlink Mirror, NMEA, Cursor-on-Target / TAK, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Developer Tools exposes the exact 32-action inventory with working byte/MAVLink/hardware-ID parsers plus the shared Device Operations window: 3 of 32 actions are usable and the other 29 are explicitly disabled. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

The first-activation SETUP stall is fixed: `ApmCustomFirmwareConfig` yields before serial enumeration, downloads the manifest asynchronously through a dedicated reply, uses progress-based inactivity watchdogs, and keeps firmware download/upload state isolated. Flash actions retain an immutable selected-device snapshot and remain locked until a terminal upload outcome; destruction aborts replies and stops uploader timers. The still-large uncompressed manifest, GUI-thread JSON parsing, serial enumeration and complete MP10 firmware-selector parity remain explicit follow-up work.

`ESP8266 Setup` is now routed after Parachute and uses a dedicated exact-link client for `MAV_COMP_ID_UDP_BRIDGE` instead of changing the globally selected autopilot component. It waits for the 18 MP10 settings, displays the exact control set/defaults, serializes all 22 parameter writes and ACK-gates storage, reboot and reset. Unit/UI/transport coverage includes wrong-link replies, retry, partial response, target invalidation, reentrant cancellation and destruction; live bridge hardware, profile gating and cross-platform visual evidence remain.

The next audited SETUP packages are CubeID and Secure. CubeID is a target-aware firmware updater, not another HW-ID presentation; its CubePilot messages are absent from the current generated dialect and require a coordinated MAVLink update. `ConfigSecureView` manages bootloader public-key slots with `SECURE_COMMAND`, while `ConfigSecureApView` generates Ed25519 keys and signs bootloader/firmware files; MAVLink link signing remains a separate Advanced Tools workflow. The security pages need a reviewed cross-platform Ed25519 dependency before implementation.

The three Antenna Tracker routes are audited in `SETUP_ANTENNA_TRACKER_AUDIT.md`. The `Antenna Tracker (Serial)` and `Antenna Tracker (Live)` routes now exist after `ESP8266 Setup`: one shared `AntennaTrackerUIViewModel` (owned by `SetupView`, so the tracker loop survives page resets and navigation like MP10) drives `ConfigAntennaTrackerView` and `AntennaTrackerUIView` on top of `AntennaTrackerSerialService`, `AntennaTrackerGeometry` and a `UasAntennaTrackerTelemetrySource`; `antennatrackeruiviewmodel_tests` (11 cases) and `antennatrackerviews_tests` (4 cases) cover MP10 texts, validation order, settings, the loop, manual mode, live trim/reverse, failure recovery, the trim sweep and shutdown. `RadioStatusMonitor` now consumes both MAVLink radio-statistics messages per physical link and reproduces MP10's raw-unit SNR formula, 50 percent read-driven EMA and one-second hold; the telemetry source reads only the current exact target's link. The pages were smoke-tested on real X11 under Xvfb with a local SITL: both routes render, a real `QSerialPort` connect to a local UART reached `Connected (Maestro).`, Vehicle Az updated live and the application closed cleanly while connected. Deviations SETUP-018..022 record the shared instance, the Home / Center quirk, the intentional cancellable trim state machine, gating/tracker-home gaps and GUI approximations. The active sequence is the PLAN "Tracker Home" action, then the exact-target 25-field parameter page and remaining route/profile gates. The old `AntennaTrackerConfig` remains until these replacements are routed and verified.

Legacy APM Planner binary plugins are not a requirement. Where MP10 calls something a plugin/page/tool, reproduce the user-visible function with a native Qt page/service or the trusted QML extension system; do not restore the old ABI.

## Settings/CONFIG audit baseline

The active Settings path is `MainWindow -> ConfigView`; the compiled
`ApmSoftwareConfig` is not the production surface. MP10 and the truthful Qt
shell now both register 15 ordered concrete CONFIG factories. `MAVFtp` uses
the same functional native remote-file browser in CONFIG and SETUP, and legacy
`Heli Setup` now opens its own parameter/live-setup page. `Onboard OSD` opens a
dedicated, non-empty 30x16 phase-one layout
editor rather than the unrelated legacy MinimOSD stream-rate helper. It parses
complete EN/X/Y triplets, stages drag/toggle/coordinate edits, confirms refresh
when local changes exist, submits one exact-target batch and retains ownership
through partial failure, item cancellation and timeout until terminal batch
completion. Helicopter detection combines MP10's
`H_SWASH_TYPE` marker with an early heartbeat fail-safe and hides Copter Basic
rather than opening the wrong widget. The shared firmware
family now gives Plane Basic to VTOL and Rover Basic to surface boats. Flight
Modes is a native shared CONFIG/SETUP page; useful legacy GeoFence, vehicle
tuning and Planner factories remain under their reference headers with a
visible `Legacy` badge. Plane `QP Extended Tuning` is still
absent, while the deliberately narrower GeoFence vehicle restriction composes
with MP10's profile gate. The QtCore-only `ConfigRouteProfile` records all 15 MP10 routes in
reference order, separates reference visibility from vehicle/capability
actionability and tests offline/advanced, Copter, legacy/current Heli, Plane/VTOL,
Rover and per-feature profile gates. `ConfigView` consumes that policy instead
of maintaining a second set of vehicle lambdas. The deterministic JSON
`DisplayViewProfileService` supplies all 11 CONFIG and 35 SETUP flags, with 30
of the SETUP flags currently consumed by existing navigation factories,
preserves unknown Custom fields, starts Advanced when no profile is stored,
applies MP10's startup parameter-list overrides and keeps the old Advanced Mode
action synchronized as an exact Basic/Advanced preset switch. Existing
`QGC_MAINWINDOW/ADVANCED_MODE` state is migrated once when no profile exists.

The legacy Planner page now exposes the shared Basic/Advanced/Custom layout
selector and MP10's Startup UDP Listener controls while its native replacement
is built. The exact three MP10 keys default to independent ports 14550/14551,
validate 1..65535, collapse duplicates and take effect after restart.
LinkManager creates each configured listener unless that exact port already
exists, and excludes automatic listeners from the saved manual-link array, so
disabling them is effective on the next launch. A single hostless legacy 14550
definition is adopted once; manual definitions with outbound hosts are never
discarded. Failed automatic binds stop after one attempt, and editing an
automatic link explicitly converts it to a persisted manual definition.

The persistent blank Settings content path through vehicle/parameter resets is
fixed at the shared backstage boundary. Re-enabling automatic selection with
an empty stack now synchronously materializes the first visible concrete page,
while `ConfigView::resetVehiclePages()` preserves a still-visible current route
and the constructor's deliberately disabled lazy-selection state. Programmatic
visibility fallback is guarded so it does not overwrite the user's saved route.
The regression test proves a non-empty page ID, current widget and stack index
after the formerly blank reset ordering without eagerly constructing an earlier
factory. A broader production `ConfigView` matrix across Copter, Plane, Rover,
Heli, advanced mode and target changes is now covered at the pure production
route-policy boundary; a full singleton-backed `ConfigView` fixture remains
unnecessarily broad. The first truthful route/profile correction is now in
place. The next Settings package is the native Planner page and migration of
the already working Alt/Dist, map renderer/cache, audio/link and log controls,
followed by the later full-canvas/tuning-slot OSD
phases documented in `OSD_AUDIT.md`. The parity ledger retains the exact
per-route classification so missing pages cannot be mistaken for working
settings.

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
