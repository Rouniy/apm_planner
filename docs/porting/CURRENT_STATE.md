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
2. Continue the requested `TOOLS` stream after DataFlash Spectrogram, 3D Terrain View, External Guided, Follow Me, Moving Base, RF Propagation and OSD Video. Only the five safety-critical Swarm workflows remain disabled at the top level; no visible action may open an empty panel or silently do nothing.
3. After the principal Tools/dialog gaps, return to Settings/CONFIG; the incomplete controls inside the nine-section Planner page remain the broadest Settings gap.
4. Keep auditing the complete SETUP count and retain useful older APM Planner modules with honest Legacy/partial labels. Joystick/PX4Flow/FFT/REPL, Tracker Home/parameter and DATA HUD work remain tracked.
5. Track functional and GUI inaccuracies separately and keep committing complete slices rather than disconnected stubs.

## Verified checkpoint

The pre-Wave-1 functional checkpoint was `fb4f08b5` (`feat: port MinimOSD telemetry helper`), following `4290221f` (`feat: add antenna tracker output protocols`) and `e6fab89e` (`feat: port ESP8266 setup workflow`). The later verified checkpoints include `9d69cebb` (PLAN wrapper geometry), `9b180eba` (waypoint row actions), `e6d9e75b` (independent Inspector windows), `fbeacd6e` (Antenna Tracker pages) and `e7d5e58b` (their parity-ledger checkpoint).

At this checkpoint:

