# CONFIG and Planner Settings inventory audit

Updated: 2026-09-05.

This audit compares Mission Planner 10 `ConfigViewModel` and
`ConfigPlannerView.axaml` with the active Qt `ConfigView` and native
`ConfigPlannerView`. It counts registered routes and interactive controls,
not support classes or controls that are merely present in source files.

## CONFIG route count

Mission Planner 10 registers 15 ordered CONFIG routes. Qt models all 15 in
`ConfigRouteProfile` and now registers all 15 as concrete factories. There are
no null factories in the active Qt navigation.

| # | Mission Planner 10 route | Qt status |
|---:|---|---|
| 1 | Flight Modes | Native exact-target page; in progress |
| 2 | Standard Params | Native metadata-backed page; partial |
| 3 | Advanced Params | Native metadata-backed page; partial |
| 4 | GeoFence | Useful legacy Copter page retained; partial |
| 5 | Basic Tuning | Useful legacy Copter page retained; partial |
| 6 | Heli Setup | Native legacy-heli editor; in progress |
| 7 | Basic Tuning (Plane) | Useful legacy Plane page retained; partial |
| 8 | Basic Tuning (Rover) | Useful legacy Rover page retained; partial |
| 9 | Extended / QP Extended Tuning | Native Plane QP page; useful legacy Copter/Heli editor retained |
| 10 | Onboard OSD | Native phase-one editor; in progress |
| 11 | MAVFtp | Native remote file browser and exact-target service; in progress |
| 12 | User Params | Native metadata-backed page; in progress |
| 13 | Full Parameter List | Native staged parameter page; in progress |
| 14 | Planner | Native nine-section page; partial |
| 15 | Planner (Advanced) | Native read-only settings snapshot; in progress |

There is no longer a CONFIG route-level factory or vehicle-dispatch gap. The
shared route creates the native `QP Extended Tuning` page for Plane and keeps
the useful non-empty legacy editor for Copter/Heli. Existing APM Planner pages
remain only where they provide a same-domain workflow; their Legacy/partial
classification is not a parity claim.

Factory coverage is therefore 15/15, but functional parity is not. The current
truthful route policy deliberately narrows `Standard Params` and `Advanced
Params` to Copter/Heli/Plane/Rover and `GeoFence` to Copter/Heli. `Extended
Tuning` is actionable for Copter/Heli/Plane: the stable route id changes its
visible Plane label to `QP Extended Tuning` without losing saved selection,
then dispatches Plane to the native Q controller/INS page and Copter/Heli to
the retained legacy ATC/PSC/WPNAV editor.

The related SETUP `Advanced` launcher is a separate auxiliary-tools inventory:
all 16 MP10 actions are present, fourteen open working shared tools and two are
visibly disabled. MAVLink Inspector, Mavlink Mirror, NMEA, Cursor-on-Target /
TAK, DataFlash Spectrogram, External Guided, Follow Me, Moving Base, Map Tile
Cache and Proximity reuse working application actions. FFT, Param gen, Anon Log
and Warning Manager now have native workflows; FFT Setup is also a direct page.
MAVLink Signing and upstream-placeholder Support Proxy remain unavailable.
They must not be counted as working merely because every direct CONFIG route
has a factory.

Signing now has a tested AES-GCM/PBKDF2 vault and a production key-domain manager
binding exact/legacy TX and authenticated RX. Internal offline configuration
creates `mavlink-signing/signing-clock.state` and `signing-link-ids.state` under
the writable application data directory; ordinary unprotected connections do not.
These hold timestamp reservations and stable profile IDs, never signing secrets.
Manual `LINKMANAGER/LINKS` rows now persist `profileId` and `signingRequired`;
`MAVLinkSigning/Profiles/ID/fingerprint` stores strict SHA256 metadata and
`MAVLinkSigning/StartupRequiredPorts/PORT` guards automatic UDP restoration.
Required profiles restore locked before connection; corrupt/missing hinted
policies stay blocked. The application-owned asynchronous vault service uses
`mavlink-signing/authkeys.vault`, without implicitly creating a vault. These
infrastructure settings do not increase the direct Planner-control count.
Modeless Add/Use/Delete/Disable and provisioning remain gates in
`MAVLINK_SIGNING_PORT.md`. The master passphrase is never
persisted; OpenSSL Crypto remains a required build dependency.

Warning Manager persists compatible `warnings.xml` under the writable application
data directory and shares `speechenable` / `speech_armed_only` with Planner.
DATA Quick uses `quickViewCount`, `quickViewColumns` and `quickView1`..`12`, and
is the native warning-color consumer. Its 394-field exact telemetry catalog and
unit/import limitations are documented in `WARNING_MANAGER_PORT.md` and
`WARNING_TELEMETRY_CATALOG.md`; it is not another direct CONFIG route/control.

