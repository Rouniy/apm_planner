# Warning Manager exact telemetry catalog

Reference: Mission Planner 10 `ExtLibs/ArduPilot/CurrentState.cs` and
`ExtLibs/Utilities/Warnings/CustomWarning.cs`, inspected 2026-09-05.

## Coverage accounting

The reference contains **486 top-level public numeric/boolean properties**.
This count excludes fields (for example `hilch1`), methods, nested sensor-class
properties, arrays, strings, dates and object-valued properties. `GetOptions()`
actually offers every non-null successfully evaluated public property without
filtering its type; `GetValue` later converts the result to double. Consequently
the reference dropdown also offers values that cannot meaningfully be compared.
The native catalog offers numerical/boolean fields only and does not fabricate
values for unsupported reference names.

The source statically maps **394 of those 486 names**, with **92 explicitly absent**.
This is not a claim that all original data producers, aggregation or display-unit
semantics are ported. The complete name inventories below make omissions visible.
`WarningTelemetrySourceTest::catalogAndEmptyState` reports the executable count;
the root-owned full build/test and production evidence are recorded in the slice
handoff, not inferred from this document.

## Identity, freshness and ownership

One source follows exactly `(physical link, system, component, target generation)`.
Both generation-changed and generation-settled invalidate previous samples.
Its separate monotonically increasing source epoch also invalidates all samples
and the retained home reference on a physical-link/replay ingress boundary.
The owner must call `invalidateSourceEpoch()` for matching
`LinkManager::physicalLinkSessionBegan/Ended`, then forward only frames from the
current ingress epoch using `mavlinkMessageObserved`. Do not additionally forward
the system-ID-only legacy UAS graph or duplicate `messageReceived` stream.

Every streaming field expires independently when older than **5000 ms**.
Unrelated messages do not refresh it. Missing/invalid/NaN/Inf/sentinel values are
absent from `values()`, never false zeros; a newly invalid field deletes its
previous sample immediately. Derived fields require all fresh inputs.
An unknown/dropped target yields an empty map, including boolean fields; it does
not become a fictitious disarmed vehicle. `connected` is only supplied by fresh
heartbeat. Same sysid on a different link or component cannot update any field.
Injected clocks are copied before invocation, guarded for source deletion and
checked for epoch changes after reentrant callbacks. Backward and extreme
signed clock values cannot extend freshness or overflow its age calculation.

HOME_POSITION is a reference received potentially once per session and remains
valid until its exact source epoch ends or an invalid replacement arrives.
`HomeAlt` can therefore remain available without streaming packets;
`DistToHome` always requires fresh lat/lng. No home position is guessed.
Distance uses a great-circle calculation (Earth radius 6371000 m) and handles
the equator and dateline; MP10 uses a flat TrackerLocation projection with
zero-coordinate exclusions. Operator-set tracker home is not yet an input.

## Sources and units

Canonical fields use **m, m/s and degrees**, regardless of display preferences.
Changing CONFIG units must not silently change a saved warning's physical trigger.
Imported MP10 XML thresholds created with non-SI display units need operator
review; they cannot be converted without knowing the originating preferences.
`fieldUnits()` supplies the actual comparison unit next to a selector/value.