- CMake configure and `cmake --build build-codex-qt -j12` completed successfully; Claude's separately leased `build-claude` target also compiled and tested the NMEA sentence builder.
- The complete test suite passes with the Tools catalogue, Link Statistics, Tlog Convert / Extract, MAVLink Mirror, NMEA Output, CoT/TAK, Device Operations, DataFlash Spectrogram, 3D Terrain View, External Guided, Follow Me, Moving Base, RF Propagation settings/core/exact-target telemetry/DATA+PLAN overlays, OSD Video local decode/tlog timeline/MJPEG writer/window, CONFIG Onboard OSD, DisplayView profiles, Planner startup UDP, native HUD/speech/Message Severity Planner controls and exact-target periodic/high-message speech source/queue, the native Planner Settings page, shared CONFIG/SETUP MAVFTP, both Heli Setup schemas, both current/legacy Frame Type pages, the native current Compass parameter/onboard/fixed-yaw workflow, the dedicated Compass/Motor route, shared native Flight Modes, Plane QP Extended Tuning and the production SETUP factory audit: **175/175 tests**.
- Real X11 smoke verifies the exact 24-entry MP10 TOOLS order without late HIL/custom/panel actions, independent Link Statistics, Tlog Convert, MAVLink Mirror, NMEA Output, CoT/TAK, Device Operations, DataFlash Spectrogram, 3D Terrain View, External Guided, Follow Me and Moving Base windows from the main surface, modeless Inspector, Map Tile Cache, Plugin Manager and Log Download windows, Developer Tools navigation, and clean application exit with tool windows open. The Spectrogram run opens the production window through Ctrl+L, verifies the non-empty 1050×820 X/Y/Z surface and records `/tmp/apm-spectrogram-smoke.png`. The Terrain run opens the production entry through `TOOLS`, receives a live exact `SYS 1:1` target, renders a 33×33 elevation mesh with telemetry and hover coordinates, opens a second independent instance and closes both bounded windows; evidence is `/tmp/apm-terrain-smoke.fxoUiI/terrain-hover.png` and `/tmp/apm-terrain-smoke.fxoUiI/terrain-second.png`. The External Guided run opens the enabled TOOLS route and non-empty 620×390 window, verifies the default-Cancel warning, receives Accepted from a live local ArduCopter SITL, rereads an edited file on the next cycle and shows explicit Busy in a second independent window; evidence is `/tmp/apm-external-guided-smoke.VQu32f/tools.png`, `/tmp/apm-external-guided-smoke.VQu32f/external-guided.png`, `/tmp/apm-external-guided-smoke.VQu32f/confirmation.png`, `/tmp/apm-external-guided-smoke.VQu32f/running.png`, `/tmp/apm-external-guided-smoke.VQu32f/running-updated.png` and `/tmp/apm-external-guided-smoke.VQu32f/second-busy.png`. The Follow Me run clicks the enabled production TOOLS row, renders a non-empty 480×420 client window and exits with status 0; evidence is `/tmp/apm-followme-final-smoke.eT2MTm/tools.png` and `/tmp/apm-followme-final-smoke.eT2MTm/follow-me.png`. The Moving Base run clicks the enabled production TOOLS row, renders the non-empty 580×500 Serial/TCP/UDP/status surface and exits with status 0; evidence is `/tmp/apm-movingbase-smoke-live/tools.png` and `/tmp/apm-movingbase-smoke-live/moving-base.png`. The Mirror smoke used a live UDP heartbeat source, received 64 framed bytes through TCP Host, observed Tx accounting and Listening after client disconnect, then opened a second independent window and exited with status 0. A separate write-back run sent the exact `WRITEBACK_MARKER_4096` bytes from the TCP peer through the pinned UDP vehicle link, showed Rx 21 with the checkbox enabled and exited cleanly. The NMEA smoke received 100 TCP Host sentences from a live local MAVLink source, verified the first GGA/GLL/HDG/VTG/RMC cycle and its five checksums, opened a second independent window and exited with status 0. The CoT smoke opened the enabled menu route on a live UDP vehicle, selected TCP Host, delivered the same parseable CoT 2.0 event to two simultaneous clients, opened a second independent window and exited cleanly with the first session active. The Device Operations smoke opened the 760×520 window through TOOLS, opened a second independent instance through Ctrl+J, then opened a third through SETUP → Developer Tools and exited with status 0.
- The RF Propagation production smoke opens the enabled main-window shortcut into a complete 480×700 settings surface, opens a second independent modeless instance and exits cleanly with both windows present. Evidence is `/tmp/apm-rf-propagation-final-smoke.96e5GV/rf-settings.png` and `/tmp/apm-rf-propagation-final-smoke.96e5GV/two-rf-settings.png`; representative live terrain/vehicle overlay evidence remains.
- The OSD Video production smoke clicks its enabled bottom TOOLS entry with no external GStreamer sink override, opens the complete 1120×720 modeless source/tlog/offset/preview/output surface and keeps the application alive. Evidence is `/tmp/apm-osd-production-smoke.khi1Ik/result.png`. Synthetic Qt Multimedia decode and production-writer decode pass; representative real camera/tlog export, reference comparison and native-platform evidence remain.
- A new real-X11 CONFIG smoke used a local UDP target with two OSD screens and nine committed parameters, clicked the production `Onboard OSD` navigation row, rendered the non-empty canvas plus ALT/BAT item editors and closed the application cleanly. Evidence is `/tmp/apm-osd-smoke.kLNaLd/osd.png` for this workspace run; reference screenshot diff and physical-vehicle writes remain.
- A real-X11 Flight Modes smoke used one isolated exact Copter endpoint with a complete nine-parameter snapshot and live RC5=1500. It selected both production rows independently through SETUP and CONFIG/Settings and verified Current Mode=Auto, numeric Current PWM, all six rows, Save/Refresh/help controls and the green active-row paint before a clean exit. Evidence is `/tmp/apm-flightmodes-smoke.qQEFJI/setup-flight-modes.png` and `/tmp/apm-flightmodes-smoke.qQEFJI/config-flight-modes.png`; physical Copter/Plane/Rover/PX4 writes, reference screenshot diff and native-platform evidence remain.
- A real-X11 Plane CONFIG smoke used an isolated disarmed QuadPlane endpoint with `Q_ENABLE=2` and representative Q controller, RC and INS parameters. It clicked the production `QP Extended Tuning` row, rendered the non-empty 17-group/68-row native surface, exercised its inner scroll through Harmonic Notch and Filter Logs, and exited with status 0. Evidence is `/tmp/apm-qp-extended-smoke.TWFM0D/qp-extended-top.png` and `/tmp/apm-qp-extended-smoke.TWFM0D/qp-extended-bottom.png`; physical writes, reference screenshot diff and native-platform evidence remain.
- Real-X11 CONFIG smokes click the production `Planner` row and render the native scrollable Planner Settings page instead of the old generic widget. The original evidence `/tmp/apm-planner-settings-smoke.pSKCFA/planner-lower.png` covers the lower sections; the updated live-control run `/tmp/apm-planner-live-smoke.3anzjo/planner-after.png` toggles HUD off and Speech on, reports the intentionally missing developer-build speech engine, persists the exact `CHK_hudshow=false` and `speechenable=true` keys, switches to DATA without restarting and leaves its HUD host visibly blank in `/tmp/apm-planner-live-smoke.3anzjo/data-hud-disabled.png`, then exits with status 0. All nine section/order and bidirectional service checks are deterministic in `configplannerview_tests` and `hudcontrol_tests`.
- Planner speech has two real-X11 follow-ups. The first opens the original five Speech event controls only after `Enable Speech`, completes the production Mode and Battery prompts and verifies their exact persisted values (`/tmp/apm-planner-speech-smoke.PE5xfb/planner-after.png`). The current run shows all eight MP10 speech sub-controls in reference order, persists `speechenable=true` and exits cleanly (`/tmp/apm-planner-speech-c2.Jz91ZI/planner-eight-controls.png`). Event gates, transactional cancellation, unit conversion, cadence, exact-link filtering, No Data and bounded queue behavior are deterministic in `speechsettings_tests`, `speechannouncer_tests`, `speechtelemetrysource_tests`, `queuedspeechcontroller_tests`, `configplannerview_tests` and `batterymonitor_tests`.
- The current Message Severity X11 run drives one real production UDP endpoint, opens CONFIG -> Planner with persisted `severity=6`, verifies the visible `Message Severity = Info` control, switches through the production DATA action and observes `STATUS ERROR FROM EXACT LINK` on the native HUD before a clean exit. Evidence is `/tmp/apm-status-smoke.Ucj2NV/planner-severity.png` and `/tmp/apm-status-smoke.Ucj2NV/data-high-message.png`; the deterministic settings, chunking, normalization, target-epoch, speech and color cases remain covered by the full suite.
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

