# Translation / RESX Editor

Verified: 2026-09-06. Functional Qt5 slice; strict cross-platform/visual parity
is not claimed. Developer Tools now has31/32 working actions; only SFTP Logs
remains unavailable. This adds no SETUP page or fixed TOOLS menu entry.

## Reference and scope

Primary: MP10 `Services/ResxTranslationService.cs`,
`ViewModels/TranslationEditorViewModel.cs`, `Views/TranslationEditorWindow.axaml`
and its code-behind, plus `ResxTranslationEditorTests.cs`. The original WinForms
ResEdit only resolves historical import identity. This is a .NET RESX resource
editor, not the Qt application's .ts/.qm localization pipeline. It performs
no network or vehicle operation and is available disconnected.

The existing TranslationResxEditorButton invokes actionTranslationResxEditor.
MainWindow owns one real modeless TranslationEditorWindow,1400x820/min940x600.
Source/culture Load, five-column editable table, search/missing/export filters,
revert, output choice, export, resume HTML import, all-row CSV clipboard and
owned cancellation are wired. Unsaved/busy Close defaults to Cancel; application
Close waits for the editor decision and worker completion. Cancelling application
Close cannot cause a subsequent standalone editor close to close the application.

## File and model contract

- Bounded recursive RESX discovery skips .git/.backup/bin/obj/translation and
  linked children. Neutral resources exclude known culture suffixes. String-only
  data, no DTD, first value/comment and last duplicate key, exact MP10 translatable
  suffixes plus all Strings.resx keys, localized comment precedence and missing
  translation fallback are preserved. Bad individual resources produce warnings.
- Windows-style case-insensitive path identity and case-sensitive keys are
  separate. ASCII path ordering explicitly uses uppercase ordinal comparison:
  Qt lowercase ordering otherwise moves ConfigAC_Fence before ConfigAccelerometer.
  Unicode case identity remains Qt simple folding, not exact .NET ordinal casing.
- Filters never reduce export/CSV snapshots. The loaded culture stays pinned even
  when the combo changes. Missing/existing/export/modified are distinct states.
  Ordered HTML duplicates keep first insertion position and last value; legacy
  .resources/basename aliases apply only to a unique row, assigned at most once.
- All edited strings can be multiline and exceed32767 characters. Enter inserts
  a newline; Ctrl+Enter or Tab commits (tooltip explains this).
- Export generates sparse localized RESX, including valid empty resources to
  remove stale translations, plus an all-row output.html. Consent explicitly says
  existing non-string resources and unloaded keys are not carried forward.
  This replaces selected output files only, never the neutral source files.
- Every existing destination, including output.html, is backed up under
  .backup/UTC-yyyyMMdd-HHmmss-guid8 before the first replacement. Publication is
  per-file QSaveFile atomic, direct-write fallback disabled, with path/stat/SHA256
  checks. Cancellation/failure retains separate actual backup/publication receipts.
  This is not a group transaction, filesystem lock or durable recovery journal;
  hostile concurrent filesystem writers remain outside the guarantee.
- XML matches .NET UTF8/no-BOM, lowercase declaration, two-space indentation,
  CRLF/CR-to-LF values/comments, and no trailing newline. Header normalization
  never rewrites literal encoding="UTF-8" inside translations. HTML preserves raw
  CR, implements WebUtility-style Latin1/astral numeric entities and HTML4 decode;
  CSV is fully quoted CRLF with escaped quotes.

Workers own immutable value snapshots and cancellation/progress atomics, never
GUI objects. QtConcurrent/QFutureWatcher bridge results on the GUI thread;
revision and lifetime fences retain the previous grid after failure/cancellation.

Limits:20k files/directories, depth64,200k loaded/import entries,16MiB per RESX,
100MiB resume/text and256MiB aggregate. Discovery also caps200k directory entries.
Selected linked roots, child links, unsafe/reserved Windows names, ambiguous
case-colliding output paths and concurrent destination changes are refused.
These deliberate bounds/path restrictions are stricter than MP10.

