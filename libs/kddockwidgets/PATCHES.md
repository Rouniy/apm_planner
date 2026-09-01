# KDDockWidgets vendor record

- Upstream: <https://github.com/KDAB/KDDockWidgets>
- Tag: `v1.4.0`
- Commit: `9a784f122229d1b575c1ab69cc77abc9180a688c`
- Imported frontend: QtWidgets only
- License selection for APM Planner 3.0: GPL-3.0-only

The C++ sources and resources in this directory are unchanged from the recorded
upstream commit. The root upstream build system was replaced by the small static
CMake target in this directory so the application builds only the QtWidgets files
and does not acquire examples, tests, Python, documentation or QtQuick artifacts.

Upstream was re-audited through tag `v2.4.1` (released 2026-07-20). It still
supports Qt 5, but its QtWidgets frontend requires the matching Qt private
development modules on every platform and version 2.x is a layout-engine
architecture rewrite. Updating the vendored source is therefore a separate
dependency migration and packaging gate, not an in-place source overlay.

Hermes/GTU was used as an integration reference. Its `docks_export.h` change is
not applicable to this standalone static target. Its change of the global
`LayoutSaver` default is intentionally not applied: APM Planner's `DockHost` must
request `RestoreOption_RelativeToMainWindow` explicitly and test that behavior.

Future source patches must be listed below with their upstream issue/commit,
rationale and the APM test that covers them.

## Source patches

None.