The parity inventory currently has 129 product rows: 61 `in-progress`, 37 `partial`, 31 `not-started`, and no row is considered complete yet under the strict visual/cross-platform evidence rule. The duplicate legacy-Heli inventory row has been removed. This means a large amount of global functionality still remains; passing tests does not imply Mission Planner parity.

## Implemented foundations worth reusing

These are working foundations, though their parity rows may remain partial because visual, hardware or cross-platform evidence is outstanding:

- Mission Planner-like main shell and deterministic DATA/PLAN `QSplitter` layouts without KDDockWidgets.
- User-selectable compiled map backends through a common abstraction and one canonical filesystem tile cache.
- DATA HUD/map/telemetry surface and PLAN mission table/map/action surface.
- Typed Mission, Fence and Rally stores; mission file/transfer paths; QGC Plan and legacy fence/rally file handling.
- PLAN mission editing, context commands, store-aware undo, route geometry, polygon drawing/offset/fence conversion, Survey/Corridor import, terrain source, elevation graph and distance measurement.
- Exact `(link, system, component)` vehicle target registry, shared link transmitter, command service, parameter service and committed parameter store.
- Backstage SETUP/CONFIG infrastructure, the phase-one Onboard OSD layout editor and a substantial set of hardware/parameter pages listed in the parity ledger.
- A native Plane `QP Extended Tuning` page with all 17 MP10 groups and 68 stable rows, Plane-first Q aliases, metadata numeric/enum/bitmask editors, per-field unavailable state, full Roll-to-Pitch lock behavior and the more-than-double confirmation. It pins one exact target/component, gates Q rows through the complete `Q_ENABLE` state while retaining common RC/INS controls, blocks writes while armed or heartbeat-stale and reconciles partial ordered batches before editing resumes. Copter/Heli retain their useful legacy Extended editor under the same stable route id.
- A native Planner Settings page with the exact nine-section MP10 topology on the canonical CONFIG route, live unit/profile/runtime controls, shared live HUD-overlay and speech-policy services, all eight MP10 event gates/templates, exact-target periodic/No Data policy, a bounded serialized TTS queue, a truthful Test Speech status, dual startup UDP policy and an explicitly bounded Legacy settings dialog. CONFIG is now the sole native Planner route: the orphaned standalone action and duplicate top-level dialog have been removed to match MP10.
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

