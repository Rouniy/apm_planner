# Developer Tools — exact single-vehicle actions

This slice ports six existing MP10 actions, keeping the original32-item
Developer page and the Tools → Developer Tools / SETUP shared route. It does
not complete Developer Tools, Signing transitions, or the overall port.

## Wire semantics

| Action | Exact operation | Result |
|---|---|---|
| Set QNH | GND_ABS_PRESS, else BARO1_GND_PRESS,80000–120000Pa | Matching typed PARAM_VALUE, not submission |
| Adjust Barometer Altitude | Same pressure parameter: current + metres ×11.1; offset±100m, resulting pressure within bounds | Matching typed PARAM_VALUE |
| Force Accel Calibrated | PREFLIGHT_CALIBRATION, param5=76, others0 | Exact COMMAND_ACK |
| Force Compass Calibrated | PREFLIGHT_CALIBRATION, param2=76, others0 | Exact COMMAND_ACK |
| Reboot Vehicle | PREFLIGHT_REBOOT_SHUTDOWN, param1=1, others0 | ACK means acknowledged request, not observed completed reboot |
| Reboot to DFU | PREFLIGHT_REBOOT_SHUTDOWN,42/24/71/99/0/0/0 | Often no ACK because firmware immediately reboots; outcome remains uncertain |

The sources of these semantics are MP10
`ViewModels/GCSViews/ConfigurationView/ConfigDeveloperToolsViewModel.cs`,
`MAVLinkInterface.doDFUBoot/doReboot`, and local ArduPilot
`libraries/GCS_MAVLink/GCS_Common.cpp`, `libraries/AP_Baro/AP_Baro.cpp`,
`libraries/AP_Param/AP_Param.cpp` and `libraries/AP_BoardConfig/AP_BoardConfig.h`.

DFU is **not** param1=3 (hold in the ArduPilot bootloader). Its99 branch is
controlled by HAL_ENABLE_DFU_BOOT, separately from failure-injection compilation.
Signed firmware refuses it; unsupported boards may reply UNSUPPORTED. Firmware
handles the DFU branch before its ordinary armed check, so the Qt fresh-disarmed
guard is particularly important. Neither a lost heartbeat nor an ACK timeout
proves entry to DFU. Do not retry or fall back to another kind of reboot.

The historical “QNH” label changes the ground calibration reference, not a
certified sea-level-pressure setting. Modern BARO1_GND_PRESS is internal and
normally refuses MAVLink writes. Its fallback is therefore available only when
an exact complete snapshot already reports BRD_OPTIONS bit2/value4 enabled.
The tool never changes BRD_OPTIONS or invents a missing pressure parameter.
Pressure must have runtime REAL32 type; integer metadata is refused before
consent so fractional QNH/11.1 Pa/m values cannot be silently rounded to integers.
GND_ABS_PRESS is retained for legacy firmware that exposes it; the firmware may
still deny its write, and the outcome must remain a rejection/uncertainty.
BARO_ALT_OFFSET and fast barometer recalibration have different semantics and
are not silently substituted for either MP10 pressure action.

## Ownership and safety

One application-owned DeveloperVehicleToolService captures an immutable plan
before the first prompt: selected endpoint/generation, physical session and
vehicle-instance epochs, ArduPilot autopilot component1, one observed autopilot
on the physical route, and a disarmed heartbeat at most3000ms old. Pressure
plans additionally pin complete typed parameter records and any BRD_OPTIONS
capability record. Every consent return revalidates the same plan.

All six changes require a named default-Cancel confirmation. Force-calibration
warnings explicitly describe bypassing ordinary calibration for recovery after
a parameter wipe. Closing a prompt sends nothing; closing a page after admission
does not cancel the application-owned ACK waiter. Bounded history survives page
reopening. Missing/stale/armed/unsupported states have explicit disabled reasons.

VehicleCommandService has a separate single-vehicle exact reservation policy;
the existing Swarm route policy is not broadened to serial or learned UDP.
Pressure operations own both command and parameter lanes. Active/uncertain
compass calibration blocks these recovery tools; reservations block a new
legacy calibration command from racing an admitted developer operation.

Operation-specific guards run last, immediately before a command or each
parameter transmission/retry. After a guard, only non-callback identity checks
precede the writer. This avoids an injected route validator changing armed state
after the last safety check. Refusal before any frame is distinct from an
uncertain partial attempt. Exact terminal tokens, not raw global ACKs or a send
return, own completion. Existing endpoint/command quarantine remains in force.

## Verification and remaining scope

Qt5/audio build, focused7/7 and full239/239 tests (29.59 seconds) pass.
Verification evidence is recorded in CURRENT_STATE.md and
`/tmp/apm-developer-vehicle.Nwtyz6/` (`full.log`, `focused-3.log`, `x11.log`).
The production audit uses an in-process TCP subclass, never a network flight
controller; it drives the real Tools route, both numeric prompts, all six Cancel
and Yes paths, typed pressure echoes and exact command payloads. Both offscreen
and X11 exit0, with no fixture left active. The initial snapshot becomes readable
before the6-second legacy PARAM traffic fence permits writes; the audit waits
actual reservation admission, without shortening the production fence. A fake
DFU ACK tests UI routing only; hardware commonly reboots without acknowledging.

Upgrade Bootloader, parameter recovery, MAVFTP download, remote logging and the
other Developer file/transport tools remain separate functional gaps. Native
Windows/macOS/high-DPI/reference screenshots and physical DFU/reboot/calibration
evidence remain release gates. Signing rekey/disable/reconciliation is still
open; Settings follows single-vehicle Tools and Swarm remains last.