| Producer | MP10 fields and retained units |
|---|---|
| ATTITUDE / AOA_SSA | roll, pitch, yaw (negative primary yaw normalized into 0..360), AOA, SSA; degrees |
| GLOBAL_POSITION_INT / GPS_RAW_INT / GPS2_RAW | lat/lng and GPS2 coordinates in degrees; altitude m; velocities m/s; HDOP divided by 100; satellite/fix counts; accuracy m/m/s/deg; yaw centidegrees to degrees |
| VFR_HUD / NAV_CONTROLLER_OUTPUT / MISSION_CURRENT | air/ground speed and climb m/s, throttle percent, nav angles degrees, distances m, MP10 aspd_error divided by 100, waypoint number |
| SYS_STATUS / BATTERY2 / BATTERY_STATUS ids 0..8 | voltage V, current A (signed charging allowed; -1 wire sentinel absent), remaining percent, consumption mAh, temperature °C, remaining minutes; primary cells 1..14 V; watts from fresh primary V and A |
| RC_CHANNELS / RC_CHANNELS_RAW | ch1in..ch16in PWM microseconds, explicit channel count respected; RAW only port 0 channels 1..8; RSSI integer percent using MP10 denominators 254 / 255 |
| SERVO_OUTPUT_RAW | port 0 ch1out..ch16out; port 1 ch17out..ch32out; PWM microseconds; zero output unavailable; MAVLink 1 extension channels absent |
| RAW_IMU / SCALED_IMU / SCALED_IMU2 / SCALED_IMU3 | ax/ay/az in mg, gx/gy/gz in mrad/s, mx/my/mz in mG; primary/2/3 suffixes; temperatures °C; magnitude accelsq in g, gyrosq/magfield in original vector units; RAW_IMU nonzero id not merged into primary |
| SCALED_PRESSURE / SCALED_PRESSURE2 | press_abs in hPa; **press_temp and airspeed*_temp retain reference raw centidegrees Celsius**, not its misleading unscaled temperature label |
| HWSTATUS / POWER_STATUS / MEMINFO / MCU_STATUS | hwvoltage and MCU volts V; **boardvoltage/servovoltage remain raw mV as actually stored by MP10**; MCU temperature °C; bytes/errors/bitmasks; freemem32 preferred if nonzero |
| EKF_STATUS_REPORT / VIBRATION | five variance values, flags, reference aggregate failure rule, vibration m/s², three clipping counts |
| RANGEFINDER / DISTANCE_SENSOR | sonarrange m and sonarvoltage V; explicit sensor ids 0..9 -> rangefinder1..10 **cm** |
| OPTICAL_FLOW / TERRAIN_REPORT / WIND / RPM | flow raw pixel and compensated m/s, quality; terrain heights m, loaded/pending counts and spacing m; wind degrees/m/s; RPM |
| AHRS2 / LOCAL_POSITION_NED / FENCE_STATUS | secondary attitude degrees and altitude m, coordinates degrees; local NED position m; fence count/status/type |
| PID_TUNING | FF/P/I/D, axis, desired/achieved and SRate/PDmod; controller-axis-dependent units |
| ESC_TELEMETRY_1_TO_4 through 13_TO_16 | 16 sets of voltage V/current A/RPM/temperature °C |
| RADIO / RADIO_STATUS | raw RSSI/noise units, txbuffer percent, receive/correction counts |
| GENERATOR_STATUS / EFI_STATUS / HYGROMETER_SENSOR | generator flag/speed/current/voltage/runtime/maintenance, primary ECU temperatures/load/fuel/pressure, hygrometer ids 0/1 raw centidegrees/centipercent |
| HEARTBEAT / EXTENDED_SYS_STATE | armed, failsafe (CRITICAL), landed (STANDBY), connected; explicit VTOL/landed enum values |
| SYS_STATUS masks | prearmstatus, safetyactive, terrainactive; reverse-throttle sign supplied only while its status sample is fresh |

A generic bounded metadata reader handles scalar and array layouts using the
vendored C dialect. It copies only `message.len` payload bytes and zero-fills
trimmed MAVLink 2 scalar tails, ignoring leftover encoder checksum bytes.
MAVLink 1 extension fields are genuinely absent. This is a parser-validated
ingress consumer, not a replacement CRC/signature parser.

## Known behavioral gaps and intentional differences

- MP10's battery voltage 0.4/0.6 smoothing and integrated-current fallbacks are
  not applied: warnings compare fresh direct sensor values. Current consumption
  is taken from BATTERY_STATUS rather than guessed by integration.
- GPS coordinates/speed/course require a reported fix >= 2. Global fused
  position takes precedence over raw GPS only while its position is fresh.
  Unknown GPS accuracy/yaw, battery time, RC RSSI and other sentinels are absent,
  replacing the reference's stale/default-zero behavior. Legitimate zero
  geographic coordinates are supported.
- Relative altitude is the GLOBAL_POSITION_INT relative_alt in metres.
  Operator `altoffsethome` is not applied. Verticalspeed requires two relative
  altitude samples at least 200 ms apart, expires independently and uses 0.4/0.6
  smoothing after the first real rate. It does not initialize from a fictitious
  zero altitude/rate or retain an unchanged old rate indefinitely.
- The downward DISTANCE_SENSOR selection/fallback into sonarrange is not
  implemented; direct RANGEFINDER and all ten explicit distance sensors work.
  Sensor min/max/error-state semantics beyond UINT16_MAX still need a broader
  distance-source review.
