# Warning Manager and native Quick coloring

Reference: MP10 `WarningManagerViewModel.cs`, `WarningManagerView.axaml`,
`WarningManagerWindow.cs`, `ExtLibs/Utilities/Warnings/CustomWarning.cs` and
`WarningEngine.cs`, plus `FlightDataViewModel.QuickItems` / `QuickWarningStyle`.

## Working path

SETUP → Advanced → Warning Manager opens one reusable 1250×620 modeless editor.
Its application-owned engine remains active after the observer closes. Add,
edit and remove IF roots / linear AND chains take effect immediately; removing
a child splices its descendant, removing a root removes its chain. Save writes
`AppPaths::writableDataDirectory()/warnings.xml` atomically through QSaveFile.
The XML is the .NET `ArrayOfCustomWarning` / `CustomWarning` / `Child` schema,
including child action fields, enum names, invariant doubles and optional nil
children. Unknown field names survive and show unavailable. Bad XML, enums,
colors, DTD/entities, duplicate fields, excessive sizes or non-finite thresholds
fail without replacing the active rules or previous destination. Maximums are
4 MiB, 512 conditions, 16 levels, 256-character names and 4096-character text.

All seven comparison choices, two action types, ten colors, 0..86400-second
repeat interval and `{name}`, `{value}`, `{warning}` tokens are present.
EQ/NEQ retain exact floating-point comparison. The finite C-locale threshold
editor preserves all double digits, including imported values outside MP10's
normal ±999999 editor range; no hidden clamp/rounding changes active meaning.
Live row hints show units, current data, NONE/inactive or unavailable/stale.

`WarningTelemetrySource` consumes parser-validated pre-UAS physical ingress,
revalidates the link session, and follows an exact selected link/system/component
and target generation. Independent source epochs clear values at physical
disconnect/reconnect even if selection is unchanged. Streaming fields expire
after five seconds; unknown data is absent, never a fabricated zero. The 394/486
numeric/boolean name inventory, 92 absent names, producer and unit differences
are in `WARNING_TELEMETRY_CATALOG.md`; name coverage is not full producer parity.

## Evaluation and presentation

The event-driven 250 ms engine evaluates the entire AND chain before consuming
the root's repeat interval. Child intervals/actions remain serializable but do
not separately fire. Stable root identities preserve cooldowns across editor
changes; editing one rule cannot re-arm every other warning. Coloring is
continuous and ignores repeat, with the last matching root winning per field;
a matching NoColor explicitly resets the field. False later rules do not erase
earlier matches. Removed fields and target/source epoch changes clear colors;
still-owned edited fields are recomputed on the next tick without a blank flash.
These deliberately fix the reference's partial-AND timer consumption, flashing
colors and conflicting resets. Missing fields cannot stop independent roots.

Due SpeakAndText events pass once through SpeechAnnouncer into the existing
bounded TTS queue, honoring Speech master and Armed Only. The same high-message
channel displays a ten-second red EMERGENCY-severity banner, matching MP10;
it receives an empty speech payload so it cannot speak the warning a second
time. Identical due repeats are retained; queue saturation/backend failure can
reject audio and is not retried as a burst. Repeat zero remains every 250 ms,
with shared queue coalescing/bounds rather than a blocking wait. A warning's
visible message does not depend on speech being enabled.

DATA's native Quick tab now uses MP10's six defaults and colors, exact field
keys, double-click selection, right-click layout, 1..12 cells and 1..6 columns.
Persistent keys are `quickViewCount`, `quickViewColumns`, `quickView1`..`12`.
Warning colors affect the background with the reference black/white contrast
rule, and NoColor restores the configured foreground and transparent background.
Unavailable values show an em dash and no warning color. The older raw-value
selector remains as **Quick (Legacy)**, not as an exact-target coloring consumer.
Descriptions and numbers now scale with each cell and fit their actual label
rectangles. Local pixel-size rules override the application's global 11px theme;
font-derived minimum hints cannot prevent shrinking the splitter/grid again.
Refitting follows cell/label resize, field/unit/value edits and layout rebuilds.
Warning colors and foreground contrast preserve the fitted sizes.

## Differences and remaining gates

