# Mission Planner 10 Antenna Tracker SETUP audit

Updated: 2026-09-02. This is a read-only implementation handoff for the three
Mission Planner 10 Antenna Tracker routes. It records the next functional
slices; it is not evidence that the routes are implemented.

## Current Qt state

The active `SetupView` does not register any Antenna Tracker route. The old
`AntennaTrackerConfig` is a UI-only stub instantiated by the unused
`ApmHardwareConfig`, whose tracker buttons are hidden. Do not extend that stub
as the new architecture. Replace it after the new routes are usable.

## Required routes and visibility

Mission Planner 10 registers three independent, non-advanced sub-pages behind
the `displayAntennaTracker` profile flag:

1. `Antenna Tracker` — `ConfigAntennaTrackerParamView` and
   `ConfigAntennaTrackerParamViewModel`, immediately after `ESP8266 Setup`,
   requires a connected vehicle.
2. `Antenna Tracker (Serial)` — `ConfigAntennaTrackerView` and
   `ConfigAntennaTrackerViewModel`, immediately after `FFT Setup`, works
   offline against a dedicated serial port.
3. `Antenna Tracker (Live)` — `AntennaTrackerUIView` and
   `AntennaTrackerUIViewModel`, immediately after the serial page, works
   offline against a dedicated serial port while consuming vehicle telemetry
   when available.

The parameter page remains listed for a connected non-Tracker vehicle but its
content is disabled with: `This page requires an AntennaTracker (ArduTracker)
firmware connection.` The Qt port has no DisplayView profile service yet;
route gating must remain an explicit deviation until that service exists.

## Serial and live surfaces

Both serial views share these controls and defaults:

- Interface: `Maestro`, `ArduTracker`, `DegreeTracker`; default `Maestro`.
- Serial port and baud (`4800`, `9600`, `14400`, `19200`, `28800`, `38400`,
  `57600`, `115200`); default `9600`.
- `Connect/Disconnect`, `Find Trim Pan (Sik Radio)`, servo-damage warning and
  status.
- Pan range/angle `360`, PWM range `1000`, center PWM `1500`, speed `100`,
  acceleration `5`, trim `0` in `-180..180`, reverse off.
- Tilt range/angle `90`, PWM range `1000`, center PWM `1500`, speed `100`,
  acceleration `5`, trim `0` in `-range/2..+range/2`, reverse off.

Speed and acceleration are editable only for Maestro and only while
disconnected. Connection locks the interface, port, baud and numeric setup;
trim/reverse remain live. The serial page also points at the vehicle at 10 Hz.

The live page additionally shows vehicle and commanded azimuth/elevation,
manual-slew override, `Home / Center`, and manual azimuth/elevation sliders
(`-180..180`, `-90..90`). `Home / Center` resets manual angles; in MP10 it does
not enable manual mode, so the next automatic tick can overwrite the command.
The Qt port should either preserve and document that behavior or deliberately
fix it in `PORTING_DEVIATIONS.tsv`.

### Output protocols

- Maestro uses channel 0 for pan and 1 for tilt; `SetTarget` `0x84`,
  `SetSpeed` `0x87`, `SetAcceleration` `0x89`, two 7-bit data bytes, and target
  PWM multiplied by four. Clamp PWM to `center +/- PWMRange/2`. Preserve the
  MP flip when tilt range exceeds 120 degrees and wrapped pan exceeds 90
  degrees.
- ArduTracker sends calculated PWM as `!!!PAN:1750,TLT:1777\n`-style text.
- DegreeTracker sends tenths of degrees truncated toward zero as
  `!!!PAN:0123,TLT:-0056\n`-style text. PWM/reverse settings do not affect it.

All use a dedicated raw `QSerialPort` (8N1, no flow control), not a MAVLink
`LinkManager` link. Reject ports already owned by a main MAVLink connection.
The MP10 SiK trim search takes about 68 seconds and is neither cancellable nor
isolated from the 10 Hz loop; the Qt implementation must make it cancellable,
serialize it with normal output and stop it during navigation/shutdown.

## Tracker parameter page

The page contains 25 parameter fields:

- choices: `AHRS_ORIENTATION`, `SERVO_YAW_TYPE`, `SERVO_PITCH_TYPE`,
  `ALT_SOURCE`;
- yaw: `RC1_MIN`, `RC1_MAX`, `RC1_TRIM` (`900..2200`, step 1), `RC1_REV`;
- pitch: `RC2_MIN`, `RC2_MAX`, `RC2_TRIM` (`900..2200`, step 1), `RC2_REV`;
- ranges: `YAW_RANGE` (`0..360`), `PITCH_MIN`, `PITCH_MAX` (`-90..90`);
- yaw PID: `YAW2SRV_P/I/D` (`0..100`, step 0.1), `YAW2SRV_IMAX`
  (step 1), `YAW_SLEW_TIME` (step 0.1);
- pitch PID: `PITCH2SRV_P/I/D`, `PITCH2SRV_IMAX`, `PITCH_SLEW_TIME` with the
  same ranges/steps.

Actions are `Refresh Params`, explicit staged `Write PIDS`, `Test Yaw`, `Test
Pitch` and two `0..100` sliders. Show parameter status, aggregate write status
and live `SERVO_OUTPUT_RAW` channels 1/2, coalesced to 200 ms. Servo tests use
exact-target `COMMAND_LONG / MAV_CMD_DO_SET_SERVO (183)`, parameter 1 = servo
1 or 2, parameter 2 = calculated PWM. Serialize yaw/pitch tests because the
current `VehicleCommandService` tracks only one pending ACK per command ID.

Do not copy MP10's reverse bug: `RC1_REV` is `checked=1`, `unchecked=-1`;
`RC2_REV` is `checked=-1`, `unchecked=1`. Use staged editing and one explicit
batch commit instead of MP10's immediate write followed by a duplicate
`Write PIDS` write. Record this intentional behavior correction.

## Reusable Qt foundations

- `QSerialPort`, `QSerialPortInfo` and cancellation/ownership patterns from
  `ConfigHWBTSerialService`.
- `ParameterFirmwareFamily::AntennaTracker`, `ParameterMetaDataRepository`,
  `ParamField`, `QGCUASParamManager`/`ParameterService` and
  `VehicleCommandService`.
- `UASInterface::servoOutputChanged`, `globalPositionChanged` and
  `UASManager::homePositionChanged`.
- `LinkManager::messageReceived` for `RADIO`/`RADIO_STATUS`, after adding a
  structured SNR consumer rather than scraping logs.

The packaged resources currently lack `antennatracker.pdef.xml`; add/update
the metadata package before relying on enum choices. Never silently treat the
historical default home coordinates as the physical tracker location. Add an
explicit tracker-home state and later share it with the PLAN action.

## Recommended slices and tests

1. Pure `AntennaTrackerOutputs` codecs/factory with injectable writer and
   golden-byte tests for all three protocols, clamp, reverse and flip.
2. `AntennaTrackerSerialService` with one owning thread, latest-target-wins
   queue, port-collision checks and deterministic cancellation/destruction.
3. Shared serial/live view model plus the two MP10-named views, settings,
   manual/automatic state and lifecycle tests.
4. Pure `AntennaTrackerGeometry` with north/east/altitude, zero-distance and
   dateline tests, then live telemetry binding.
5. `ConfigAntennaTrackerParamViewModel/View` with all fields, custom reverse
   encoding, staged batch write, serialized servo tests and exact-target tests.
6. Route/profile/firmware gating, metadata packaging, real-X11 and active-loop
   shutdown smoke; only then remove the old stub and wiring.