- RADIO messages often come from modem component 68/system 51 rather than the
  selected autopilot. Strict exact-component filtering deliberately leaves those
  fields unavailable in that configuration; a future explicit trusted radio
  source policy must preserve physical-link/epoch identity instead of widening
  this source to any same-link component.
- HIGH_LATENCY, HIGH_LATENCY2, HIL, HIGHRES_IMU, custom-named telemetry and newer
  AIRSPEED message producer fallbacks are not mapped. The original sources that
  do exist are listed above; a field name in the catalog is not proof that every
  MP10 producer of that property is covered.
- PID axis-specific rate history, param-dependent throttle/airspeed/flight-time,
  magnetics/airspeed estimators, all original home/tracker/mission geometry,
  transponder state, telemetry rates/link-loss statistics and user custom fields
  remain absent below. EFI fuel_pressure is missing from the vendored dialect.
- Numeric warnings carry doubles like the reference. Very large 64-bit status
  bitmasks are not suitable for exact bitwise matching; bitmask comparisons remain
  numeric rather than a new bit-test operator.

## Complete mapped names (394)

```text
AOA DistToHome HomeAlt SSA accelsq accelsq2 accelsq3 ahrs2_alt ahrs2_lat ahrs2_lng ahrs2_pitch
ahrs2_roll ahrs2_yaw airspeed airspeed1_temp airspeed2_temp alt alt_error altasl altasl2 armed
aspd_error asratio ax ax2 ax3 ay ay2 ay3 az az2 az3 battery_cell1 battery_cell10 battery_cell11
battery_cell12 battery_cell13 battery_cell14 battery_cell2 battery_cell3 battery_cell4 battery_cell5
battery_cell6 battery_cell7 battery_cell8 battery_cell9 battery_remaining battery_remaining2
battery_remaining3 battery_remaining4 battery_remaining5 battery_remaining6 battery_remaining7
battery_remaining8 battery_remaining9 battery_remainmin battery_remainmin2 battery_remainmin3
battery_remainmin4 battery_remainmin5 battery_remainmin6 battery_remainmin7 battery_remainmin8
battery_remainmin9 battery_temp battery_temp2 battery_temp3 battery_temp4 battery_temp5
battery_temp6 battery_temp7 battery_temp8 battery_temp9 battery_usedmah battery_usedmah2
battery_usedmah3 battery_usedmah4 battery_usedmah5 battery_usedmah6 battery_usedmah7
battery_usedmah8 battery_usedmah9 battery_voltage battery_voltage2 battery_voltage3 battery_voltage4
battery_voltage5 battery_voltage6 battery_voltage7 battery_voltage8 battery_voltage9 ber_error
boardvoltage brklevel ch10in ch10out ch11in ch11out ch12in ch12out ch13in ch13out ch14in ch14out
ch15in ch15out ch16in ch16out ch17out ch18out ch19out ch1in ch1out ch20out ch21out ch22out ch23out
ch24out ch25out ch26out ch27out ch28out ch29out ch2in ch2out ch30out ch31out ch32out ch3in ch3out
ch3percent ch4in ch4out ch5in ch5out ch6in ch6out ch7in ch7out ch8in ch8out ch9in ch9out climbrate
connected current current2 current3 current4 current5 current6 current7 current8 current9 efi_baro
efi_exhasttemp efi_fuelconsumed efi_fuelflow efi_headtemp efi_health efi_intaketemp efi_load efi_rpm
ekfcompv ekfflags ekfposhor ekfposvert ekfstatus ekfteralt ekfvelv errors_count1 errors_count2
errors_count3 errors_count4 esc10_curr esc10_rpm esc10_temp esc10_volt esc11_curr esc11_rpm
esc11_temp esc11_volt esc12_curr esc12_rpm esc12_temp esc12_volt esc13_curr esc13_rpm esc13_temp
esc13_volt esc14_curr esc14_rpm esc14_temp esc14_volt esc15_curr esc15_rpm esc15_temp esc15_volt
esc16_curr esc16_rpm esc16_temp esc16_volt esc1_curr esc1_rpm esc1_temp esc1_volt esc2_curr esc2_rpm
esc2_temp esc2_volt esc3_curr esc3_rpm esc3_temp esc3_volt esc4_curr esc4_rpm esc4_temp esc4_volt
esc5_curr esc5_rpm esc5_temp esc5_volt esc6_curr esc6_rpm esc6_temp esc6_volt esc7_curr esc7_rpm
esc7_temp esc7_volt esc8_curr esc8_rpm esc8_temp esc8_volt esc9_curr esc9_rpm esc9_temp esc9_volt
failsafe fenceb_count fenceb_status fenceb_type fixedp freemem gen_current gen_maint_time
gen_runtime gen_speed gen_status gen_voltage gpsh_acc gpsh_acc2 gpshdg_acc gpshdg_acc2 gpshdop
gpshdop2 gpsstatus gpsstatus2 gpsv_acc gpsv_acc2 gpsvel_acc gpsvel_acc2 gpsyaw gpsyaw2 groundcourse
groundcourse2 groundspeed groundspeed2 gx gx2 gx3 gy gy2 gy3 gyrosq gyrosq2 gyrosq3 gz gz2 gz3
hwvoltage hygrohumi1 hygrohumi2 hygrotemp1 hygrotemp2 i2cerrors imu1_temp imu2_temp imu3_temp landed
landed_state lat lat2 lng lng2 load magfield magfield2 magfield3 mcumaxvolt mcuminvolt mcutemp
mcuvoltage mx mx2 mx3 my my2 my3 mz mz2 mz3 nav_bearing nav_pitch nav_roll noise opt_m_x opt_m_y
opt_qua opt_x opt_y packetdropremote pidD pidI pidP pidPDmod pidSRate pidachieved pidaxis piddesired
pidff pitch posd pose posn prearmstatus press_abs press_abs2 press_temp press_temp2 rangefinder1
rangefinder10 rangefinder2 rangefinder3 rangefinder4 rangefinder5 rangefinder6 rangefinder7
rangefinder8 rangefinder9 remnoise remrssi roll rpm1 rpm2 rssi rxerrors rxrssi safetyactive satcount
satcount2 satcountB servovoltage sonarrange sonarvoltage target_bearing ter_alt ter_curalt ter_load
ter_pend ter_space terrainactive txbuffer verticalspeed verticalspeed_fpm vibeclip0 vibeclip1
vibeclip2 vibex vibey vibez vlen voltageflag vtol_state vx vy vz watts wind_dir wind_vel wp_dist
wpno xtrack_error yaw
```

