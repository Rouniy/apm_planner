# APM Planner 3.0

APM Planner 3.0 is a cross-platform Qt/CMake ground-control station for
ArduPilot. The current development target is functional and visual parity with
Mission Planner 10 while retaining a native desktop application for Linux,
Windows and macOS.

The port is under active development. A successful build does not yet mean that
every Mission Planner workflow has reached parity; progress and acceptance
evidence are tracked in
[`docs/porting/MISSION_PLANNER_SCREEN_PARITY.tsv`](docs/porting/MISSION_PLANNER_SCREEN_PARITY.tsv).

## Architecture

- C++17 core and Qt Widgets desktop shell.
- QML for extensible UI and trusted in-process user plugins.
- Multiple selectable map widgets backed by one shared tile cache.
- Qt/CMake is the only supported build path; the old qmake/plugin ABI is not
  supported.
- APM Planner 3.0 uses a fresh settings and user-data namespace. Legacy APM
  Planner profiles are not imported.

See [`docs/porting/PORTING_PLAN.md`](docs/porting/PORTING_PLAN.md) for the port
strategy and [`docs/porting/QML_PLUGINS.md`](docs/porting/QML_PLUGINS.md) for the
QML plugin contract.

## Build

Required tools and libraries:

- CMake 3.21 or newer and a C++17 compiler;
- Qt 5.10 or newer, or Qt 6, with Core, Gui, Widgets, Network, Svg, Xml, Sql, SerialPort,
  Quick, Qml, QuickWidgets and PrintSupport;
- SDL2.

Qt 5 is currently the reproducible baseline. Qt Multimedia and TextToSpeech
are required by default. DataVisualization remains optional. CMake fails early
if the audio development modules are missing; existing build directories should
set `-DAPM_REQUIRE_QT_AUDIO=ON` explicitly to override an older cached opt-out.
A runtime speech engine and an unmuted audio output are also required.
Only intentionally silent developer builds should use
`-DAPM_REQUIRE_QT_AUDIO=OFF`: **Test Speech** and spoken alerts cannot work
without TextToSpeech, and such builds are not speech-parity evidence.

On Debian/Ubuntu with Qt 5, install the audio development modules and the
Speech Dispatcher plugin:

```sh
sudo apt install qtmultimedia5-dev libqt5texttospeech5-dev \
  qtspeech5-speechd-plugin speech-dispatcher-espeak-ng
```

On Debian/Ubuntu, the Qt 5 development libraries do not automatically install
the QML runtime modules required by user plugins. Install at least:

```sh
sudo apt install qml-module-qtqml qml-module-qtquick2 \
  qml-module-qtquick-controls2
```

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DAPM_QT_MAJOR=5
cmake --build build --parallel 12
```

Run the application from the source checkout:

```sh
./build/apmplanner3
```

Run the test suite:

```sh
ctest --test-dir build --output-on-failure
```

Check the installed speech engine on the desktop (the second command speaks):

```sh
./build/speech_backend_probe
./build/speech_backend_probe --speak
```

This diagnostic is built with `BUILD_TESTING=ON` but is intentionally not an
automatic CTest: headless CI cannot prove working speakers or runtime speech
plugins. Also verify CONFIG → Planner → Enable Speech → Test Speech.

Do not start concurrent builds of this repository. Use one build process and
limit it to at most 12 parallel compiler jobs to avoid exhausting memory.

For an installed or packaged build, disable source-tree resource lookup:

```sh
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DAPM_QT_MAJOR=5 \
  -DAPM_ENABLE_SOURCE_DATA_FALLBACK=OFF
cmake --build build-release --parallel 12
cmake --install build-release --prefix /desired/prefix
```

Use `-DAPM_QT_MAJOR=6` for the Qt 6 diagnostic path. It is not yet the product
completeness baseline.

## Source references

The parity work uses Mission Planner 10 as the naming, behavior and visual
reference. QGroundControl and Hermes/GTU may be used for Qt-native implementation
patterns where that does not change Mission Planner behavior.

## License

APM Planner is free software distributed under the GNU General Public License,
version 3 or later. See [`license.txt`](license.txt).
