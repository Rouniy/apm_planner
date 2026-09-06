# Flight Log Index

Reference: MP10 `Services/LogIndexService.cs`, `Views/LogIndexWindow.axaml`,
`ViewModels/LogIndexViewModel.cs`, `Views/LogBrowseView.axaml.cs`,
`ExtLibs/Utilities/DFLog.cs` and `DFLogBuffer.cs`. The original WinForms LogIndex
and LogMap clarify history, but their wildcard deletion and network thumbnail
requests are not copied.

## User workflow

The existing Developer Tools **Flight Log Index** action opens one real modeless
`LogIndexWindow`, even offline. The retained Logfile Plot toolbar and each
`LogAnalysis` browser also expose Index. All routes share the same application
window; immediate close/reopen cannot resurrect a deletion-pending instance.
Double-click opens the selected file in a new, independently owned working
`LogAnalysis` browser. This preserves a useful APM Planner module, not an empty
stand-in for MP10's different LogBrowseWindow.

The 1380x760 window (minimum900x520) starts scanning the configured DataFlash
directory on first show. Qt has a separate configured TLOG directory, so it has
an additional shortcut; Custom Directory, Refresh and Cancel remain available.
Paths are displayed canonically, including an ordinary selected directory
under a platform ancestor alias such as macOS `/var` → `/private/var`.

All13 reference columns are present: Map, Date, Directory, Frame, Aircraft/sysid,
Duration, Name, Size, Home, Time in air, Distance, CAM and Read warning. Numeric
fields sort numerically. Ctrl/Shift multiselection sums flight time/distance;
durations support multiple days. The earlier inventory's requirement for a
search box was inaccurate: the active MP10 window/ViewModel has no such control.

## Bounded local processing

Recursive case-insensitive BIN/LOG/TLOG discovery excludes symlink descendants
and bounds files20000, directories20000, depth64 and directory entries200000.
The initial root may have an ordinary ancestor alias, but a directly linked
selected root is refused. All subsequent operations pin canonical paths.

Scan runs outside the GUI with a dedicated pool of1–4 workers, at most four
outstanding file tasks. Cancellation callbacks are serialized; progress is
coalesced by the GUI. A slow file can leave other workers idle until the current
batch finishes. Per-file failures retain recovered metrics with warnings.

DataFlash reads raw bounded records and preserves source bytes. TLOG uses the
shared streaming timestamped MAVLink reader, excludes sys255 and selects the
first HEARTBEAT/GLOBAL_POSITION_INT system. GPS fix, armed state, camera messages
and SITL marker follow the reference. Home is the first accepted coordinate;
the reference jump filter, one-second distance cadence and time-in-air rules
apply to every accepted point. Uniform midpoint-to-even sampling retains at
most4000 preview points; larger tracks require a stamp-checked additional pass.

DataFlash first finds a GPS/GPS2/GPSB UTC anchor and freezes MP10's integer
millisecond offset. Each sample then follows TimeMS → TimeUS → T clock
precedence. Flight intervals stay relative to boot time, avoiding both mixed
UTC/relative epochs and precision loss from subtracting large epoch doubles.
This clock-discovery prepass can read the whole file if no anchor exists; a
late anchor remains consistent instead of repeating MP10's limited-prepass
quirk. Display dates use Qt's millisecond precision. Old-format GPS TimeMS
wrapping at a GPS-week boundary retains the reference backward-clock behavior.

Thumbnails are240x140 JPEGs with exact `<full log name>.jpg` sidecars, source
mtime, route/grid/start/end rendering and no network requests. Up to12 tiles
come from the current supported provider's canonical shared cache. The adapter
uses `PureImageCache::sharedTilePath` with bounded reads, not the historical
singleton cache or `GetImageFromCache` (which explicitly updates access time).
Ordinary filesystem reads can still update OS-managed access times.

Fresh valid sidecars are reused; invalid fresh sidecars are preserved, with an
in-memory fallback and warning. Stale sidecars use QSaveFile, a precommit mtime
and source/companion rechecks. Decode limits apply before decompression. A scan
holds at most64MiB of encoded previews; the view lazily decodes visible rows,
keeps at most128 pixmaps and can reload matching stamped sidecars. At the budget
limit a log without a usable sidecar may lack a preview; its metrics remain.
JPEG decoding of visible rows is bounded but still occurs on the GUI thread.

## Deletion and lifetime

Delete Selected first prepares an immutable plan, then displays its complete
exact path list in an asynchronous default/Escape-Cancel confirmation. Only the
selected source, its `.jpg`, and a TLOG's paired same-basename `.rlog` qualify;
no wildcard or similarly named sibling is deleted. Any unsafe selected entry
refuses preparation rather than silently narrowing consent. Execution rechecks
each entry and can report partial completion; only actually removed source
rows disappear. Source is removed before companions. Cancellation can leave
companions after a source deletion and the result reports that limitation.

The operator must stop active writers first. Size/mtime, canonical ancestry and
available root birth-time checks are not file locks or cryptographic identity:
same-stamp replacements and final-check-to-remove/commit races remain. Root
identity detection is weaker on filesystems without birthTime. Symlink and
changed companion substitutions fail closed, including companions that appear
only after scanning. Repointing an admission alias does not retarget a prepared
canonical plan. Deletion is permanent, not a move to Trash.

Closing during a worker operation asks before cancellation and waits
asynchronously for that operation. Application destruction cancels local work;
workers capture values/shared state, not widgets. There is no durable job
history, undo or automatic rollback.

## Acceptance and remaining parity

Final verification: Qt5/audio configure and build, focused9/9 (35.73s),
full262/262 (47.36s), ten repeats of each of4 Index suites (5.69s), and actual
X11 exit0/zero audit failures. Root inspected the left/right table and full
deletion warning screenshots. Evidence directory: `/tmp/apm-log-index.rK5jYK/`;
CURRENT_STATE.md lists the final filenames. No network SITL was changed.
`logindex_probe <file>` is a read-only diagnostic that analyzes exactly one
source, writes JSON only to stdout and never scans directories or creates
sidecars. The runtime audit uses isolated settings and temporary logs, no
network SITL subscription, and exercises actual application navigation.

Real-file numeric evidence: `/home/alex/SRC/ArduPilot/logs/00000002.BIN`,5275648
bytes, SHA256 `16726aaea629c615e4828b72217c08240552061bba7d0eeff2ac879149cf6d5a`.
Independent vendored-pymavlink and Claude raw-Python calculations agree with Qt:
ArduCopter/sys0/CAM0,1210 accepted points, duration242.800341s,
time-in-air191.599996s and distance552.7791954304238m, Home
(-35.362938,149.165085,584.09m). Qt displays UTC2026-08-31T15:44:30.000Z;
the reference clock additionally retains252 microseconds below display precision.
The file's incomplete40-byte final record remains an explicit read warning;
the diagnostic exits1 for that warning, not for a metric mismatch. Original
size, mtime and SHA256 remained unchanged. The initial per-GMS implementation
would have yielded552.835595m; the discovered clock defect is regression-tested.

Reference screenshot-diff, native Windows/macOS, large-directory GUI latency,
broader firmware/dialect and historical-log evidence remain. The current Qt
MAV_TYPE name table follows its older vendored dialect. Historical GPS leap
seconds are selected by log date rather than MP10's current-day leap count.
Functional and GUI limitations remain separate in PORTING_DEVIATIONS.tsv.