The header menu now has the exact 24 MP10 items, order, separators and shortcuts. Nineteen routes are enabled because they have a truthful presentation: Developer Tools, Plugin Manager, MAVLink Inspector, MAVLink Mirror, NMEA Output, Cursor-on-Target / TAK Output, Map Tile Cache, MAVLink Device Operations, Link Statistics, Connection Options, Download Logs (MAVLink), Tlog Convert / Extract, DataFlash Spectrogram, 3D Terrain View, External Guided (File), Follow Me, Moving Base, RF Propagation Settings and OSD Video. Only the five Swarm entries remain visibly disabled as `not ported yet`; they cannot flip a check mark, open an empty dock or silently no-op. Installed trusted QML tools may append one clearly named extension submenu after the reference inventory.

`LinkStatsWindow` is a fresh 300×250 modeless window per invocation. Every refresh resolves the current exact target to its physical link again, converts the rolling Qt bit rate to bytes per second and reads per-link MAVLink received/lost counters. Link removal or absence shows zero/em-dash values and an explicit status. `LogDownloadDialog` now closes through normal dialog lifecycle, clears an interrupted vehicle's state/connections and presents an explicit no-vehicle state.

`MavlinkLogWindow` is a fresh 460×340 modeless window per invocation. Its private-buffer reader streams timestamped MAVLink 1/2 records without sharing a protocol channel, resynchronizes corrupt input, and cancellation or failure leaves no partial single-file output. KML/GPX use valid `GLOBAL_POSITION_INT` tracks, CSV/text decode bundled-dialect fields, parameter extraction preserves ArduPilot versus bytewise wire semantics, and mission extraction requires complete per-endpoint transfers and deduplicates snapshots. The source tlog can never be selected as an output, and automatically named mission siblings cannot silently replace existing files. All exports use an explicit sensitive-data confirmation whose default and Escape action is Cancel. Matlab remains visibly unavailable until a verified cross-platform MAT-file writer exists.

`SpectrogramWindow` is a fresh 1050×820 modeless window from both TOOLS/Ctrl+L and SETUP Advanced. It opens Unicode-path DataFlash `.bin`/`.log` files, selects ACC1–5/GYR1–5 and computes separate 1024-point Hann-windowed FFT images for X/Y/Z off the UI thread. A lightweight log-sink interface lets the existing ASCII/binary parsers stream only the chosen sensor instead of retaining the complete log. Direct ACC/GYR rows use fourfold overlap and split only on large forward timestamp gaps, so parser-repaired duplicate timestamps remain contiguous; current `QHBBHHQf` ISBH plus `QHHaaa` ISBD batches are correlated by number/type/zero-based instance/sequence with incomplete-batch fallback, and the ASCII parser accepts both flattened and bracketed `int16[32]` array fields. `SampleUS` has priority in the modern direct `ACC,QBQfff` layout. Parsing is cancellable, selected input is capped at 2,097,152 samples and each output is capped at 2048×512 by default; both ASCII and binary parsers now have a direct regression proving that a rejecting bounded sink stops parsing after the rejected row. When more FFT windows exist than raster columns, per-frequency maximum pooling retains transient peaks. The two obsolete legacy Load-button Ctrl+L bindings were removed so the MP10 Tools shortcut remains unambiguous. Deterministic direct and synthetic Unicode ASCII/binary batch tests cover axis peaks, the fifth sensor, malformed input, bounds, repaired timestamps and teardown.

