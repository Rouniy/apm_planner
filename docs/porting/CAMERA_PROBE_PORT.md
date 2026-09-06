# Probe MAVLink Camera

The existing `ProbeMavlinkCameraButton` in Developer Tools invokes one
application-owned `CameraProbeService` through a page-owned
`CameraProbeController`. It is not an empty widget or an autopilot capture tool.
The button remains discoverable offline; without a selected connection and a
known camera, the page log explains why no command was sent.

## Reference and wire contract

Primary reference: MP10 `ConfigDeveloperToolsViewModel.ProbeCameraAsync`,
`ExtLibs/ArduPilot/Camera.cs::test`, `MAVLinkInterface.doCommandAsyncCore`, and
`MAVList`. The consent in MP10 already names mode selection and streaming;
earlier review claims that it omits them or does not await ACK were incorrect.

Choose the first heartbeat-known component100 on the selected physical link,
regardless of its system id or autopilot type. Qt uses the smallest discovered
component-instance epoch to represent first-known order, not sorted system id.
Never substitute component1, require ArduPilot camera firmware, or change the
selected autopilot. A camera with `MAV_AUTOPILOT_INVALID` is valid.

The frozen plan names the selected generation and exact camera, physical epoch
and discovered instance. Default/Escape Cancel sends nothing. Confirmation
explicitly warns that image mode and video streaming can change camera state.

| Order | COMMAND_LONG | Parameters |
|---|---|---|
| 1 | REQUEST_CAMERA_INFORMATION (521) | seven positive zero floats |
| 2 | REQUEST_VIDEO_STREAM_INFORMATION (2504) | seven positive zero floats |
| 3 | REQUEST_CAMERA_SETTINGS (522) | seven positive zero floats |
| 4 | SET_CAMERA_MODE (530) | seven positive zero floats |
| 5 | REQUEST_STORAGE_INFORMATION (525) | seven positive zero floats |
| 6 | VIDEO_START_STREAMING (2502) | seven positive zero floats |

These are the actual reference values, including its legacy request flags.
Modern dialects interpret zero in the information/settings request flag and
storage request flag as no action. Mode0 means image; stream0 means all streams.
Do not silently replace zeros with1/NaN, omit the state changes, or substitute
REQUEST_MESSAGE while claiming this is the reference probe.

One command runs at a time, waiting2000ms for ACK, with at most three retries
(confirmation0,1,2,3). ACCEPTED and other terminal MAV_RESULT values are logged;
even rejection continues with the next command, as in MP10. Exhausted timeout
stops the remaining sequence. A silent camera therefore stops after the first
command's approximately eight-second window, not after six silent commands.
IN_PROGRESS resets the inactivity window and disables retries.

## Ownership and deliberate differences

Camera commands reuse the central `VehicleCommandService`, with a distinct
typed component lease rather than inventing a Swarm vehicle lease. Endpoint
reservations, command ACK arbitration and late-ACK quarantine remain shared.
Production ACKs enter once through the physical packet path, before the legacy
UAS gate. They are not replayed into the arbiter from legacy message delivery.
Source system/component, command, physical epoch and nonzero MAVLink2 ACK
destination extensions must match. Final pre-writer validation covers callback
changes during signing; actual writer attempts are counted before callbacks.
Other command callers retain their previous zero-retry default.
The operation safety callback runs at admission and again before the writer,
including retries; it must tolerate repeated validation and must not perform
the requested command or any other user mutation itself. Production uses the
physical dispatcher; the older epoch-less/component-only observation entry
points remain compatibility seams for isolated unit fixtures.

Qt freezes consent rather than MP10's post-confirmation camera re-resolution.
Changing the selected vehicle, retiring the camera, losing the physical route
or closing the page stops remaining commands. The existing registry retires
components after ten seconds without traffic. Exact supported serial/TCP/UDP
routes and signing readiness are required; UDP with multiple peers is refused.
Repeated IN_PROGRESS has an absolute30-second limit per command, unlike the
reference's potentially unbounded wait.

Cancel closes only the owned reservation, forbids retries and drains the current
ACK/deadline without blocking the UI. Already submitted changes are not undone.
The application service keeps at most128 log lines across page recreation;
history is not persisted across application restarts. Closing an old page
cannot cancel a newer or foreign operation. Shutdown can detach an outstanding
command with an explicitly uncertain result. A six-second late-ACK quarantine
after an uncertain command can temporarily refuse an immediate new probe,
including a quickly reopened physical connection: submission quarantine is
deliberately conservative across domains and reconnects, not an ACK nonce.
MAVLink ACKs have no transaction nonce: this bounded quarantine cannot establish
the origin of an arbitrarily late ACK, and unsigned peers are not authenticated.

The modeless progress dialog is an added Qt control, not a33rd Developer action.
Every step reports accepted/rejected/not-sent/uncertain and actual attempts.
An ACK is not evidence that camera information, a URI or video arrived. Responses
remain inspectable in the existing MAVLink Inspector; this slice does not add
Flight Data video controls, camera definition parsing or stream playback.

## Verification and remaining gates

Unit fixtures exercise the actual registry/target/transmitter/command services,
not a substituted autopilot. The production Developer audit injects an isolated
camera heartbeat and ACKs on its private in-process link; it checks real menu
navigation, default Cancel, stale consent, six exact zero-parameter commands,
continuation after rejection, unchanged selected target and resulting page log.
No network SITL, live vehicle or physical camera is modified by these tests.

Final run evidence is recorded in `CURRENT_STATE.md`. Hardware camera behavior,
the older bundled dialect, native Windows/macOS builds and MP10/HiDPI screenshot
comparison remain open gates; functional availability is not strict parity.
