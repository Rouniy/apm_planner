# SETUP inventory audit

Updated: 2026-09-05.

This audit compares the active Mission Planner 10
`ViewModels/SetupViewModel.cs` navigation with the active Qt
`SetupView::buildPages()` surface. It counts registered/potential navigation
entries, not support classes in the source tree; connection, profile and
advanced-mode gates mean they are not all visible simultaneously.

## Exact counts

Both applications use the same three named collapsible groups: Mandatory
Hardware, Optional Hardware, and Advanced. MP10 currently renders a static
literal `>>` in each group header even when expanded. By explicit user request,
Qt preserves the paired-arrow affordance but makes its state truthful: `<<`
means expanded and `>>` means collapsed. Including the ungrouped pages above
the groups, the navigation has four logical sections.

| Logical section | MP10 pages | Qt pages | MP10 group heading | Qt group heading |
|---|---:|---:|---:|---:|
| Ungrouped | 4 | 1 | 0 | 0 |
| Mandatory Hardware | 16 | 16 | 1 | 1 |
| Optional Hardware | 26 | 21 | 1 | 1 |
| Advanced | 7 | 6 | 1 | 1 |
| **Total** | **53** | **44** | **3** | **3** |

Thus MP10 registers 56 potential navigation entries when group headings are
included. Qt registers 47: 44 page factories plus the same three group
headings. Every current Qt page registration has a concrete factory. Counts
alone do not imply parity because several factories still wrap legacy APM
Planner widgets rather than the corresponding MP10 implementation.

## Missing MP10 pages

The 2026-09-04 baseline recorded 11 direct MP10 gaps. This ledger was already
present in this audit; these were not newly discovered pages. The operational
problem was that the production test gate copied Qt's 43-route list instead of
enforcing this known independent reference ledger.

The new Joystick route closes one baseline gap through a deliberately labelled
partial reuse of the legacy production workflow. Its non-empty SETUP page
launches MainWindow's shared `actionJoystickSettings`, so the application keeps
one `JoystickInput` and reuses one modeless `JoystickWidget` instead of creating
a duplicate live controller. The current missing count is therefore 10:

- Ungrouped: `Install Firmware Legacy`, `Secure`, and
  `Secure (Bootloader Keys)`.
- Optional Hardware: `CubeID Update`, `NV Modem`, `PX4Flow`, `Antenna Tracker`,
  and `FFT Setup`.
- Advanced: `Onboard Lua REPL` and `Local Script REPL`.

Qt also has one intentional additional page, `QML Plugins`. It is the useful
user-facing manager for the fresh port's trusted QML extension system and must
remain. The current arithmetic is therefore `53 - 10 + 1 = 44` Qt pages. The
machine-readable `SETUP_REFERENCE_INVENTORY.tsv` retains all 53 MP10 rows,
marks all 11 baseline gaps, records Joystick as the one gap closed in this
slice, and lists the Qt-only QML page separately.

## Retained useful legacy modules

Existing APM Planner modules may remain when they provide a real and useful
workflow. Install Firmware, Accel Calibration, Radio Calibration, FailSafe,
SiK Radio, Battery Monitor and Terminal are substantive same-domain
implementations worth retaining as explicitly `partial`. The vehicle pages
derived from `AP2ConfigWidget` still bind the global active UAS/parameter
manager and often force component 1, so none should be treated as exact-target
safe until its target lease, component filtering and ACK lifecycle are ported.

`Flight Modes` no longer belongs to that legacy set. SETUP and CONFIG now use
the same native six-row widget, immutable target lease, exact-link heartbeat
and RC monitor, and one changed-only parameter batch. Copter always exposes
both Simple columns, disabling one only when its parameter is absent. Plane
and Rover keep their sixth slot, Rover uses `MODE_CH`, and PX4 uses the
`COM_FLTMODE_CH` channel plus the exact 0..12 slot enum separately from packed
heartbeat custom modes. The legacy source remains compiled but is not
instantiated by either active route.

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
can accept a failed result. The separate native `Compass` route now supplies
the current parameter/discovery/priority workflow with exact-target writes plus
an application-owned pinned-target onboard and Large Vehicle calibration
service. Start/Accept/Cancel/fixed-yaw are ACK-gated, multi-compass completion
waits for the stable reported mask, and ambiguous command outcomes cannot be
retried on the same generation. `Calibrate from Log` alone stays explicitly
disabled until the separate OfflineMagFit workflow is ported. The dedicated
`Compass/Motor Calib` route now replaces the disabled legacy dialog with an
application-owned exact-target state machine. It matches the MP10 start and
two-frame Finish wire protocol, keeps Finish reachable while firmware has the
motor outputs armed, filters values and logs by pinned physical link and
endpoint, and adds ordered point-to-point transport, single-autopilot,
disarmed, vehicle-family and default-Cancel propeller safety gates. Listening
UDP, UDP client, simulation and unknown transports are rejected; MAVLink 1 is
supported on a dedicated Serial/TCP link, with terminal firmware text required
because its ACK has no target extensions. Range Finder
supports one old `RNGFND_*` instance, Airspeed
exposes only the old enable/use/pin choices, Optical Flow is only a
`FLOW_ENABLE` checkbox, and Camera Gimbal is the old single `MNT_*` surface.
Those four pages use parameter families that are obsolete on many modern 4.x
vehicles, so they should be explicitly labelled `(Legacy)` and shown only when
their legacy parameters exist. Joystick is now an honest partial route: its
launcher delegates to the already-wired MainWindow action and the retained
legacy 624x448 modeless dialog, preserving the legacy settings and live-input
lifecycle without constructing a second controller. A native embedded MP10
Joystick page remains future work. The old CompassMot dialog remains inactive
source and is not a safe substitute for the native page.

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
`Default Settings` sequence. The current `Compass` route follows Accel
Calibration and precedes the retained `Compass (Legacy)` route, matching MP10.
The shared native `Flight Modes` page remains after `ESC Calibration` and
before `FailSafe`, also matching MP10 without changing the page/section count.
The helicopter page's five sections retain the exact
8x5 servo, 13 swashplate, 12 rotor-speed, nine governor and nine miscellaneous
bindings. The route deliberately checks the current `H_SW_TYPE` capability;
MP10's `IsHeli()` checks the obsolete `H_SWASH_TYPE` even for this 4.0+ page,
which would hide it from current-only vehicles and collide with the separate
legacy CONFIG editor.

