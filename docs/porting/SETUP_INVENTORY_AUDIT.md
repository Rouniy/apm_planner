# SETUP inventory audit

Updated: 2026-09-04.

This audit compares the active Mission Planner 10
`ViewModels/SetupViewModel.cs` navigation with the active Qt
`SetupView::buildPages()` surface. It counts registered/potential navigation
entries, not support classes in the source tree; connection, profile and
advanced-mode gates mean they are not all visible simultaneously.

## Exact counts

Both applications use the same three named collapsible group headings:
`>> Mandatory Hardware`, `>> Optional Hardware`, and `>> Advanced`. Including
the ungrouped pages above them, the navigation therefore has four logical
sections.

| Logical section | MP10 pages | Qt pages | MP10 group heading | Qt group heading |
|---|---:|---:|---:|---:|
| Ungrouped | 4 | 1 | 0 | 0 |
| Mandatory Hardware | 16 | 15 | 1 | 1 |
| Optional Hardware | 26 | 19 | 1 | 1 |
| Advanced | 7 | 6 | 1 | 1 |
| **Total** | **53** | **41** | **3** | **3** |

Thus MP10 registers 56 potential navigation entries when group headings are
included. Qt registers 44: 41 page factories plus the same three group
headings. Every current Qt page registration has a concrete factory. Counts
alone do not imply parity because several factories still wrap legacy APM
Planner widgets rather than the corresponding MP10 implementation.

## Missing MP10 pages

Qt is missing 13 direct MP10 pages:

- Ungrouped: `Install Firmware Legacy`, `Secure`, and
  `Secure (Bootloader Keys)`.
- Mandatory Hardware: the current `Compass` page.
- Optional Hardware: `CubeID Update`, `NV Modem`, `Joystick`,
  `Compass/Motor Calib`, `PX4Flow`, `Antenna Tracker`, and `FFT Setup`.
- Advanced: `Onboard Lua REPL` and `Local Script REPL`.

Qt also has one intentional additional page, `QML Plugins`. It is the useful
user-facing manager for the fresh port's trusted QML extension system and must
remain. The arithmetic is therefore `53 - 13 + 1 = 41` Qt pages.

## Retained useful legacy modules

Existing APM Planner modules may remain when they provide a real and useful
workflow. Install Firmware, Accel Calibration, Radio Calibration, Flight Modes,
FailSafe, SiK Radio, Battery Monitor and Terminal are substantive same-domain
implementations worth retaining as explicitly `partial`. The vehicle pages
derived from `AP2ConfigWidget` still bind the global active UAS/parameter
manager and often force component 1, so none should be treated as exact-target
safe until its target lease, component filtering and ACK lifecycle are ported.

Several old modules are useful but misleading under an unqualified modern MP10
route. Keep their source, but label them Legacy/partial or replace the route.
`Frame Type` now uses a native `FRAME_CLASS` / `FRAME_TYPE` page and the useful
`FRAME` workflow is retained as the separate native `Frame Type (Legacy)`
route. The old combined factory remains available to the legacy shell but is
not an MP10 SETUP page. The old compass page is now called
`Compass (Legacy)` because it lacks current multi-compass discovery/priority.
Its useful basic fields remain, while Live, Onboard and CompassMot calibration
actions are disabled: the inherited dialogs bind the global active vehicle/link,
the live path can zero more offsets than it reconstructs, and the onboard path
can accept a failed result. The modern `Compass` page therefore remains a
visible inventory gap rather than being claimed by this widget. Range Finder
supports one old `RNGFND_*` instance, Airspeed
exposes only the old enable/use/pin choices, Optical Flow is only a
`FLOW_ENABLE` checkbox, and Camera Gimbal is the old single `MNT_*` surface.
Those four pages use parameter families that are obsolete on many modern 4.x
vehicles, so they should be explicitly labelled `(Legacy)` and shown only when
their legacy parameters exist. Existing Joystick and Compass/Motor code may
seed the missing direct routes after ownership and lifecycle review, but the
old CompassMot dialog is not an active SETUP route or a safe substitute.

Retention does not permit a misleading replacement. A legacy module may be an
additional clearly named route, or a temporary partial implementation of the
same workflow, but it must not perform an unrelated operation under an MP10
name. This is why the CONFIG `Onboard OSD` route now uses a separate phase-one
layout editor and never the unrelated legacy stream-rate helper, while useful
legacy SETUP hardware pages remain available.

## Ordering and visibility gaps

Qt preserves the three broad groups but not the complete MP10 order. Among
the completed direct routes, `Heli Setup (4.0+)` is now the first child of
Mandatory Hardware before the native `Frame Type`, `Frame Type (Legacy)` and
`Default Settings` sequence. Its five sections retain the exact
8x5 servo, 13 swashplate, 12 rotor-speed, nine governor and nine miscellaneous
bindings. The route deliberately checks the current `H_SW_TYPE` capability;
MP10's `IsHeli()` checks the obsolete `H_SWASH_TYPE` even for this 4.0+ page,
which would hide it from current-only vehicles and collide with the separate
legacy CONFIG editor.

Among pages common to both applications, the active inversion is
`DroneCAN/UAVCAN`:
MP10 places it immediately after `Battery Monitor 2`, while Qt places it after
both Antenna Tracker pages. The tracker pages correctly precede `HW CAN` in
both. Qt now has one shared JSON `DisplayViewProfileService`. It preserves all
35 MP10 SETUP feature flags; 30 currently gate corresponding existing
factories, including the independent CONFIG `displayOSD` versus SETUP
`displayOsd` distinction, CAN, tracker and Terminal flags. The five dormant
flags belong to the missing Compass/Motor, FFT, Joystick, PX4Flow and REPL
routes. Active gates compose with connection and conservative vehicle-family
checks; they never make a missing page appear.
Useful Qt-only `QML Plugins` and Advanced utilities without a reference feature
flag remain. `setup_lastpage` is persisted and restored.

Three narrower safety/truthfulness checks remain deliberate: Initial
Parameters stays Copter/Plane-only, Serial Ports requires a known firmware
family, and Motor Test excludes ArduSub. The profile service writes
deterministic JSON, preserves unknown Custom fields and omits legacy
XML/`advancedview` migration for this fresh port.

There are no null factories in active SETUP, but a concrete factory can still
be misleading. The native Frame pages build their complete unavailable state
before any firmware or parameter callback and therefore replace the combined
`FrameTypeConfig` blank/stale lifecycle on the active route. `Optical Flow` is
non-empty but near-placeholder functionality and remains the first production
navigation case the SETUP click/reset test must expose.

The required navigation regression test must lock:

- the 41 Qt page IDs and three group IDs in production order;
- a non-null concrete widget after activating every visible page;
- offline, connected, partial-parameter and advanced-mode visibility;
- Copter, Plane, Rover, Heli and tracker profile transitions;
- fallback selection after the current page is hidden or reset.

New MP10 routes should be added only as complete vertical slices. Useful
legacy pages and `QML Plugins` stay registered and are classified separately
instead of being removed to make the raw counts look closer.
