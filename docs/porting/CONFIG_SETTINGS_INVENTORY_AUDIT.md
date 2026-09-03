# CONFIG and Planner Settings inventory audit

Updated: 2026-09-04.

This audit compares Mission Planner 10 `ConfigViewModel` and
`ConfigPlannerView.axaml` with the active Qt `ConfigView` and native
`ConfigPlannerView`. It counts registered routes and interactive controls,
not support classes or controls that are merely present in source files.

## CONFIG route count

Mission Planner 10 registers 15 ordered CONFIG routes. Qt models all 15 in
`ConfigRouteProfile`, but deliberately registers only the 13 routes that have
concrete factories. There are no null factories in the active Qt navigation.

| # | Mission Planner 10 route | Qt status |
|---:|---|---|
| 1 | Flight Modes | Useful legacy page retained; partial |
| 2 | Standard Params | Native metadata-backed page; partial |
| 3 | Advanced Params | Native metadata-backed page; partial |
| 4 | GeoFence | Useful legacy Copter page retained; partial |
| 5 | Basic Tuning | Useful legacy Copter page retained; partial |
| 6 | Heli Setup | Missing; no misleading Copter substitute |
| 7 | Basic Tuning (Plane) | Useful legacy Plane page retained; partial |
| 8 | Basic Tuning (Rover) | Useful legacy Rover page retained; partial |
| 9 | Extended / QP Extended Tuning | Legacy Copter editor retained; Plane QP workflow missing |
| 10 | Onboard OSD | Native phase-one editor; in progress |
| 11 | MAVFtp | Missing; no unrelated substitute |
| 12 | User Params | Native metadata-backed page; in progress |
| 13 | Full Parameter List | Native staged parameter page; in progress |
| 14 | Planner | Native nine-section page; partial |
| 15 | Planner (Advanced) | Native read-only settings snapshot; in progress |

The raw factory gap is therefore two routes: `Heli Setup` and `MAVFtp`.
Plane `QP Extended Tuning` is a third functional mismatch even though the Qt
route count is occupied by a useful Copter-only legacy editor. Existing APM
Planner pages remain only where they provide a non-empty same-domain workflow;
their Legacy/partial classification is not a parity claim.

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

## SETUP cross-check

The separate `SETUP_INVENTORY_AUDIT.md` remains the count baseline: both
applications have four logical sections (ungrouped plus three named groups).
Mission Planner 10 registers 53 pages plus three group headings, or 56
navigation entries. Qt registers 38 concrete pages plus the same three group
headings, or 41 entries. Qt is missing 16 reference pages and intentionally
keeps one useful extra page, `QML Plugins`.