- The window is a singleton observer instead of multiple mutable MP10 windows.
- Thresholds use the labeled canonical/raw units, not global display preferences.
  Imported XML has no unit metadata: thresholds authored in feet/knots must be
  reviewed. The editor shows this warning explicitly; no automatic conversion or
  claim of interchangeable physical trigger thresholds is made.
- MP10's non-numeric readable properties are excluded; unsupported names remain
  visible if loaded. The 92 missing numeric properties and additional producers,
  direct-vs-smoothed battery values, raw temperatures/board voltage, radio
  component scope and derived-home differences remain explicit catalog gaps.
- A shared bounded queue avoids the original busy-wait and double-speech paths;
  a per-rule audio-delivery/rejection history is not yet presented.
- Quick still uses raw field labels instead of all reference DisplayText
  descriptions. Adaptive text fitting is restored and checked with the production
  theme. Native/reference
  visual comparison, Windows/macOS speech plugins and physical hardware remain.
- Static LinkManager destruction can occur after QApplication in the production
  audit. The source invalidates its state but suppresses epoch notification while
  Qt is closing down, preventing repaint through an already destroyed Qt style.

## Verification

Root-owned Qt5/audio build completed. New engine, source, editor and Quick suites
plus speech and production route tests exercise this complete path. Initial full
run was 228/229: all route checks completed, but late static destruction crashed
in Quick repaint. GDB traced it through target destruction → source epoch → Qt
style; closing-down notification suppression fixes the production boundary.
Final full suite passes **229/229 tests in 16.61 seconds**, including actual
production-audit process exit.

Real X11 on isolated loopback UDP port 14697/system 231 verifies six native Quick
values, steady yellow altitude coloring, red custom warning banner, row editing,
Save XML and close/reopen preservation. Ending the sender removes stale values
and coloring while retaining a truthful unavailable status. The output-monitor
capture contains 160000 s16 samples, peak 32768 and RMS 3410.84 during repeated
test warnings (not a human intelligibility assessment). No hardware write or
real vehicle operation was used. Final-build X11 also verifies current values
in the editor, persisted threshold 15 after restarting the application, physical
disconnect clearing Quick immediately, reconnect restoring color/values, and
normal exit 0 while the sender is still active.
Evidence: `/tmp/apm-warning.2r8mcV/`.

## Quick regression follow-up, 2026-09-05

User-reported small text came from the initial fixed 24px implementation. The
first adaptive fix passed most tests but the real theme overrode `setFont()` with
11px; its dense-grid fit also used estimated rather than actual row geometry.
Both are now fixed. Tests exercise grow/shrink, 1-to-12-cell layouts, long
coordinates/descriptions/units and repeated warning/value updates with and
without the exact production ancestor-font rule.

The missing DistToHome producer was HOME_POSITION acquisition for a late-joining
GCS. The source now emits a rate-limited read intent when heartbeat/position are
fresh and Home is absent. MainWindow rechecks exact target/source/physical epochs
and single-vehicle route eligibility, then sends REQUEST_MESSAGE(242) through
VehicleCommandService. It never sets Home or assumes a missing distance is zero.
See `WARNING_TELEMETRY_CATALOG.md` for cadence and compatibility limits.

Final Qt5/audio build and **234/234 tests pass (57.18 seconds)**. A synthetic
peer proves request/response -> 11.12 m -> stale dash -> recovery and normal
exit 0. A read-only ten-second capture of the user's actual network SITL
`192.168.0.43:14556 -> UDP14550`, system/component1/1, sees 977 packets including
fresh position and zero current in both SYS_STATUS and BATTERY_STATUS, but no
HOME_POSITION. Final X11 against that same SITL receives Home after the new
modern request and displays current **0.00 A**, distance about **0.01 m**, and
clearly larger/smaller text at 1120x720 -> 1600x950 -> 1120x720.
Independent TLOG parsing at the checkpoint sees one HOME_POSITION, 314 global
positions, 210 SYS_STATUS and 210 BATTERY_STATUS with current0 and zero BAD_DATA.
Evidence: `/tmp/apm-quick-home.NlMkar/`; personally inspected `live-quick-small.png`,
`live-quick-large.png` and `live-quick-shrunk.png`. The final live-SITL window is
left open for the user with isolated settings. Claude TCP c200/c150 and Codex
cross-review are recorded; no real flight, arming, mode or parameter write was
performed for this verification.