## Complete absent numeric/boolean reference names (92)

```text
AZToMAV DistFromMovingBase DistRSSIRemain ELToMAV GeoFenceDist KIndex QNH altd100 altd1000
altoffsethome battery_kmleft battery_mahperkm campointa campointb campointc capabilities ch1percent
crit_AOA customfield0 customfield1 customfield10 customfield11 customfield12 customfield13
customfield14 customfield15 customfield16 customfield17 customfield18 customfield19 customfield2
customfield3 customfield4 customfield5 customfield6 customfield7 customfield8 customfield9
distTraveled efi_fuelpressure gimballat gimballng glide_ratio horizondist linkqualitygcs localsnrdb
lowairspeed lowgroundspeed pidSRateAccZ pidSRateLanding pidSRatePitch pidSRateRoll pidSRateSteer
pidSRateYaw radius rateattitude rateposition raterc ratesensors ratestatus remotesnrdb speedup
targetairspeed targetalt targetaltd100 timeInAir timeInAirMinSec timeSinceArmInAir timesincelastshot
toh tot turng turnrate uid xpdr_adsb_tx_sys_fail xpdr_airborne_status xpdr_board_temperature
xpdr_es1090_tx_enabled xpdr_gps_no_fix xpdr_gps_unavail xpdr_ident_active
xpdr_interrogated_since_last xpdr_maint_req xpdr_mode_A_enabled xpdr_mode_A_squawk_code
xpdr_mode_C_enabled xpdr_mode_S_enabled xpdr_nacp xpdr_nic xpdr_status_pending xpdr_status_unavail
xpdr_x_bit_status
```

## Focused regression cases

The source test covers catalog uniqueness and units, no-sample state, same sysid
on different links/components, reentrant target switches, physical epoch reset,
independent 5-second freshness, sentinel invalidation, GPS/fused precedence,
battery ids/cells/charging, RC and servo port isolation, IMU/nav/pressure/EKF
conversions, derived prerequisites, dateline home distance and retained home,
trimmed MAVLink 2 scalars versus absent MAVLink 1 extensions, target destruction,
clock-triggered source destruction and extreme/backward clocks. No test sends
vehicle commands or mutates a transport.
