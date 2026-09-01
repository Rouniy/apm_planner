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

Hermes/GTU was used as an integration reference. Its `docks_export.h` change is
not applicable to this standalone static target. Its change of the global
`LayoutSaver` default is intentionally not applied: APM Planner's `DockHost` must
request `RestoreOption_RelativeToMainWindow` explicitly and test that behavior.

Future source patches must be listed below with their upstream issue/commit,
rationale and the APM test that covers them.

## Source patches

None.
