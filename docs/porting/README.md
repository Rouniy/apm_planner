# Mission Planner 10 to APM Planner 3.0 parity control

`MISSION_PLANNER_SCREEN_PARITY.tsv` is the acceptance ledger for the Qt/CMake
reimplementation. It is based on the current reference tree at
`/home/alex/src/MP/MissionPlanner`, not on an older WinForms release.

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
