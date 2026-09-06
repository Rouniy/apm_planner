# Tools re-audit: routes are not functional parity

User reports missing dialogs remain. Root, three Codex streams and Claude over
TCP4096 rechecked source routes while finishing SFTP. This is an operational
queue, not a new completeness claim or replacement for the129-row ledger.

## Principal findings

The fixed header TOOLS menu has24 reference entries, plus the separate Signing
extension. Advanced has16 actions (14 working slices, partial Signing, unavailable
Support Proxy). Developer has32 implemented routes including SFTP. **None of these
counts means every Tools dialog is fully ported.** Some entries navigate to a
page, some open a dialog, and some dialogs still lack reference functionality.
An old class can have been rewritten; a new class can be partial. Git creation
dates are not acceptance evidence.

Highest-priority concrete gaps in existing Tools windows:

1. `MavlinkLogWindow`: the TLOG MATLAB gap is now closed functionally by a real
   bounded Level5 writer, named consent/Save As, progress/cancel and no-overwrite.
   Actual MP10 oracle plus SciPy proves283 real-log and160 extended synthetic
   variables bit-exact. See `MATLAB_TLOG_EXPORT_PORT.md` for remaining dialect,
   filesystem and visual limits. The separate BIN/LOG `MatLab.ProcessLog` and
   its DATA routes remain missing; enabling this button does not cover them.
2. `Terrain3DWindow`: guided clicks and the missing shared altitude producer are
   now implemented. DATA/PLAN/Simulation share the actual altitude/coordinate
   dialogs, while Terrain freezes rendered camera, point and physical instance
   before default-Cancel consent. The one-shot service shares the guided lane
   and central exact COMMAND_INT ACK/retry/quarantine, without changing Follow
   Me's payload/retry policy. Terrain uses explicit intent, never vehicle altitude,
   and preserves frame3/p2=0 (no mode change), including a numeric-frame warning.
   Full300/300, repeated new suites and inspected production X11 DATA/Terrain
   pass. Imagery draping and native/reference compatibility remain separate gaps;
   see `GUIDED_NAVIGATION_PORT.md`.
3. `MavlinkSigningWindow`: rekey/disable/recovery are unavailable. This requires
   persisted transitions, verified new-key traffic and explicit uncertain outcomes;
   SETUP_SIGNING has no ACK. Do not copy MP10's optimistic success message or
   weaken the existing fail-closed policy merely to enable a button.

Support Proxy is also in **MP10's own `_notPorted` list** in
`ConfigAdvancedViewModel.cs`; only the Original tree has SerialSupportProxy.
Its visible Qt placeholder is not a regression from a working MP10 dialog and
does not outrank the actual working-reference features above. OSD Video depends
on Qt Multimedia: the verified Qt5/audio build has it; a deliberately silent/
incomplete developer build can leave its action unavailable.

## Other missing tool entry points, not covered by Developer count

MP10 `GCSViews/FlightDataView.axaml` has **15 TabItems**, including Drone ID.
Qt `QGCTabbedInfoView` has six mapped tabs plus the useful Quick (Legacy)
extension after `installQuickView`, seven total. Nine reference tabs are absent:
Drone ID, Gauges, Transponder, Servo/Relay, Aux Function, Scripts, Payload Control,
Telemetry Logs and DataFlash Logs. The initial independent audit missed Drone ID
because its x:Name precedes Header; root corrected this against the full source.

The missing log tabs contain Review a Log, Auto Analysis, BIN→LOG, KML/GPX and
MATLAB entry points. Some backend functionality already exists elsewhere, but
the DATA route does not. Retain the useful legacy LogAnalysis window until a
full LogBrowse replacement is wired; naming an old widget does not close that gap.

Additional separate backlog items: header Readonly/Connection List and log-tool
shortcuts; PLAN Terrain Maker, Tracker Home and map-overlay loading; DATA GeoRef.
Many SETUP pages retain useful legacy implementations with partial labels.
Terminal still lacks the MP10 SSH transport; libssh2 is now available but that
does not itself implement a terminal. Do not delete retained modules for counts.

## Queue discipline

SFTP, TLOG MATLAB and guided Terrain dialogs are implemented; continue other single-vehicle
Tools gaps. Restore missing log-tool routes as complete UI/service
slices, then resume Settings/CONFIG (15 route factories is not full functionality;
Planner still21/64 controls). Swarm remains last. SETUP46 pages/eight absent
reference routes and its disconnected/profile visibility gates remain explicit.

Evidence inputs: source tracing by root and Codex, Claude TCP c299 corrected
report (`15671e9c7ad88c147f07307b324e271f389d9c640bd5a48bc6272944133cec49`)
and c300/c302 security reviews, plus production runtime tests per slice.
SFTP's first screenshot showed a real empty-grid defect despite rowCount=1:
blocking internal model notifications prevented display. The fix keeps model
notifications live and asserts visual item geometry, not just model counts.
