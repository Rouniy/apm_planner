# APM Planner 3.0 agent instructions

Read these files before changing the port:

1. `docs/porting/CURRENT_STATE.md` — current objective, verified state and immediate work queue.
2. `docs/porting/PORTING_PLAN.md` — fixed architecture and acceptance rules.
3. `docs/porting/MISSION_PLANNER_SCREEN_PARITY.tsv` — authoritative screen/function inventory.
4. `docs/porting/PORTING_DEVIATIONS.tsv` — known functional and GUI gaps kept separate from implementation status.

## Product objective

Reimplement Mission Planner 10 functionality and recognizable UI as **APM Planner 3.0**, using Qt and CMake for Linux, Windows and macOS. This is a fresh port: do not preserve the legacy APM Planner plugin ABI, old settings/layout migration or obsolete class names. Preserve Mission Planner 10 class, view and widget names when the concepts correspond.

Build a usable application before spending time on small visual differences. Work in broad vertical slices that connect navigation, UI, application services, persistence/transport and tests. Record remaining behavior and GUI differences explicitly; do not hide them by marking incomplete work complete.

## Fixed decisions

- The visible application version is `3.0.0`; do not add a second `3.0` product suffix.
- Use Qt Widgets/C++ for the shell, dense editors and core services. QML is supported for trusted in-process user plugins and suitable dynamic views through the documented broad API. There is no plugin sandbox.
- DATA and PLAN use ordinary `QWidget`/`QSplitter` layouts. Do not add KDDockWidgets or floating `QDockWidget` architecture there.
- Map backends are user-selectable and must use the same canonical tile cache. Backend-specific private caches are forbidden.
- QGroundControl and Hermes/GTU are implementation references. Mission Planner 10 is the functional/UI reference; the original WinForms tree resolves ambiguous behavior.
- Prefer Qt cross-platform facilities. Do not introduce a platform-only implementation when Qt provides the required behavior.

Reference trees:

- Mission Planner 10 port: `/home/alex/SRC/MP/MissionPlanner`
- Original Mission Planner: `/home/alex/SRC/MP/Oroginal/MissionPlanner`
- QGroundControl: `/home/alex/SRC/qgroundcontrol`
- Hermes/GTU: `/home/alex/SRC/AgroSky/GTU`

## Work discipline

- The coordinating agent schedules every configure, build and test process. Delegated agents must not configure or compile unless the coordinator grants an explicit, command-scoped build lease. A lease names the exact command, requires the preflight below, and must be released before any other build starts.
- Run only one build process globally across the workspace at a time, with at most `-j12`. Never use bare `--parallel`, bare `-j` or an equivalent unbounded job flag; always provide the numeric limit. Do not modify source files while the leased build is running.
- Immediately before **every** configure or build, run this exact command by itself:

  ```sh
  pgrep -af '(^|/)(cc1plus|clang\+\+|g\+\+|c\+\+)( |$)' || true
  ```

- Never start a build while another compiler/build is active. Only one build lease may exist at a time; agents without that lease remain read-only with respect to build products. Tests run from the coordinating agent unless their exact command is separately leased.
- Preserve unrelated user changes in a dirty worktree.
- Use thematic commits after a verified functional slice. Update `CURRENT_STATE.md`, the parity ledger and the deviations ledger in the same slice.
- Do not delete old files merely because they look unused. Delete them only after their user-visible replacement is wired and verified.
- If Claude is used, do not call `mcp__claude__Agent`. Use headless Claude Code through Bash, for example `claude -p "<task>" --output-format text`, in the intended working directory. When coordinating through the local TCP bridge, use explicit file claims, cross-review, and the same build-lease rule.

## Current execution order

1. Keep the completed PLAN right-column audit as the baseline: its direct MP10 controls are all represented, while seven named advanced actions remain explicit functional gaps. Do not spend the main stream on its accepted bottom stretch.
2. Concentrate on SETUP and port the required pages, plugins and tools in high-value functional groups, starting with Advanced Tools, Developer Tools, Terminal and the trusted QML plugin manager.
3. Keep functional differences and GUI differences as separate entries in `PORTING_DEVIATIONS.tsv` so later polish is deliberate.
4. Resume remaining DATA/PLAN/CONFIG/SIMULATION/HELP parity after the usable SETUP surface is established.

Do not reinterpret this order as a request for pixel-perfect polishing. The current milestone is a broadly working program; small geometry, color and wording differences are a later pass.
