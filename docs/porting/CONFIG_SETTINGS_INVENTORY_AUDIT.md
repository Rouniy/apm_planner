# CONFIG and Planner Settings inventory audit

Updated: 2026-09-04.

This audit compares Mission Planner 10 `ConfigViewModel` and
`ConfigPlannerView.axaml` with the active Qt `ConfigView` and native
`ConfigPlannerView`. It counts registered routes and interactive controls,
not support classes or controls that are merely present in source files.

## CONFIG route count

Mission Planner 10 registers 15 ordered CONFIG routes. Qt models all 15 in
`ConfigRouteProfile` and now registers all 15 as concrete factories. There are
no null factories in the active Qt navigation.

| # | Mission Planner 10 route | Qt status |
|---:|---|---|
| 1 | Flight Modes | Native exact-target page; in progress |
| 2 | Standard Params | Native metadata-backed page; partial |
| 3 | Advanced Params | Native metadata-backed page; partial |
| 4 | GeoFence | Useful legacy Copter page retained; partial |
| 5 | Basic Tuning | Useful legacy Copter page retained; partial |
| 6 | Heli Setup | Native legacy-heli editor; in progress |
| 7 | Basic Tuning (Plane) | Useful legacy Plane page retained; partial |
| 8 | Basic Tuning (Rover) | Useful legacy Rover page retained; partial |
| 9 | Extended / QP Extended Tuning | Native Plane QP page; useful legacy Copter/Heli editor retained |
| 10 | Onboard OSD | Native phase-one editor; in progress |
| 11 | MAVFtp | Native remote file browser and exact-target service; in progress |
| 12 | User Params | Native metadata-backed page; in progress |
| 13 | Full Parameter List | Native staged parameter page; in progress |
| 14 | Planner | Native nine-section page; partial |
| 15 | Planner (Advanced) | Native read-only settings snapshot; in progress |

There is no longer a CONFIG route-level factory or vehicle-dispatch gap. The
shared route creates the native `QP Extended Tuning` page for Plane and keeps
the useful non-empty legacy editor for Copter/Heli. Existing APM Planner pages
remain only where they provide a same-domain workflow; their Legacy/partial
classification is not a parity claim.

Factory coverage is therefore 15/15, but functional parity is not. The current
truthful route policy deliberately narrows `Standard Params` and `Advanced
Params` to Copter/Heli/Plane/Rover and `GeoFence` to Copter/Heli. `Extended
Tuning` is actionable for Copter/Heli/Plane: the stable route id changes its
visible Plane label to `QP Extended Tuning` without losing saved selection,
then dispatches Plane to the native Q controller/INS page and Copter/Heli to
the retained legacy ATC/PSC/WPNAV editor.

The related SETUP `Advanced` launcher is a separate auxiliary-tools inventory:
all 16 MP10 actions are present, six open working shared tools and ten are
visibly disabled. In particular, `Warning Manager`, `FFT` and `Spectrogram`
are not yet ported; `FFT Setup` is also still a missing direct SETUP page. They
must not be counted as working merely because every direct CONFIG route has a
factory.

The legacy `Heli Setup` route is capability-gated by the exact MP10
`H_SWASH_TYPE` marker; current `H_SW_TYPE` vehicles use the separate native
SETUP `Heli Setup (4.0+)` page and are never collapsed into the binary CCPM/H1
editor. The native page has the exact 43 logical parameter rows and aliases,
the six visible manual-servo commands, swash controls, collective/acro plot,
RC3/RC4 input, servo-output-6 cursor and observed ranges. Writes are disarmed,
exact-target and terminal-batch correlated. Non-zero manual modes require a
default-Cancel blade-removal confirmation, mode 5 stays disabled unless
firmware metadata declares it, and leaving the page makes a best-effort
`H_SV_MAN=0` request. Lost ACKs and raw parameter echoes cannot clear the
conservative manual-override latch; only an exact successful zero batch can.
Link/target uncertainty remains an operator-visible warning.

## Planner Settings section and control count

Mission Planner 10 has exactly nine Planner Settings sections, in this order:

1. Display
2. Speech
3. Flight Command Shortcuts
4. Waypoints / Connect
5. Startup UDP Listeners
6. Telemetry Stream Rates (Hz)
7. Aircraft Icon / Map
8. Logs
9. Advanced

The reference AXAML contains exactly 64 interactive controls: 35 checkboxes,
11 combo boxes, 11 numeric editors, five buttons and two text boxes. The
native Qt page now renders all nine sections in the same order, and every
section contains either a working control or an explicit unavailable note;
none is an empty placeholder.

Working settings in the first native slice are Alt/Dist units, the shared
Basic/Advanced/Custom DisplayView profile, exact restart-scoped dual Startup
UDP listeners, the Qt map renderer, audio mute, GCS heartbeat, MAVLink
logging, DataFlash/tlog directories, beta update channel and system proxy.
The two production entry points share one application-owned model, so one
open page cannot overwrite stale UDP/beta state from another.

The remaining MP10 controls are not represented as fake toggles. Language,
speed/HUD/severity, full speech/vario, safety-confirmed flight shortcuts,
connect policies, the five target-safe telemetry rates and GCS identity, map
vectors/overlays/cache/external ADS-B, and the remaining advanced policies are
called out in their corresponding section. Useful old APM Planner themes,
file paths and seven-rate telemetry editor remain reachable through an
explicitly named Legacy dialog; dead reconnect/donate/titlebar/low-power and
split-brain MAVLink identity controls are hidden there.

The native `Flight Modes` page is shared by CONFIG and SETUP. It renders all
six MP10 mode rows and PWM bands for Copter, Plane, Rover and PX4 schemas.
ArduPilot slot choices come from packaged metadata (plus the MP10
`31: ModelCal` Copter extension); PX4 `COM_FLTMODE1..6` uses its separate
0..12 slot enum and never the packed heartbeat `custom_mode`. Unknown values
remain selectable. Copter always exposes the `SIMPLE` and `SUPER_SIMPLE`
columns and help, while each column stays disabled when its parameter is not
provided by the vehicle. Current Mode,
numeric Current PWM, active-row highlighting and the Super Simple help route
consume telemetry from the immutable physical-link target rather than the
legacy global active-UAS stream. Edits require a fresh exact heartbeat and a
disarmed vehicle; Save submits one changed-only ordered parameter batch and a
partial outcome forces full-snapshot reconciliation. Full parameter refreshes
preserve staged per-field edits, while reconciliation deliberately replaces
them with the new committed snapshot. The additional `Refresh Params` action
is the explicit recovery path for incomplete or uncertain state. The old
`FlightModeConfig` source remains compiled for legacy-shell compatibility but
neither active MP10 route instantiates it.

The 15 direct CONFIG routes now all create a truthful non-empty page for their
supported vehicle contexts. Planner Settings is the next broad control-level
gap: all nine sections are present, but many of the audited 64 controls are
still explicitly unavailable.

## SETUP cross-check

The separate `SETUP_INVENTORY_AUDIT.md` remains the count baseline: both
applications have four logical sections (ungrouped plus three named groups).
Mission Planner 10 registers 53 pages plus three group headings, or 56
navigation entries. Rechecked after this CONFIG slice, Qt still registers 43
concrete pages plus the same three group headings, or 46 entries. Qt is missing
11 reference pages and intentionally keeps one useful extra page, `QML Plugins`.