The legacy `Heli Setup` route is capability-gated by the exact MP10
`H_SWASH_TYPE` marker; current `H_SW_TYPE` vehicles use the separate native
SETUP `Heli Setup (4.0+)` page and are never collapsed into the binary CCPM/H1
editor. The native page has the exact 43 logical parameter rows and aliases,
the six visible manual-servo commands, swash controls, collective/acro plot,
RC3/RC4 input, servo-output-6 cursor and observed ranges. Writes are disarmed,
exact-target and terminal-batch correlated. Non-zero manual modes require a
default-Cancel blade-removal confirmation, mode 5 stays disabled unless
firmware metadata declares it, and leaving the page makes a best-effort
`H_SV_MAN=0` request. Lost ACKs and raw parameter echoes cannot clear the
conservative manual-override latch; only an exact successful zero batch can.
Link/target uncertainty remains an operator-visible warning.

## Planner Settings section and control count

Mission Planner 10 has exactly nine Planner Settings sections, in this order:

1. Display
2. Speech
3. Flight Command Shortcuts
4. Waypoints / Connect
5. Startup UDP Listeners
6. Telemetry Stream Rates (Hz)
7. Aircraft Icon / Map
8. Logs
9. Advanced

The reference AXAML contains exactly 64 interactive controls: 35 checkboxes,
11 combo boxes, 11 numeric editors, five buttons and two text boxes. The
native Qt page now renders all nine sections in the same order, and every
section contains either a working control or an explicit unavailable note;
none is an empty placeholder.

Working settings in the first native slice are Alt/Dist units, the shared
Basic/Advanced/Custom DisplayView profile, exact restart-scoped dual Startup
UDP listeners, the Qt map renderer, audio mute, GCS heartbeat, MAVLink
logging, DataFlash/tlog directories, beta update channel and system proxy.
CONFIG → Planner is now the sole native Planner Settings route and owns one
application-wide model. The old dormant `actionSettings`, its duplicate
top-level dialog and their stale-overwrite risk have been removed. This
matches MP10, which likewise exposes Planner only through CONFIG.

The four live slices add twelve direct MP10 controls, bringing the working
native-equivalent count to 21 of 64: `Enable HUD Overlay`, `Enable Speech`,
`Test Speech`, `Armed Only`, `Waypoint`, `Mode`, `Custom`, `Battery`,
`Alt Warning`, `Arm/Disarm`, `Low Speed` and `Message Severity`.
HUD visibility now has one application-owned `CHK_hudshow` service shared with
the HUD context menu, so either surface updates the other immediately. The
speech master uses the canonical `speechenable` key and gates all TTS without
disabling WAV alerts or beeps; Test Speech reports disabled, muted, ready and
unavailable-backend states instead of failing silently. The central announcer
applies MP10 event keys, templates, token substitution and current-vehicle
gates for waypoint, mode, custom status, battery, altitude, arm state and low
speed; `speech_armed_only` gates all except the arm/disarm transition itself.
The one-second policy loop also implements MP10's armed No Data warning. Its
telemetry comes from one immutable `(link, system, component, generation)`
lease, so an equal sysid on another physical link cannot drive a phrase. The
TTS adapter serializes a current phrase plus four pending phrases, coalesces
duplicates, bounds text length, waits for the backend's actual Ready state
after a watchdog stop and clears stale speech on target changes. Retained Qt
PreArm/Arm and `#audio` phrases now use the same complete MAVLink 2 assembly,
exact endpoint, one-shot queue, master and Armed Only policy instead of the
old direct packet-fragment path; legacy battery phrases preserve their exact
endpoint through their existing path. Legacy link-state and object-detection speech is withheld until
those producers preserve physical-link identity.
As a retained Qt audio directive, a non-empty `#audio:` message bypasses the
severity threshold and is shown without the raw prefix. Chunk completion
follows MAVLink's null-terminator contract, so a text whose length is an exact
multiple of 50 bytes requires the specified terminating empty chunk.

`Message Severity` stores the exact MP10 `severity` key with default Warning
(`4`) and all eight MAV severity names in order. The high-message source
assembles complete UTF-8 STATUSTEXT chunks, applies the legacy ArduPilot
severity compatibility thresholds, rechecks the immutable target lease after
reentrant callbacks and promotes severity-at-or-above-threshold plus
`Tuning:`, `PreArm:` and `Arm:` prefixes. The DATA HUD shows one line for ten
seconds at two-thirds height with MP10 red/yellow/white severity colors; No
Data updates the same banner even when TTS is disabled. Repeated identical
messages refresh display lifetime without repeating speech.