Among pages common to both applications, `DroneCAN/UAVCAN` now follows
`Battery Monitor 2`, the partial `Joystick` route follows DroneCAN, and the new
`Compass/Motor Calib` route follows Joystick and precedes `Range Finder`; this
preserves their MP10 relative order. The tracker pages correctly
precede `HW CAN` in both. Qt now has one shared JSON
`DisplayViewProfileService`. It preserves all 35 MP10 SETUP feature flags; 32
currently gate corresponding existing factories, including the newly active
`displayJoystick`, `displayCompassMotorCalib`, the independent CONFIG
`displayOSD` versus SETUP `displayOsd` distinction, CAN, tracker and Terminal
flags. The three dormant flags belong to the missing FFT, PX4Flow and two REPL
routes. Active gates compose with connection requirements; they never make a
missing page appear.
Useful Qt-only `QML Plugins` and Advanced utilities without a reference feature
flag remain. `setup_lastpage` is persisted and restored.

Three non-reference visibility filters have been removed: Serial Ports no
longer requires a recognized firmware family, Initial Parameters is no longer
limited to Copter/Plane, and Motor Test is no longer hidden solely for ArduSub.
They now follow MP10's connection plus profile rules. This visibility parity
does not relax the factories' exact-target, armed-state, acknowledgement or
transport safety requirements; an exposed route is not permission to issue an
unsafe command. The modern Heli `H_SW_TYPE` capability correction and the
Compass/Motor critical-lifetime extension remain documented deliberate
deviations. The profile service writes deterministic JSON, preserves unknown
Custom fields and omits legacy XML/`advancedview` migration for this fresh
port.

There are no null factories in active SETUP, but a concrete factory can still
be misleading. The native Frame pages build their complete unavailable state
before any firmware or parameter callback and therefore replace the combined
`FrameTypeConfig` blank/stale lifecycle on the active route. `Optical Flow` is
non-empty but near-placeholder functionality and is the reset/recreate probe
in the production audit.

`setupviewroutes_tests` constructs the real hidden production `SetupView` in
an isolated BUILD_TESTING process. Its oracle is now an independently
transcribed 53-page MP10 manifest rather than a list copied from the current Qt
registry. Every reference route has either one explicit Qt mapping or membership
in the reviewed 10-route missing allowlist; the original 11-gap baseline and
the Joystick closure are also asserted. The useful Qt-only `QML Plugins` route
is an explicit extension. The audit then locks the resulting 44 page IDs and
three group IDs in production order, invokes every real factory through its
navigation button, checks current/checked/stack ownership and semantic
non-empty content, and resets/recreates Optical Flow. Removing a mapped route,
adding an unreviewed route, or silently omitting a new MP10 ledger entry can no
longer pass through the former 43-route self-oracle.

The action-registration regression is covered too: the audit first creates the
Joystick, Advanced Tools and Developer Tools pages before their shared actions,
then calls the production action-ready hook, proves the stale pages were
destroyed, recreates them, and proves the Joystick launcher triggers the shared
action. Backstage lifecycle signals remain blocked, so the audit does not start
page network or hardware operations. The test also covers Setup teardown
ordering: all created pages are reset while the derived services, leases and
weak owners are still alive.

Real-X11 verification on 2026-09-05 exercised the production navigation. The
expanded and collapsed Setup states show `<<` and `>>` respectively, the
Joystick route is non-empty, and `Open Joystick Settings` opens the actual
legacy 624x448 modeless dialog. No joystick hardware was attached, so physical
axis input, mappings, activation and vehicle control remain unverified. Evidence
is `/tmp/apm-graph-speech-setup.irm6Qx/setup-expanded.png`,
`setup-collapsed.png`, `joystick-route.png`, and `joystick-dialog.png`. The full
200-test suite passed, and the normal application smoke exited with status 0.

Still required as a separate visibility-matrix regression:

- offline, connected, partial-parameter and advanced-mode visibility;
- Copter, Plane, Rover, Heli and tracker profile transitions;
- fallback selection when profile/vehicle gates hide the current page.

New MP10 routes should be added only as complete vertical slices. Useful
legacy pages and `QML Plugins` stay registered and are classified separately
instead of being removed to make the raw counts look closer.
