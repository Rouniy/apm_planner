# Mission Planner 10 to APM Planner 3.0 parity control

Start with `CURRENT_STATE.md` for the active task, verified checkpoint and
immediate implementation order. `PORTING_DEVIATIONS.tsv` keeps functional gaps
separate from GUI differences so broad usability work is not confused with the
later visual-polish pass. Repository-wide agent/build rules are in `AGENTS.md`.

`MISSION_PLANNER_SCREEN_PARITY.tsv` is the acceptance ledger for the Qt/CMake
reimplementation. It is based on the current reference tree at
`/home/alex/SRC/MP/MissionPlanner`. The separately cloned original WinForms
tree at `/home/alex/SRC/MP/Oroginal/MissionPlanner` is the authoritative
behavioral reference whenever the current Mission Planner 10 port is
incomplete or ambiguous; fixes found during comparison may also be applied to
our port.

Statuses have strict meanings:

- `not-started`: no usable Qt implementation exists; this blocks product parity.
- `in-progress`: implementation exists but the stated acceptance evidence is incomplete.
- `partial`: an older APM Planner surface covers part of the workflow, but reference behavior or
  layout is missing; this also blocks parity.
- `complete`: functional tests and a visual/geometry comparison prove the row against the current
  reference implementation.
- `remove`: an intentional omission approved by the user, with a rationale and replacement. No
  feature may be assigned this status merely to make the application compile.

The product is not complete while any row is `not-started`, `in-progress` or `partial`. A source
file compiling is not parity evidence. Every completed screen requires its behavior tests plus a
render captured at the reference window's minimum size and at the normal 1280x800 size.

The map backend is deliberately represented by several rows rather than treated as the product.
SETUP, CONFIG, parameter editors, component-specific settings, DATA, PLAN, SIMULATION, HELP and
TOOLS are all required parts of the same acceptance gate.

The exact WinForms behavior can be checked directly in
`/home/alex/SRC/MP/Oroginal/MissionPlanner`. For example, PLAN's `Insert Wp`
is one parent item with both a direct click and the `At Current Position`
child; the Qt implementation deliberately preserves those object names and
split semantics. Safe rejection of an unknown/non-finite vehicle position is
the documented Qt deviation from the original ability to create a `0/0`
waypoint.