`Terrain3DWindow` is a fresh 1100×760 modeless software-rendered heightfield view from both TOOLS and SETUP Developer Tools; it deliberately does not enable the unrelated legacy OSG/QGLWidget `Pixhawk3DWidget`. It binds HEARTBEAT, GLOBAL_POSITION_INT and ATTITUDE to one exact `(link, system, component, generation)` epoch, clears stale frames on target loss/change and shows armed state, mode, coordinates, AMSL, attitude and velocity. Cancellable workers sample the common local-GDAL/SRTM elevation source into a bounded 17–65 grid, build elevation shading/fog/grid/MAV marker frames and publish only while the captured target epoch is still current. Lock-to-MAV/free-camera movement, vertical exaggeration, automatic recenter, reload coalescing and terrain hover raycasts are active. Imagery controls remain visibly disabled until the canonical shared tile cache can supply a bounded in-memory atlas. The shared guided service now exists, but clicks stay read-only until current-altitude, per-click default-Cancel confirmation, explicit policy and tests are added. Core geodesy/dateline/projection/render, telemetry isolation/reentrancy and window lifecycle tests plus a live X11 main-menu smoke pass.

`ExternalGuidedWindow` is a fresh 620×390 modeless window from both TOOLS and SETUP Advanced. It accepts a bounded Unicode-path file containing exactly three C-locale fields, validates it before a default-Cancel warning and rereads it after consent so a changed command cannot bypass confirmation. The application-owned service pins one `(link, system, component, generation)` lease with a fresh heartbeat, serializes one owner, queues only the newest update and sends `COMMAND_INT DO_REPOSITION` with CHANGE_MODE until the first terminal Accepted acknowledgement. One-second rereads resume only after the preceding command is accepted; invalid runtime contents withhold the update without stopping. A lost acknowledgement retries the identical target up to three sends; an Accepted result after retry drains possible additional retry acknowledgements before sending the newest queued target. Stop releases its window immediately and ambiguous outcomes use a bounded late-ACK isolation interval. Since MAVLink carries no command transaction ID or echoed coordinates, the UI/documentation treats Accepted as command-level rather than proof of a particular coordinate payload. Target changes stop the session, `(0,0)` is withheld, and transport/rejection/unsupported states remain visible. MP10's legacy Plane/current=2 and unacknowledged position-target fallbacks remain compatibility work.

`FollowMeWindow` is a fresh 480×420 modeless window from both TOOLS and SETUP Advanced. It accepts either validated manual WGS84 coordinates or framed serial NMEA GGA input, with explicit port/baud, 0.25/0.5/1/2 Hz rates and relative altitude. Start requires a default-Cancel movement warning, then reserves the same application-owned guided sender against one fresh immutable target before opening the serial port; no command is sent until a valid fix exists. Valid updates are newest-wins and ACK-gated through `DO_REPOSITION`; malformed, no-fix and stale input withhold movement, while target change, link loss, input failure, Stop or window destruction ends the session and releases ownership. Exact `(0,0)` is deliberately rejected as ambiguous, but either zero-valued axis remains valid. The port does not reproduce MP10's unacknowledged fixed-rate/legacy fallbacks: the heartbeat freshness bound and command-level acknowledgement remain visible safety differences pending physical serial, older-autopilot, reference-screenshot and native-platform evidence.

