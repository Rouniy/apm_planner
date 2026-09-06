# Upgrade Bootloader

This slice fills the existing `UpgradeBootloaderButton` in TOOLS → Developer
Tools and SETUP → Developer Tools. It does not add a Setup section or flash a
user-selected firmware file. Developer now has 18/32 implemented actions; the
other 14 remain explicitly unavailable. Swarm stays at the end of the queue.

## Reference and intentional differences

The local Mission Planner 10 reference is
`ViewModels/GCSViews/ConfigurationView/ConfigDeveloperToolsViewModel.cs`,
`UpgradeBootloaderAsync` (lines675–687): safe-vehicle admission, two confirmations,
then `MAV_CMD_FLASH_BOOTLOADER` (42650), parameters `[0,0,0,0,290876,0,0]`.
The running firmware supplies its own board-specific embedded bootloader.
There is no bootloader download, board picker, APJ upload or version comparison
in this workflow.

ArduPilot's local `libraries/GCS_MAVLink/GCS_Common.cpp`,
`handle_command_flash_bootloader` (lines4882–4905), validates the magic and maps
both `OK` and `NO_CHANGE` to ACCEPTED. Its
`libraries/AP_HAL_ChibiOS/Util.cpp`, `flash_bootloader` (lines234–340), loads
`bootloader.bin` from ROMFS, checks signed firmware policy where configured,
compares flash, then erases/writes when necessary. An error can occur after
erasure; a rejected/failed command is not evidence that flash was untouched.
Support depends on the firmware build/board and is not established by ordinary
MAVLink capability bits.
SITL/Linux builds normally report UNSUPPORTED; fixture ACCEPTED does not imply
SITL support. Some boards also update the persistent-parameter block in the
bootloader sector. The bootloader is used at the next boot; this action does not
send a reboot command.

Qt intentionally sends exactly one COMMAND_LONG frame with confirmation0 and
does not repeat an unacknowledged flash command. Both acknowledgement inactivity
and absolute lifetime are set to five minutes. IN_PROGRESS cannot extend the
absolute lifetime. This differs from MP's retrying generic command helper.

## Consent and ownership

The application-owned `DeveloperVehicleToolService` captures one selected
target generation and vehicle instance. Existing fresh disarmed ArduPilot,
component1 and physical-route gates apply at preparation and immediately before
transmission. The UI validates the same plan before and after each asynchronous
confirmation; changing targets, changing away and back, arming, closing the page
or destroying a prompt before submission sends nothing.

Both named dialogs show link/system/component and default/Escape Cancel:

- `DeveloperUpgradeBootloaderSourceConfirmation` explains the embedded source
  and unsupported-build possibility.
- `DeveloperUpgradeBootloaderFlashConfirmation` explains permanent flash,
  interruption/bricking risk, stable power, the five-minute precaution and
  absence of automatic retries. It explicitly warns that telemetry/heartbeats
  may pause and the UI can indicate link loss while flashing.

Only the second Yes admits a command. The shared Developer interlocks apply to
the new action too. After admission, closing or destroying the page does not
undo flashing or terminate the application-owned waiter. Results remain in the
service's bounded in-memory history; there is no durable journal across app
exit and no claim that shutdown cancels the physical operation.

An exact ACCEPTED response is reported as flashed **or already current**, not a
proven version upgrade. Missing ACK, target/link loss or transport uncertainty
remain an unknown outcome; the tool does not automatically retry or power-cycle.
Firmware failure and unsupported responses are reported, not converted into
success. Existing exact ACK matching checks physical link, sender, command and
destination extensions; MAVLink1/zero extension destinations remain supported.

## Verification and limits

Verification uses unit fixtures and the production X11 Tools route against an
in-process TCPLink subclass that opens no socket. No network SITL, real board,
user firmware or flash contents are modified. Runtime checks both Cancel
boundaries, no transmission after the first Yes, both exact-target warnings,
one fully decoded command after the second Yes, and accepted/already-current
wording. `APM_BOOTLOADER_AUDIT_SCREENSHOT` optionally captures final consent.

Service/widget tests cover exact payload, ACK filtering and outcomes, bounded
timeout/no retry, stale/armed targets and prompt/page lifetime. The slice also
guards the shared command service after terminal callbacks that delete it in
deadline, link-forget and vehicle-retirement paths, with regression tests.

Qt5/audio configure/build pass; the final full suite passes246/246 in30.41s.
Production X11 exits0 with zero audit failures. Root inspected the readable
final warning/target/buttons screenshot. Evidence:
`/tmp/apm-bootloader.zYAKC5/{configure.log,build-final.log,full-final.log,x11-final.log,bootloader-final-consent.png}`.
The initial focused run failed only the new widget fixture's post-ABA
re-enable check: it omitted a fresh heartbeat for the new target generation.
The corrected test now proves old-consent rejection despite fresh disarmed
telemetry, and passes in the final suite.
Claude TCP c218 independently audited the reference and code; its10170-byte
report SHA256 is `323baae9202ca058614117da8777d0a49879cf5ca76d06e922fe7d76efd76411`.
Root corrected its initial claim that ACK-time validation never uses heartbeat
freshness: production `LinkManager.cc` configures registry `validateLease` there.
Native Windows/macOS, real bootloader support, physical flash success/recovery,
cryptographic provenance and Mission Planner reference-pixel parity are not
verified by this slice. ACK correlation has no wire transaction nonce; the
existing bounded quarantine is not proof against arbitrarily delayed replies.
The existing ACK lease validator also requires fresh telemetry: a long firmware
flash stall can conservatively yield unknown outcome even when an ACK arrives.
It is intentionally not treated as permission to weaken pre-write safety gates.
