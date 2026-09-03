# Mission Planner 10 OSD route audit

Updated: 2026-09-02. This is a read-only implementation handoff. It separates
two Mission Planner routes that currently share the old Qt `OsdConfig` widget
but have different purposes.

## Route split

- SETUP `OSD`, ID `ConfigHWOSDView`, is the legacy MinimOSD telemetry helper.
  It belongs after Optical Flow, requires a connection and is gated by
  `displayOsd` in MP10. The Qt route order and purpose are already close, but
  its widget/class identity and transaction feedback are incomplete.
- CONFIG `Onboard OSD`, ID `ConfigOSDView`, is a separate onboard layout
  editor after Extended Tuning. Its phase-one Qt page now requires a connected
  target and complete parameter list, while MP10's persisted `displayOSD` gate
  remains future work. The former wrong `OsdConfig` mapping stays removed.
- Developer Tools `OSD Video — Telemetry Overlay` and PLAN `OSD Color` are
  independent functions and must not be folded into either page.

The Qt port does not yet have the shared DisplayView profile service. Both
profile gates remain tracked deviations.

## SETUP ConfigHWOSDView

The exact MP10 surface is a title `OSD`, intro `MinimOSD telemetry helper.`,
`Enable Telemetry`, a status line and the note `You only need to use this if
you are having issue with your OSD not updating.` The original also shows
`files/images/devices/MinimOSD.jpg`; the local image is byte-identical to both
reference trees.

The action writes value `2` Hz to exactly 24 existing parameters:

```text
SR0_EXT_STAT SR0_EXTRA1 SR0_EXTRA2 SR0_EXTRA3
SR0_POSITION SR0_RAW_CTRL SR0_RAW_SENS SR0_RC_CHAN
SR1_EXT_STAT SR1_EXTRA1 SR1_EXTRA2 SR1_EXTRA3
SR1_POSITION SR1_RAW_CTRL SR1_RAW_SENS SR1_RC_CHAN
SR3_EXT_STAT SR3_EXTRA1 SR3_EXTRA2 SR3_EXTRA3
SR3_POSITION SR3_RAW_CTRL SR3_RAW_SENS SR3_RC_CHAN
```

`SRx_PARAMS` is not written. The current `OsdConfig` hardcodes component 1,
fires independent writes and has no busy/progress/failure/missing-parameter
state. Replace it with `ConfigHWOSDView`/`ConfigHWOSDViewModel` over one
exact-target batch of the parameters present in the committed snapshot.

## CONFIG ConfigOSDView

The current MP10 view contains `Refresh Params`, Screen selection, `Enable
All`, `Disable All`, `OSD 5/6 Tuning Slots…`, status, a left canvas and a
scrolling item editor on the right. Screen items are discovered by
`^OSD(\d+)_(.+)_EN$` and exist only when `_EN`, `_X` and `_Y` are all present.

The full original editor supplies Settings and Screen 1–6 tabs, staged `Write
customization`, `Auto write on leaving`, `Refresh`, `Discard all changes`,
global/screen/item option editors, copy/paste/clear layout, names versus glyph
captions and SD/HD/decreased canvas modes. Preserve the safer original staged
semantics in Qt: write only changed values, accept them only after ACK, warn
before refresh loses dirty state, and restore accepted values on Discard.

The implemented phase-one Qt page discovers complete EN/X/Y item triplets,
preserves staged edits across an identical snapshot, offers screen selection,
enable/disable all, numeric coordinates and a draggable 30x16 preview, and
submits dirty fields as one exact-target batch. Refresh warns while dirty;
timeout and per-item cancellation keep editing locked until the parameter
service emits terminal batch completion. Empty/offline states remain visible.
Full Settings tabs, glyph atlas, SD/HD geometry modes, item backgrounds,
copy/paste/clear, auto-write and tuning slots remain later phases.
The production route was also exercised on real X11 with a local UDP target:
Screen 1 rendered two parameter-backed items and their editors, and the
application closed cleanly. Reference screenshot comparison and a physical
OSD write remain outstanding.

Parameter editor rules include `OSD_FONT`, `OSD_UNITS`, `OSD_SW_METHOD` enums,
`OSD_OPTIONS` bitmask, integer offsets and X/Y, boolean EN/ENABLE, tuning-slot
type choices and metadata-backed numeric fallbacks. Incomplete item triplets
remain ordinary screen parameters rather than draggable items.

### Canvas contract

- SD grid: 30x16, NTSC boundary at row 13, normal cell 24x36.
- HD grid: 60x22, DJI area 50x18, normal cell 18x27.
- Decreased cell size: 12x18 for either mode.
- `clarity.png` is a 207x303 atlas of 256 glyphs, each 12x18 with one-pixel
  separators.
- Item position is `(X - captionXOffset) * cellWidth, Y * cellHeight`; selected
  items receive a yellow rectangle and dragging preserves the pointer offset.

A Qt `LayoutControl : QWidget` using `QPainter` and mouse events fits this
dense editor better than QML.

## Screens 5/6 tuning slots

This uses MAVLink 2 messages `OSD_PARAM_CONFIG`/`_REPLY` (11033/11034) and
`OSD_PARAM_SHOW_CONFIG`/`_REPLY` (11035/11036), not PARAM_SET. Read screens 5
and 6, slots 1–9, for 18 requests at 20 ms spacing with a 3-second timeout.
Correlate the unique request ID plus immutable link/system/component. Names
must be 1–16 printable ASCII bytes with NUL padding; a full non-NUL 16-byte
reply is valid. Types 0–7 are writable; 8 is the sentinel. Sequential writes
stop on error or timeout, and target change/cancel/destruction settles every
pending operation. The vendored dialect contains these messages, but their IDs
require MAVLink 2.

## Implementation sequence

1. Replace only SETUP `OsdConfig` with exact `ConfigHWOSDView` and one
   result-aware 24-parameter batch.
2. **Implemented phase 1:** add `ConfigOSDView`, `ConfigOSDViewModel` and pure
   OSD model; parse snapshots, provide screen/item controls, a basic 30x16
   drag canvas and staged Write/Discard/Refresh.
3. Add full canvas, atlas and global/screen/item editor parity.
4. Add the exact-target tuning-slot service/window.
5. Add auto-write lifecycle, armed/dirty warnings, profile gates, screenshots
   and native-platform evidence. Remove old `OsdConfig` only after both routes
   have verified replacements.

Tests must lock the exact legacy 24-name set/value, route separation, item
triplet parsing, staged/target-revision behavior, canvas conversions, atlas
slices, tuning message packing/correlation/failure and safe page destruction.
