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
| Mandatory Hardware | 16 | 13 | 1 | 1 |
| Optional Hardware | 26 | 18 | 1 | 1 |
| Advanced | 7 | 6 | 1 | 1 |
| **Total** | **53** | **38** | **3** | **3** |

Thus MP10 registers 56 potential navigation entries when group headings are
included. Qt registers 41: 38 page factories plus the same three group
headings. Every current Qt page registration has a concrete factory. Counts
alone do not imply parity because several factories still wrap legacy APM
Planner widgets rather than the corresponding MP10 implementation.

## Missing MP10 pages

Qt is missing 16 direct MP10 pages:

- Ungrouped: `Install Firmware Legacy`, `Secure`, and
  `Secure (Bootloader Keys)`.
- Mandatory Hardware: `Heli Setup (4.0+)`, `Frame Type (Legacy)`, and
  `Compass (Legacy)`.
- Optional Hardware: `CubeID Update`, `NV Modem`, `Joystick`,
  `Compass/Motor Calib`, `PX4Flow`, `Antenna Tracker`, `FFT Setup`, and
  `MAVFtp`.
- Advanced: `Onboard Lua REPL` and `Local Script REPL`.

Qt also has one intentional additional page, `QML Plugins`. It is the useful
user-facing manager for the fresh port's trusted QML extension system and must
remain. The arithmetic is therefore `53 - 16 + 1 = 38` Qt pages.

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
`Frame Type` must split into the modern `FrameTypeConfigNew` route and a
separate `Frame Type (Legacy)` route backed by `FrameTypeConfigOld`; the current
combined factory is neither MP10 page. The current compass page should be
called `Compass (Legacy)` because it lacks current multi-compass
discovery/priority. Range Finder supports one old `RNGFND_*` instance, Airspeed
exposes only the old enable/use/pin choices, Optical Flow is only a
`FLOW_ENABLE` checkbox, and Camera Gimbal is the old single `MNT_*` surface.
Those four pages use parameter families that are obsolete on many modern 4.x
vehicles, so they should be explicitly labelled `(Legacy)` and shown only when
their legacy parameters exist. Existing Joystick and Compass/Motor code may
seed the missing direct routes after ownership and lifecycle review.

Retention does not permit a misleading replacement. A legacy module may be an
additional clearly named route, or a temporary partial implementation of the
same workflow, but it must not perform an unrelated operation under an MP10
name. This is why the CONFIG `Onboard OSD` route now uses a separate phase-one
layout editor and never the unrelated legacy stream-rate helper, while useful
legacy SETUP hardware pages remain available.

## Ordering and visibility gaps

Qt preserves the three broad groups but not the complete MP10 order. Among
pages common to both applications, the active inversion is `DroneCAN/UAVCAN`:
MP10 places it immediately after `Battery Monitor 2`, while Qt places it after
both Antenna Tracker pages. The tracker pages correctly precede `HW CAN` in
both. Qt also lacks the shared MP10 `DisplayView` profile service, so its
visibility is based mainly on
connection state, coarse firmware family and global advanced mode. The exact
MP10 per-feature gates are therefore not yet reproduced.

There are no null factories in active SETUP, but a concrete factory can still
look empty or be misleading. `FrameTypeConfig` creates its inner widget only
after both a valid firmware version and a later parameter callback; cached
parameters arriving first can leave its scroll area blank indefinitely. A
later vehicle/firmware switch can also leave the previously created inner
widget visible for the wrong profile. `Optical Flow` is non-empty but
near-placeholder functionality. These are the first production navigation
cases the SETUP click/reset test must expose.

The required navigation regression test must lock:

- the 38 Qt page IDs and three group IDs in production order;
- a non-null concrete widget after activating every visible page;
- offline, connected, partial-parameter and advanced-mode visibility;
- Copter, Plane, Rover, Heli and tracker profile transitions;
- fallback selection after the current page is hidden or reset.

New MP10 routes should be added only as complete vertical slices. Useful
legacy pages and `QML Plugins` stay registered and are classified separately
instead of being removed to make the raw counts look closer.