The useful Qt audio-mute control remains an extension and is not included in
the 21-control parity count. The first real-X11 production-route run toggles
HUD and the speech master, verifies their exact keys and observes the DATA HUD
disappear live (`/tmp/apm-planner-live-smoke.3anzjo`). The follow-up run
completes the real Mode and Battery prompts, verifies their exact MP10 keys,
templates and thresholds, confirms Battery-off does not disable the master,
and exits cleanly (`/tmp/apm-planner-speech-smoke.PE5xfb`). A current follow-up
real-X11 run shows all eight MP10 speech sub-controls in their reference order,
persists the master key and exits cleanly
(`/tmp/apm-planner-speech-c2.Jz91ZI/planner-eight-controls.png`). Prompt
cancellation is transactional: an event is enabled only after every required
value is accepted, so cancellation cannot leave a half-configured policy.
The current severity run uses one live synthetic exact UDP endpoint, renders
`Message Severity = Info` on the production Planner route, then switches to
DATA and renders its error STATUSTEXT on the native HUD before a clean exit
(`/tmp/apm-status-smoke.Ucj2NV`).

The remaining MP10 controls are not represented as fake toggles. Language,
theme/theme editor, speed/OSD color, speech level and the vario consumer,
safety-confirmed flight shortcuts,
connect policies, the five target-safe telemetry rates and GCS identity,
distance-to-home display, track length, map vectors/overlays/cache/external
ADS-B, and the remaining advanced policies are
called out in their corresponding section. Useful old APM Planner themes,
file paths and seven-rate telemetry editor remain reachable through an
explicitly named Legacy dialog; dead reconnect/donate/titlebar/low-power and
split-brain MAVLink identity controls are hidden there.

The standalone `TOOLS → Connection Options` dialog now owns three additional
MP10 settings that are also consumed by the connection/runtime layer:

| Control | Canonical key | Default | Qt owner/consumer |
|---|---|---:|---|
| Default baud rate | `baudrate` | `115200` | Staged dialog; fallback for new `SerialConnection` instances and the empty header baud field |
| Send GCS heartbeat | `CHK_GCSheartbeat` | `true` | Staged dialog; `MainWindow::enableHeartbeat()` updates current UAS instances and new UAS instances read the same policy |
| GCS system id | `gcsid` | `255` | Staged dialog with read-only fallback from `GCS_sysid`; all application senders load it consistently on restart |

The dialog preserves an unknown stored baud as an unselected staged value,
matching MP10 instead of silently rewriting an older profile. Existing serial
links persist their baud only in `SERIALLINK_COMM_PORTMAP`; they no longer
overwrite the global default. Immediate live GCS-id mutation is withheld until
the protocol, every UAS, every application-owned exact service and page-owned
clients can switch sender identity atomically without invalidating an in-flight
ACK transaction. This restart-scoped safety difference is recorded as
`TOOLS-040`.

The native `Flight Modes` page is shared by CONFIG and SETUP. It renders all
six MP10 mode rows and PWM bands for Copter, Plane, Rover and PX4 schemas.
ArduPilot slot choices come from packaged metadata (plus the MP10
`31: ModelCal` Copter extension); PX4 `COM_FLTMODE1..6` uses its separate
0..12 slot enum and never the packed heartbeat `custom_mode`. Unknown values
remain selectable. Copter always exposes the `SIMPLE` and `SUPER_SIMPLE`
columns and help, while each column stays disabled when its parameter is not
provided by the vehicle. Current Mode,
numeric Current PWM, active-row highlighting and the Super Simple help route
consume telemetry from the immutable physical-link target rather than the
legacy global active-UAS stream. Edits require a fresh exact heartbeat and a
disarmed vehicle; Save submits one changed-only ordered parameter batch and a
partial outcome forces full-snapshot reconciliation. Full parameter refreshes
preserve staged per-field edits, while reconciliation deliberately replaces
them with the new committed snapshot. The additional `Refresh Params` action
is the explicit recovery path for incomplete or uncertain state. The old
`FlightModeConfig` source remains compiled for legacy-shell compatibility but
neither active MP10 route instantiates it.

The 15 direct CONFIG routes now all create a truthful non-empty page for their
supported vehicle contexts. Planner Settings is the next broad control-level
gap: all nine sections are present, but many of the audited 64 controls are
still explicitly unavailable. The orphaned standalone `actionSettings` has
been removed, so the next route-level check is a production click-through
that instantiates every visible CONFIG row and proves its page is non-empty.

## SETUP cross-check

The separate `SETUP_INVENTORY_AUDIT.md` remains the count baseline: both
applications have four logical sections (ungrouped plus three named groups).
Mission Planner 10 registers 53 pages plus three group headings, or 56
navigation entries. At the current checkpoint Qt registers 46 concrete pages
plus the same three group headings, or 49 entries. Qt is missing eight reference
pages and intentionally keeps one useful extra page, `QML Plugins`. Offline
connection filtering is separate from this registered-page inventory.