`MovingBaseWindow` is a fresh non-empty 580×500 modeless window from both TOOLS and SETUP Advanced. It reserves one immutable exact vehicle target before opening an input, accepts framed GGA from Serial 8N1, newest-client TCP Host, TCP Client, first-sender-pinned UDP Host or an exact-endpoint UDP Client that announces its return port with one empty discovery datagram, and publishes the newest fix at 0.25/0.5/1/2 Hz. No-fix or stale input clears the Flight Data cyan `BASE` marker while leaving the input session active; target/link/owner changes stop and clear it. Host listeners bind all IPv4 interfaces but UDP Host refuses any port owned by an active MAVLink link and uses an exclusive bind. Settings persist only after Listening/Ready, and raw lines are bounded to 4096 bytes in a 4 MiB active log plus one capped backup. TCP Client deliberately does not auto-reconnect yet. Unlike MP10, Moving Base does not feed NTRIP GGA. Relative altitude and Rally Point 0 stay visible but disabled until an exact home-altitude source and an exact `MAV_MISSION_TYPE_RALLY`/`MISSION_ACK` transaction exist; MP10 has no position-offset controls. Unit, full-suite and production X11 click/open/clean-exit evidence pass; physical serial/network sources, live marker evidence, reference screenshot and native platforms remain.

`PropagationSettingsWindow` is a fresh non-empty 480×700 modeless window from TOOLS/Ctrl+W. It exposes and autosaves all 16 MP10 propagation settings. Application-owned DATA and PLAN controllers independently render elevation/terrain rasters, a 360-degree Home-centred RF terrain-intercept contour and red/orange Home/vehicle battery-distance rings through the shared map contract. Cancellable generation-guarded workers use the common GDAL/SRTM elevation source, a bounded 4096-pixel raster dimension and bounded RF sampling; SRTM admission is deduplicated before GUI dispatch and capped at eight unique queued/active tiles. Stale viewport, settings, terrain or exact-target epochs cannot replace newer results. Missing/ocean/zero raster samples remain transparent and missing RF sectors suppress the complete contour with a visible status rather than claiming verified coverage. Wrapped vector paths do not draw false world-spanning chords; raster extents crossing the dateline remain fail-closed until they can be split. The 0.5-degree azimuth option is functional and zero convergence cannot enter an unbounded loop. Exact-target telemetry combines HEARTBEAT, HOME_POSITION, GLOBAL_POSITION_INT/GPS_RAW_INT and primary battery data; distance-left is unavailable until consumption and valid armed GPS movement exist. Another production map backend, reference screenshots, native platforms and representative live terrain/vehicle evidence remain.

`OsdVideoOverlayWindow` is one modeless 1120×720 TOOLS/Developer Tools window. It loads a local video through Qt Multimedia, asynchronously builds a 100 ms `.tlog` state timeline after the first `MAV_COMP_ID_AUTOPILOT1` heartbeat, applies a -900..900 second offset and renders the private default `HudControl` into a new silent JPEG85 MJPEG AVI. Source resolution or a maximum 960-pixel width is selectable, input/output identities and overwrite are rejected, cancellation finalizes a playable partial file, and all frame queues are bounded: source frames are never silently substituted and overrun fails explicitly. Microsecond PTS rounding preserves ordinary CFR frames and EndOfMedia waits for accepted cross-thread deliveries. On Linux/Qt5, only the two unused eager GStreamer display controls are temporarily assigned inert sinks during player construction, avoiding an Intel VA-API crash while the real surface remains `QAbstractVideoSurface`. Audio copy, complete firmware-specific CurrentState/mode coverage, real camera/tlog reference evidence and Windows/macOS validation remain.

`SerialPassThroughWindow` is a fresh 480×380 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Start snapshots the current exact target's physical link; successful inbound MAVLink frames are reconstructed byte-for-byte after framing and forwarded without touching the original traffic. Serial uses 8N1/no flow control, TCP Host retains one newest client, and UDP Host learns and fans out to a bounded peer set but refuses a collision with an active vehicle UDP listener. A 256 KiB total mirror-owned backpressure bound drops newest complete frames with a visible counter instead of blocking the protocol thread. Peer bytes are counted and drained with write-back disabled by default; enabling it writes raw bytes through an ephemeral link-ID lookup and automatically disables the reverse path if the link rejects them. Closing one window stops only its own service.