## Culture catalogue and independent oracle

`files/resx/dotnet10-cultures.json` is an English display-name snapshot generated
from the actual MP10 service linked to local .NET10.0.11 with en-US current/UI
culture. Its851 sorted entries are augmented with seven actual GetCultureInfo
names/display names: zh-CN/TW/HK/MO/SG/CHS/CHT, then sorted identically:858 total.
The Qt executable embeds the data and requires no .NET runtime.

Linux ICU enumeration omits those accepted Chinese aliases. Actual MP10 Load
on its own source tree returns134 resource files/3188 rows, including924 Chinese
rows across45 localized files wrongly treated as neutral. Qt intentionally fixes
this:89 files/2264 rows, zero warnings. After excluding only those45 erroneous
localized paths and mapping null comments to empty QString, **all2264 ordered
entries, source/translation/comment/existing fields match the actual MP10 output**.
Do not infer Windows NLS behavior from this Linux result.

Actual .NET export and Qt probe export of the same Unicode/multiline sparse
fixture are byte-identical for both RESX files and output.html. Source trees
are read-only inputs; export evidence is confined to temporary directories.
The oracle sources, linked primary service and goldens are under
`/tmp/apm-resx-oracle.F6cI2I/` (build2.log, Program.cs, ResxOracle.csproj,
realtree-ru.json, entries.json, net-output). `resx_translation_probe` supports
`<source> <culture>` and `export <output> <culture> <entries.json>`.

## Verification and remaining gates

Evidence: `/tmp/apm-resx-editor.7ctTCg/`:

- Qt5/audio configure passes (oracle directory qt-configure.log); final build4.log
  passes. Earlier build failures were Qt5 QPointer upcast and QStringList test
  initialization, corrected without changing semantics.
- full.log:285/285,49.71s, including the production navigation audit.
- repeat.log: all three new suites pass ten consecutive repeats,8.84s.
- realtree-qt.json/realtree-equal.txt:89/2264, zero warnings, ordered equality true.
- qt-output: recursive byte comparison against net-output has no differences.
- x11.log: actual application --translation-editor-audit exits0, zero failures.
  It opens the real Developer route offline, proves singleton window, actual
  source picker Cancel/accept, filter-independent CSV, pinned-culture export,
  default-Cancel consent, exact backups, unchanged neutral source, unsaved
  application-close Cancel, standalone Close, reopen and clean shutdown.
- Root visually inspected consent.png and consent.png.window.png: readable
  complete output plan/warnings, five-column table and controls, not a placeholder.
- window-x11.log: full window suite passes on the native X11 backend.

Three Codex streams provided core/model/window plus independent lifecycle review;
root integrated routes, oracle/probe, catalog and runtime validation. Claude TCP
c291/c293/c295 reports were fully read and SHA256 checked. His Chinese suffix gap and
whole-document encoding rewrite were fixed; raw-enumeration ordering and proposed
Unicode casing equivalence were corrected from actual .NET evidence. Final frozen
review c295 found no blocker (8456 bytes, SHA256
5cb8424578d30466c3ee90b50041ed534891cd9e505a1b537c198a805f624e98).
Root's subsequent ASCII underscore-order correction is independently pinned by
the complete real-tree comparison and a dedicated load/export regression. No MCP used.

Still open: automated MP10 screenshot diff/min-width/HiDPI, Windows/macOS package
and native file-dialog evidence, exact non-ASCII .NET path identity/order, catalog
refresh across future .NET/ICU versions, large/slow network-share UX and richer
scrollable partial receipt presentation (current status shows counts/first paths).
English catalog labels and multiline-cell key behavior are explicit GUI deviations.
The complete MP10 port remains incomplete; Settings follows the remaining SFTP tool,
and Swarm stays last.
