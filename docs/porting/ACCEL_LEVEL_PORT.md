# SETUP Calibrate Level

The MP10 page contains Full accelerometer calibration, Calibrate Level and
Simple Accel Cal. The old Qt page had Full, Simple and a useful Legacy option,
but lacked the separate one-axis/AHRS-trim action. This slice adds Level in
its reference position, preserving the other modes.

Reference: `GCSViews/ConfigurationView/ConfigAccelCalibrationView.axaml` and
`ViewModels/GCSViews/ConfigurationView/ConfigCalibrationPages.cs:147` in the
local Mission Planner 10 tree. Level sends `COMMAND_LONG`, command241
(`MAV_CMD_PREFLIGHT_CALIBRATION`), confirmation0, parameters
`[0,0,0,0,2,0,0]`. MP10 displays Completed only after an accepted command ACK.
The older Qt `ApmPlaneLevel` gyro/baro command is not an equivalent action.

## Integration

`SetupView` injects the application-owned `DeveloperVehicleToolService` into
the retained `AccelCalibrationConfig`. An appended `CalibrateLevel` action
uses its existing immutable prepare/validate/execute and exact ACK owner.
It does not add another entry to the32-action Developer Tools inventory.

The Level button opens a nonblocking confirmation identifying the target and
asking the user to place the autopilot flat and level. Cancel is the default
and Escape action. Full/Simple starts cannot overlap a Level confirmation or
active Level command; existing calibration interaction prevents starting Level.
Accepted ACK, rejection and uncertain outcome are distinct; sending alone
does not mean Completed. This only proves firmware acceptance, not that the
physical mounting was actually level.

Simple now shares that one-shot owner with its unchanged reference p5=4.
Previously it set the legacy `isInCalibration` flag without clearing it from
an ACK, so the next Full click could send a position acknowledgement instead
of starting and Level could stay blocked. Both one-shot actions now return
controls on their own terminal result, without muting audio or changing link
timeouts. The ACK deadline is25 seconds; automatic retry is disabled because a
lost success ACK followed by an immediate duplicate can report calibration busy.

## Deliberate remaining differences

- Level requires one fresh disarmed ArduPilot target and the established exact
  single-vehicle route policy, unlike MP10's simple connection-only gate.
  PX4 and other autopilots are not claimed by this implementation.
- Full/Legacy remain the useful existing APM interactive workflows. This
  slice does not claim their complete MP10/exact-target or hardware parity.
- Unlike MP10's single retry, Level/Simple do not automatically repeat an
  uncertain calibration. The UI cannot prove physical leveling from an ACK;
  AHRS trim parameter-cache refresh remains a separate improvement.
- There is no temperature action in the actual MP10 page; the old inventory's
  generic temperature acceptance text was inaccurate.
- Physical hardware, reference screenshot comparison and Windows/macOS/HiDPI
  verification remain separate gates.

## Evidence

Service tests include the exact seven parameters/target, accepted and rejected
ACKs, and the armed zero-transmission gate. The existing production runtime
audit now opens the actual SETUP page and exercises Cancel and Accept, checks
the captured command, Completed state and restored Full/Simple controls.
Its vehicle is an in-process fixture, not the user's network SITL.

Build/test/native-window results are recorded in `CURRENT_STATE.md`.
Evidence directory: `/tmp/apm-accel-level.NVpAxK/`.

Final Qt5/audio build1, focused4/4 (35.69s), full313/313 (55.24s) and actual
production X11 (zero audit failures/exit0) pass. Root inspected the page and
default-Cancel confirmation screenshots. Claude TCP c346/c348 reviewed the
normal reference workflow and integration without building or changing files.