`SerialOutputNMEAWindow` is a fresh 480×400 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. Connect pins the exact `(link, system, component)` target and stops explicitly if that physical link disappears. `GLOBAL_POSITION_INT` drives position/altitude, with `GPS_RAW_INT`, `VFR_HUD` and `ATTITUDE` fallbacks/overrides matching MP10 precedence. Each timer tick emits one complete GGA/GLL/HDG/VTG/RMC cycle with checksums and CRLF through bounded Serial 8N1, newest-client TCP Host or learned-peer UDP Host outputs on port 14551. Nothing is written before the first usable `GLOBAL_POSITION_INT` or any `GPS_RAW_INT`; matching MP10 state semantics, a no-fix GPS report can still produce zero coordinates with GGA quality 0. Transport failures remain visible. Closing one window stops only its own service, and guarded service pointers make destruction order safe in both NMEA and Mirror windows.

`SerialOutputCotWindow` is a fresh 720×820 modeless window per invocation and is reachable from both TOOLS and SETUP Advanced. It reproduces the reference endpoint order/defaults, 0.1–3600 second interval, event/UID/callsign controls, six-column advanced identity grid, Connect/Stop, indentation and last-event preview. Connect pins the current physical link but emits for every exact system/component endpoint discovered on that link, matching MP10's one-comPort scope without collapsing equal IDs on other links. TAK multicast, UDP Client/Host, TCP Client/Host and Serial 8N1 are separate bounded transports; TCP Host broadcasts to up to 16 clients and UDP Host targets the newest sender. XML attribute/element order, precision, escaping, UTC start/stale and platform-native indentation match MP10, while invalid XML/non-finite state is rejected visibly. Closing one window stops only its session.

MAVLink Device Operations is now a fresh independent modeless window from TOOLS, Ctrl+J and SETUP → Developer Tools. Its event-driven service sends DEVICE_OP_READ/WRITE only on one generation-checked physical-link lease, rejects MAVLink 1, correlates replies by exact link/source/type/request ID, bounds all protocol fields and cancels safely on target change, link loss or destruction. The destructive ICM20948 write/read test requires a current disarmed heartbeat and refuses an edited destination outside the bound endpoint; ordinary register reads retain MP10's editable destination on the pinned link. The five remaining Swarm Tools stay visible but disabled until each complete workflow is ready; Settings/CONFIG follows that final Tools/dialog stream.

The shared MAVFTP remote-file browser is now routed from both CONFIG and SETUP. It provides the MP10 Refresh/Download/Upload/Delete/Mkdir workflow plus explicit cancellation, lazy `/` and `@SYS` browsing, progress, atomic `QSaveFile` downloads with collision-avoiding names, and a default-Cancel warning before a potentially replacing upload. Its application-owned service pins one exact physical-link target per operation, strictly correlates the MAVFTP response envelope, paginates directories, retries with per-operation bounds, closes and flushes upload sessions before a 30-second-budget CRC verification, and cancels safely on target/link changes. The current safety bound buffers files up to 64 MiB; exclusive no-replace local creation, burst/streaming transfer and live hardware evidence remain.

## Settings/CONFIG current phase

`CONFIG_SETTINGS_INVENTORY_AUDIT.md` is the count baseline. MP10 and Qt now
both have 15 ordered concrete CONFIG factories and every supported vehicle
route dispatches to a truthful non-empty page. Plane now receives the native
QP Extended Tuning surface while Copter/Heli keep the useful legacy editor;
Flight Modes uses one native exact-target page shared with SETUP. Other useful
legacy tuning and GeoFence pages remain visibly classified instead of being
deleted to improve the raw count.

