# SETUP Joystick

The user-directed priority is practical single-vehicle SETUP functionality.
This slice replaces the retained legacy launcher with `ConfigJoystickView`.
It does not start another log-export or Swarm workstream.

## Implemented workflow

- Optional Hardware → Joystick opens a real embedded page, including offline.
  The DATA Joystick action and existing application action select this page.
- SDL2 supplies device selection, refresh/hot-plug, raw input, arbitrary axes,
  buttons and hats. Qt owns the windows; SDL background input stays enabled.
- Sixteen RC rows expose axis, value, expo, reverse and movement detection.
  Button rows expose physical index, press detection, function and settings.
  Elevons, MANUAL_CONTROL, range calibration and reset are available.
- Save persists mappings/calibration locally; Import/Export uses MP10 ZIP/XML
  `.joycfg`, not JSON with a renamed extension. The optional Qt JSON member
  retains device identity and calibration. Unrepresentable SDL mappings are
  refused for portable export with an explicit channel list; local Save keeps
  them. Import binds the profile to the currently selected controller.
- Explicit Enable consent identifies one controller and one physical vehicle
  session; Cancel is the default. The application owns the 20 Hz sender, so
  leaving SETUP does not stop an enabled controller. Nothing auto-enables on
  load, reconnect or device insertion.
- RC override uses the selected vehicle's available RCn_MIN/MAX/TRIM, falling
  back to 1000/2000/1500. MANUAL_CONTROL uses the first four mapped channels.
  Device loss, target/session change or stale heartbeat stops output. Disable
  attempts release only on the original route; extended mapped channels use
  MAVLink's release value rather than its ignore value.
- Main button workflows use the existing exact-target command service: flight
  mode, arm/disarm, relay, servo/repeat, camera trigger, mount mode/centre and
  Guided → TakeOff. Custom button axes and incremental reference hat axes are
  supported. The old SDL thread is no longer constructed by MainWindow.

## Explicit remaining differences

- `Toggle_Pan_Stab` and `Gimbal_pnt_track` require the remaining mount parameter
  and live pointing workflows. Their choices are unavailable, not inert
  actions advertised as implemented.
- One local profile is stored. Firmware-specific XML imports retain their
  firmware suffix, but a newly created profile is generic and there is not yet
  automatic per-firmware profile selection. Do not claim universal MP10
  Copter/Plane/Rover profile interchange from the generic export alone.
- Flight mode settings currently accept a name, not a firmware-filtered combo.
  Vehicle-family validation occurs before command submission.
- Configuration editing requires Disable, unlike MP10's live remapping. This
  preserves the device/channel/button configuration reviewed at Enable.
- Stop submits one guarded mapped-channel RC release, not MP10's nine repeated
  all-zero frames. Submission is not an acknowledgement of flight-controller
  acceptance; firmware RC override timeout/failsafe behavior still matters.
- The Qt transport is MAVLink. MP10's built-in SITL raw UDP5501 shortcut is not
  reproduced. Ordinary network SITL can use its MAVLink connection.
- Mount mode uses `DO_MOUNT_CONFIGURE`, while MP10 writes `MNT_MODE`; modern
  firmware compatibility and physical controllers still need field evidence.
- SDL2 is the cross-platform input implementation. Physical joystick, native
  Windows/macOS, HiDPI and reference screenshot parity are not yet verified.

## Verification

Root schedules all builds/tests with the repository's single-build lease and
12-job cap. Evidence for this slice is `/tmp/apm-joystick-port.Ogdqvl/`.
The focused suites cover profiles, SDL virtual input/calibration/unplug, exact
control and the page. The production `--joystick-audit` drives the actual
SETUP selector, mappings, Save, Enable Cancel/Accept, output, navigation and
Disable using an SDL virtual joystick and an in-process vehicle link only.
It opens no socket to the user's SITL and sends no commands to that vehicle.

Final build/test and native-window results are recorded in `CURRENT_STATE.md`.