The latest source audit confirms 15/15 concrete direct route factories and all
nine Planner sections, but only 21 of the 64 MP10 Planner controls are native
equivalents. CONFIG → Planner is the sole production and source route: the
orphaned `actionSettings` path and duplicate top-level dialog have been
removed. The page now distinguishes Layout from the unported MP10 Theme/Edit
Custom editor and explicitly discloses the missing dist-to-home, Track Length
and safe runtime GCS-sysid controls. A production click-through test for every
CONFIG row is the next route-level Settings check.

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
active CONFIG route. A second source-level dialog shares that page but is
orphaned and is not counted as a production route. The exact nine
reference sections are always present and non-empty; MP10's 64 interactive
controls are audited by type. The safe slices make units, the shared
layout profile, exact startup UDP policy, map backend, audio/heartbeat/logging,
log directories, beta channel and proxy effective. HUD Overlay, Speech master,
Test Speech, Armed Only, Waypoint, Mode, Custom, Battery, Alt Warning,
Arm/Disarm, Low Speed and Message Severity now work live through shared
application services, bringing direct MP10 coverage to 21 of 64 controls.
Periodic phrases, the armed No Data warning and complete MAVLink 2 STATUSTEXT
messages consume one exact physical-target telemetry epoch; high messages use
the MP10 threshold/prefix policy, legacy ArduPilot severity normalization,
ten-second red/yellow/white HUD presentation and one-shot bounded TTS queue.
The retained Qt PreArm/Arm/#audio phrases now enter that same assembled,
deduplicated path instead of bypassing it. Vario and speech level remain the
next speech gaps; shortcuts, target-safe rates/identity, map overlays/ADS-B and
advanced policies are stated in place.
Useful legacy themes, file paths and seven-rate editing remain in an
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

`setupviewroutes_tests` now runs inside the BUILD_TESTING production binary and constructs the real hidden `SetupView`: it locks the exact 43-page/three-group order, every real factory, click selection, stack ownership, semantic non-empty content and Optical Flow reset/recreation without invoking page controls or lifecycle network/hardware actions. This audit exposed a real shutdown UAF: page `destroyed` callbacks could run only after `SetupView` members were gone. `SetupView` now disables fallback selection and destroys every created page through `resetPage()` while its services, leases and weak owners are still alive; the regression and a Valgrind rerun pass. Connected/vehicle/profile visibility matrices remain a separate follow-up.

The verified SETUP packages include the common `ActionPageView`, the exact 16-action `ConfigAdvancedView` inventory with ten working shared tools (MAVLink Inspector, Mavlink Mirror, NMEA, Cursor-on-Target / TAK, DataFlash Spectrogram, External Guided, Follow Me, Moving Base, Map Tile Cache and Proximity), a standalone inspector window, Advanced Terminal, and a user-facing trusted QML plugin manager. Six Advanced actions remain explicitly disabled. Developer Tools exposes the exact 32-action inventory with working byte/MAVLink/hardware-ID parsers plus the shared Device Operations, 3D Terrain and OSD Video windows: 5 of 32 actions are usable and the other 27 are explicitly disabled. Default Settings, HW ID and ADSB are routed in MP10 order and connected to the committed exact-target parameter snapshot; ADSB identification uses the application-owned exact-link transmitter rather than a legacy global send path.

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
visible `Legacy` badge. Plane `QP Extended Tuning` uses its native page, while
the deliberately narrower GeoFence vehicle restriction composes
with MP10's profile gate. The QtCore-only `ConfigRouteProfile` records all 15 MP10 routes in
reference order, separates reference visibility from vehicle/capability
actionability and tests offline/advanced, Copter, legacy/current Heli, Plane/VTOL,
Rover and per-feature profile gates. `ConfigView` consumes that policy instead
of maintaining a second set of vehicle lambdas. The deterministic JSON
`DisplayViewProfileService` supplies all 11 CONFIG and 35 SETUP flags, with 31
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
place. The next Settings package should add the common unit service/speed
controls and OSD text/background color, followed by the remaining Planner
speech consumers and then the later full-canvas/tuning-slot OSD
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
