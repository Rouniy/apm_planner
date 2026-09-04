# APM Planner 3.0 — полный рабочий план порта Mission Planner 10

Этот документ задаёт порядок работы от текущего состояния до пригодного к
ежедневному использованию и затем до проверенного кроссплатформенного порта.
Архитектурный контракт находится в `PORTING_PLAN.md`, а построчный реестр
экранов — в `MISSION_PLANNER_SCREEN_PARITY.tsv`. Здесь описаны приоритеты,
зависимости, известные пробелы и критерии завершения пакетов работ.

## 1. Текущее состояние и честная мера готовности

Проверенная Linux-точка после нативного Compass/Motor-среза:

- приложение и все цели CMake собираются одним `cmake --build build-codex-qt -j12`;
- проходят 135 из 135 тестов;
- реальный X11-запуск показывает точный заголовок
  `APM Planner 3.0.0 (...) — APM Planner`;
- DATA, PLAN и SETUP открываются, карта DATA работает, SETUP OSD создаёт
  отдельный `ConfigHWOSDView`, приложение закрывается без падения;
- сохранён единый exact-target стек `(link, system, component, generation)` для
  команд и параметров, единый tile cache и доверенная QML-система расширений;
- DATA/PLAN используют фиксированные `QWidget`/`QSplitter`, а не плавающие
  `QDockWidget` или KDDockWidgets.

Это не означает готовность продукта. В реестре 127 поверхностей:

| Область | in-progress | partial | not-started | Всего |
|---|---:|---:|---:|---:|
| SHELL | 1 | 3 | 0 | 4 |
| DATA | 2 | 3 | 0 | 5 |
| PLAN | 5 | 2 | 4 | 11 |
| SETUP | 27 | 18 | 11 | 56 |
| CONFIG | 7 | 11 | 2 | 20 |
| TOOLS | 8 | 3 | 18 | 29 |
| SIMULATION | 0 | 1 | 0 | 1 |
| HELP | 0 | 1 | 0 | 1 |
| **Итого** | **50** | **42** | **35** | **127** |

Ни одна строка пока не считается strict-complete: отсутствует полный набор
эталонных screenshot-diff, native Windows/macOS и hardware/live-vehicle
доказательств. Статусы означают:

- `in-progress`: новый Qt-каркас и существенная часть поведения уже есть;
- `partial`: в основном используется старый APM Planner код либо перенесена
  только часть MP10-поведения;
- `not-started`: пользовательский workflow отсутствует либо маршрут ведёт не
  на тот инструмент;
- `complete` появится только после функциональной, lifecycle, визуальной и
  кроссплатформенной проверки.

## 2. Приоритеты

### P0 — видимая поломка, риск данных или базовый ежедневный workflow

Работа P0 выполняется первой и не откладывается ради мелкого визуального
совпадения других страниц.

Основной первый срез TOOLS/диалогов доведён через Device Operations; текущий
пользовательский приоритет — Settings/CONFIG и их функциональные vertical
slices. Оставшиеся специализированные Tools сохраняются в точном меню, но
включаются только после полноценной реализации. Пункты 3–4 остаются важными
пробелами, но их визуальная часть не должна вытеснять перенос рабочих настроек;
Qt Widgets и доверенный QML API можно сочетать по назначению.

1. Убрать пустую полосу у правой границы PLAN и зафиксировать геометрию при
   1120×720, 1280×800, 1920×1080 и HiDPI.
2. Tools → MAVLink Inspector должен открывать отдельное независимое modeless
   окно, а не выглядеть/вести себя как встроенная панель; текущий source уже
   ставит `Qt::Window`, но это надо воспроизвести на пользовательской сборке и
   заменить неоднозначный child-`QWidget` lifecycle явным window-классом.
3. Довести главный DATA HUD/OSD до видимого набора MP: вертикальная скорость
   уже приходит в модель и рисуется только малоконтрастной стрелкой VSI;
   требуется явно видимое числовое значение и проверка остальных полей.
4. Довести CONFIG → Onboard OSD после отдельного phase-one редактора: добавить
   полные Settings/Screen 1–6, glyph canvas, HD и tuning-slot workflows.
5. Обновить устаревший bundled MAVLink dialect 2022 до совместимого с MP10
   набора сообщений; иначе новые сообщения нельзя декодировать в Inspector и
   специализированных инструментах.
6. Устранить блокировки GUI при Install Firmware: сжатый manifest, bounded
   streaming, атомарный cache, parsing и serial enumeration вне GUI thread.
7. Сохранять зелёными exact-target, mission/parameter transaction и shutdown
   тесты после каждого вертикального среза.

### P1 — основная функциональная полнота обычного оператора

- полноценные DATA actions/tabs/HUD/map/video и телеметрия;
- обычное построение, редактирование, импорт/экспорт и передача миссии в PLAN;
- все часто используемые SETUP hardware pages;
- основные CONFIG tuning/parameter/fence/OSD страницы;
- Inspector и основные log workflows;
- профиль видимости страниц по типу аппарата;
- второй пригодный map backend поверх того же cache;
- расширение QML API для telemetry, mission, parameters, commands, maps,
  settings и пользовательских действий.

### P2 — специализированные, но полноценные workflow

- Antenna Tracker, FFT, OpenDroneID, secure boot/signing, CubeID;
- DroneCAN inspector/write, traditional heli, PX4Flow, REPL и MAVFTP;
- SFTP logs, mag fit, georeference, terrain maker;
- Follow Me, external guided, moving base, formation/swarm;
- NMEA/CoT output, MAVLink serial/TCP bridge и serial pass-through;
- OSD video overlay, Microdrone и translation editor; Device Operations уже
  имеет основной modeless/exact-target workflow, но ещё ждёт hardware/native
  evidence для strict-complete;
- полная Simulation и signed updater.

### P3 — release parity и очистка

- автоматические reference screenshots и pixel/SSIM gate;
- Windows/macOS native CI, packaging, signing/notarization;
- keyboard/accessibility/localization/HiDPI;
- удаление старых страниц, qmake-файлов, мёртвых ресурсов и transitional
  `SubMainWindow` только после прохождения replacement gate;
- лицензии и атрибуция всех перенесённых библиотек/assets.

## 3. Ближайшая последовательность выполнения

### Wave 0 — зафиксировать базу и не допустить регрессий

- Перед каждой configure/build выполнять точную проверку процессов компилятора.
- Только координатор запускает одну сборку; Claude и субагенты не собирают без
  отдельной передачи build lease.
- Максимум `-j12`, полный `ctest -j1` после интеграционного среза.
- Каждый независимый зелёный срез получает отдельный commit; чужие или
  несвязанные изменения не попадают в него.
- Поддерживать `CURRENT_STATE.md`, parity TSV и deviations TSV одновременно с
  кодом.

Gate: clean staged scope, `git diff --check`, корректное число TSV-колонок,
полная сборка, все тесты, X11 shutdown.

### Wave 1 — три сообщённых пользователем дефекта

#### 1A. PLAN right edge

Состояние 2026-09-03: выполнено. Внешняя пустая полоса устранена, wrapper и
content имеют единый fixed extent, а видимые Up/Down/Delete добавлены как
строчные действия поверх существующих операций и покрыты тестами.

- Снять фактическую геометрию `FlightPlannerView`, `HorizontalDockSplitter`,
  `ActionPanel`, `ActionScroller`, viewport и vertical scrollbar.
- Сравнить с MP10 `FlightPlannerView.axaml`: action column 168 logical px,
  splitter 4 px, отсутствие внешнего правого margin.
- Исправить установленную причину: `DockableView` помещает content во
  `QVBoxLayout` expanding wrapper frame, а fixed width 168 сейчас получает
  только внутренний `FlightPlannerActionPanel`. На широком окне splitter
  растягивает wrapper, оставляя пустоту справа от 168-px child.
- Зафиксировать ширину/constraint на panel wrapper/splitter node, сохранив
  допустимые 6 px внутреннего margin и ширину scrollbar внутри action column.
- Отделить допустимые 6 px внутренних отступов кнопок от ошибочного пустого
  пространства между всей панелью и границей окна.
- Исправить layout ownership/size policy; не маскировать дефект увеличением
  ширины кнопок или случайным отрицательным margin.
- Проверить все controls action panel: наличие, порядок, objectName,
  enabled/disabled reason и реальное подключение к ViewModel/service.
- Добавить отсутствующие видимые MP10 waypoint-table кнопки Up, Down и Delete:
  операции/QAction в Qt уже есть, но кнопки/колонки в текущей панели не
  создаются. Проверить selection, jump remapping и undo.
- Добавить geometry test на правую координату панели и viewport, а также
  screenshot smoke на четырёх размерах.

Gate: правая граница панели совпадает с content rect, кнопки используют ширину
viewport, scrollbar не создаёт второй пустой столбец, сохранение layout не
возвращает дефект.

#### 1B. MAVLink Inspector window

Состояние 2026-09-03: выполнена window/lifecycle часть. Каждый вызов создаёт
новое modeless top-level окно, replay использует guarded multicast; unit и
real-X11 multi-instance/close/shutdown проверки проходят. Фильтры, графики и
современный dialect остаются отдельной функциональной работой.

- Проверить оба входа: верхнее Tools menu/Ctrl+I и SETUP Advanced Tools.
- Сначала проверить текущий бинарник: source уже создаёт `QGCMAVLinkInspector`
  с `Qt::Window`, `show/raise/activate`; наблюдение может относиться к старой
  сборке либо platform-specific поведению child-`QWidget`.
- Вынести содержимое в MP10-подобные `MAVLinkInspectorView` и modeless
  `MAVLinkInspectorWindow : QWidget` с явным `Qt::Window`; не наследовать
  диалоговые Enter/Escape semantics.
- Не использовать embedded center-stack, dock или child-widget с одним лишь
  поздним `setWindowFlag(Qt::Window)` как долгосрочную модель ownership.
- MP10 открывает новый modeless Inspector на каждый вызов. Сначала можно
  сохранить безопасный singleton как P0-fix, но конечный P1 gate — независимые
  multi-instance окна.
- Закрытие каждого окна удаляет его и очищает replay attachment; для
  multi-instance log player/replay link должны хранить безопасный список
  подписчиков, а не один raw pointer.
- Окно обязано фильтровать точный link/system/component, поддерживать message
  tree/rate/fields/filter/graphs и современный 32-bit message id.
- Добавить тест top-level/window flags, multi-instance, закрытия одного из двух,
  destruction и shutdown при активном потоке live/replay сообщений.

Gate: отдельная запись window manager, отдельная рамка/перемещение, два вызова
создают два окна как в MP10, основное окно остаётся доступным, нет утечек и
падения при Alt+F4.

#### 1C. DATA HUD/OSD completeness

- Составить точную таблицу всех свойств MP10 `HudControl` и текущего Qt HUD.
- Проследить источники и единицы измерения от MAVLink/UAS до
  `FlightDataViewModel` и `HudControl`.
- Вертикальная скорость: сохранить существующий VSI, добавить читаемое
  значение со знаком и `m/s`, проверить climb/descent/zero и направление.
- Проверить heading, air/ground speed, relative altitude, target values,
  throttle, mode, link quality, GPS/fix/sats, battery 1/2/current/cells,
  waypoint number/distance, x-track/turn, wind, AOA/SSA, EKF, vibration,
  prearm/failsafe/safety и custom user items.
- Проверить контекстное HUD menu: visibility toggles, aspect ratio, swap,
  detach, user items, ground color, battery cells и video actions.
- Для каждого значения добавить deterministic model test и paint-region test;
  для живого аппарата записать screenshot с ненулевым climb rate.

Gate: значение climb rate видно без интерпретации стрелки, знак/единицы верны,
переключение target очищает старое значение, отсутствующие данные не
маскируются правдоподобным stale value.

### Wave 2 — CONFIG Onboard OSD

1. Pure model: `OSDSetting`, `OSDConfiguration`, `OSDScreen`, `OSDItem`.
2. Snapshot parser: global `OSD_*`, screen `OSD{n}_*`, draggable item только
   при полном triplet `_EN/_X/_Y`, остальные параметры в screen/item options.
3. Staged editing: dirty/original/accepted, Write only changed, Discard,
   guarded Refresh, auto-write-on-leave без write storm.
4. `ConfigOSDView`/`ConfigOSDViewModel`: Settings и Screen 1..6, enable/disable
   all, status, left canvas/right item list.
5. `LayoutControl` на `QWidget/QPainter`: SD 30×16, HD 60×22, PAL/NTSC/DJI
   markers, selection, hit test, integer drag, show names/real glyphs.
6. Copy/paste layout, clear all, screen/item options, `clarity.png` atlas.
7. OSD 5/6 tuning slots: exact-target MAVLink 2 requests 11033..11036,
   request-id correlation, 18 reads, validation, timeout/cancel, sequential
   changed writes.
8. Переключить только CONFIG route на новый редактор; удалить `OsdConfig`
   после проверки, что SETUP использует `ConfigHWOSDView`.

Gate: staged changes не пересекают target, полная parameter batch ACK/cancel,
canvas и options совпадают с эталоном, hardware/live evidence.

### Wave 3 — DATA как рабочее место оператора

- Закрыть пять отсутствующих backend actions в 5×5 Actions grid; ни одна
  видимая кнопка не должна быть inert.
- Довести Quick: выбор полей, число ячеек, цвета/единицы, persistence и
  detach/restore.
- Довести Actions, Messages, Simple Actions, PreFlight и Status/Raw Sensors.
- Реализовать полную Info Page с группами, выбором полей и единицами.
- Карта DATA: track, auto-pan, mission/proximity overlays, guided/context
  actions, vehicle icons и target isolation.
- Video: Qt Multimedia sources, MAVLink camera stream, MJPEG/GStreamer adapters,
  snapshot/record, HUD-to-video export, cancel и teardown.
- HUD: весь inventory Wave 1C, настройки и custom user fields.
- Audio vario, speech alerts и warning manager должны использовать один
  telemetry snapshot и не дублировать аварийные объявления.

Gate: offline/connected/multi-vehicle fixtures, 30+ минут live soak,
disconnect/reconnect и shutdown с активным video/logging.

### Wave 4 — PLAN как полноценный Mission Planner

Уже есть основа mission model, WPL/QGC Plan, waypoint table, map editing,
survey grid, fence/rally controller, elevation graph, unit presentation и
обычный mission transfer. Осталось:

- закончить Wave 1A и точный control inventory;
- включить Grid/graticule;
- View KML и backend-neutral KML/KMZ/GeoJSON overlay/import;
- custom XYZ tile source с безопасной конфигурацией;
- WMS/WMTS через общий provider/cache контракт;
- MAVFTP и Write Fast с cancel/progress/error и target lease;
- fence undo и оставшиеся advanced context/menu actions;
- `ImportedMapOverlayRenderer`;
- `OverlapCoverageBuilder`;
- `FaceMapView` и face-map mission builder UI;
- полная `OgcMapProvider` поддержка;
- завершить MapCacheView/MapCacheManager: quota, concurrent atomic writes,
  cancel, provider isolation и cache migration только внутри fresh namespace;
- подтвердить elevation terrain chain: local GDAL → shared SRTM cache →
  bounded background download;
- проверить все mission commands, jump remapping, alt/distance unit boundaries,
  undo/redo и armed/target guards.

Gate: создать/загрузить/изменить/сохранить/записать/прочитать Mission, Fence и
Rally; после cancel/target switch нет поздних изменений; все кнопки либо
работают end-to-end, либо явно недоступны с причиной.

### Wave 5 — SETUP, 57 поверхностей

#### 5A. Завершить уже начатые native Qt vertical slices

- ADSB: live uAvionix hardware, profile gate, screenshots, native platforms.
- Default Settings: cancellation/cache/symlink/live-network evidence.
- HW ID: реальные CAN/I2C/SPI/UAVCAN ids, export/copy parity.
- Initial Params: live ACK and vehicle-family coverage.
- Serial, Servo Output, ESC Calibration, Motor Test, GPS Order, HW CAN,
  Bluetooth, Parachute, ESP8266, Battery Monitor 2: live devices, target switch,
  native serial/USB, screenshots.
- Developer/Advanced Tools: заменить 43 disabled операции законченными
  пакетами, не включая кнопки заранее.
- Elevation Sources и Mission Command List: native/package evidence.
- SETUP OSD: live 24-write full/partial path и profile gate.
- Current Compass: parameter/priority, onboard multi-compass, Large Vehicle
  fixed-yaw и отдельный Compass/Motor slices сделаны; остаются
  physical-hardware evidence и OfflineMagFit.

#### 5B. Исправить partial legacy pages

- Setup shell/order/profile gating;
- Install Firmware и legacy firmware route;
- Frame Class/Type, Accel, Compass Legacy/Compass Motor;
- Radio Input и FailSafe; Flight Modes уже заменён общим нативным
  CONFIG/SETUP exact-target экраном и проверен через оба X11 route,
  остаются hardware/native-platform
  evidence;
- Battery Monitor 1, Range Finder, Airspeed;
- GPS Inject, DroneCAN, Joystick, Mount, Optical Flow;
- SiK Radio и Terminal.

Каждая страница переводится с прямого `UAS`/hardcoded component на committed
snapshot и exact-target transaction, получает typed validation, busy/error/
cancel states и lifecycle tests.

#### 5C. Реализовать отсутствующие страницы

- `ConfigAntennaTrackerParamView` и `AntennaTrackerUIView`;
- `ConfigCubeIDView`;
- `ConfigFFTView`;
- `ConfigOnboardReplView`, `ConfigScriptReplView`;
- `ConfigPX4FlowView`;
- `ConfigSecureView`, `ConfigSecureApView`;
- `ConfigTradHeliView`, `ConfigTradHeli4View`;
- SETUP `MavFTPUIView`;
- `NvModemView`;
- `DroneCANInspectorView`;
- `OpenDroneIdView`.

#### 5D. Antenna Tracker sequence

- pure output protocols — сделано;
- pure geometry — сделано и покрыто граничными тестами;
- один thread-confined cancellable `QSerialPort` service — сделано; setup,
  initial center, partial writes, latest-target-wins, timeout, unplug/reconnect,
  reentrant transport callbacks и teardown покрыты тестами;
- serial setup ViewModel/View, port collision/unplug/partial-write tests — сделано;
- calibration, servo range/trim/reverse/speed/acceleration validation — сделано;
- live tracker view и vehicle pointing loop — сделано; PLAN home selection остаётся;
- exact-target 25-field parameter page;
- SiK trim search и teardown — сделано; hardware evidence остаётся;
- заменить старый `AntennaTrackerConfig` только после полного route gate.

Gate для SETUP: все 53 MP10 routes классифицированы, ни одна подключённая
страница не пишет другой компонент/аппарат, visibility соответствует профилю.

### Wave 6 — CONFIG, 20 поверхностей

- ConfigView shell и parameter-loading lifecycle.
- Full/Raw Params: search, staged edit, compare, load/save, defaults,
  favorites, metadata, component selection и atomic target switch.
- Friendly Params basic/advanced.
- Flight Modes: нативный общий CONFIG/SETUP slice реализован; остаются
  reference screenshot diff, native-platform и physical-vehicle evidence.
- Plane QP Extended Tuning: нативные 17 групп/68 строк, metadata editors,
  exact-target batch и X11 route реализованы; Copter/Heli сохраняют полезный
  legacy Extended editor. Остаются physical-vehicle/reference/native evidence.
- Basic tuning и оставшиеся Copter/Plane/Rover tuning gaps.
- Fence configuration и live status.
- Planner settings: нативная страница уже воспроизводит все девять секций,
  общий DisplayView profile и restart-scoped dual Startup UDP listeners;
  точный аудит насчитывает 64 MP10 controls, из них 17 уже имеют рабочие
  нативные эквиваленты после live HUD Overlay и Speech C1 slices. Центральный
  announcer применяет MP10 gates/templates для Armed Only, Waypoint, Mode,
  Battery и Arm/Disarm. Далее — периодические Custom/Alt Warning/Low Speed/No
  Data/vario consumers, severity, общий unit service/speed, затем shortcuts,
  target-safe telemetry/identity и map overlays.
- Planner Advanced и User Defined — завершить screenshots/native evidence.
- Param Compare — полный merge/diff/apply workflow.
- CONFIG Onboard OSD — Wave 2.
- Shared MAVFTP UI: завершить streaming/burst, capability gate, exclusive
  no-replace local save и live-hardware evidence.
- Отсутствующие: Traditional Heli, FFT analysis window и Warning Manager.

Gate: изменение параметров всегда проходит typed metadata validation и ACK,
dirty state никогда не переносится на новый target.

### Wave 7 — TOOLS, 28 поверхностей

#### Основные инструменты P1

- MAVLink Inspector — Wave 1B;
- MAVLink log player: seek/speed/pause/state isolation/Inspector integration;
- DataFlash Log Download: list/selected/all/erase/cancel/export;
- Log Browse: graphs/messages/params/map/report/export;
- Terrain 3D: selected map backend, vehicle/camera path и teardown;
- QML Plugin Manager: широкая документация и API coverage.

#### Отсутствующие инструменты P2

- SFTP Log Download и Log Index;
- Offline Mag Fit;
- Photo/video GeoRef;
- Terrain Maker;
- Follow Me, External Guided, Moving Base;
- Formation Control и четыре swarm workflow;
- Serial NMEA Output;
- CoT/TAK UDP/TCP Output;
- MAVLink Serial/TCP Bridge;
- Serial Pass Through;
- OSD Video Overlay;
- Microdrone Downlink;
- Device Operations — основной modeless/exact-target срез сделан; остаются
  hardware/native evidence и визуальная полировка;
- Translation Editor;
- Tracker Home Module.

Для сетевых/serial/guided/swarm tools обязательны immutable target, явный Start/
Stop, rate limit, cancellation, disconnect и destruction tests. Для файловых
tools — Unicode paths, `QSaveFile`, bounded parsing и отсутствие GUI blocking.

### Wave 8 — SHELL, connection, profiles и navigation

- Точный MainWindow header/navigation/connection strip и shortcuts.
- Connect dialog/options: serial/UDP/TCP discovery, validation, history,
  reconnect и exact link identity.
- Backstage order, group headers, loading overlay и page restore.
- Общий JSON `DisplayView`/vehicle profile service, 11 CONFIG + 35 SETUP flags
  и синхронизация Advanced Mode — сделано; 30 SETUP-флагов уже gate активные
  фабрики, пять ожидают отсутствующие routes, также остаются profile consumers
  вне navigation и полноценный Custom editor.
- Удалить старые menu/dock ownership paths после миграции всех consumers.
- Проверить несколько links с одинаковыми sysid/compid, link removal и active
  target selection.

### Wave 9 — карты и единый cache

- Hermes/OPMap остаётся первым production backend Qt 5.
- Все роли DATA/PLAN/SIM/Log используют `IMapView`/factory, общие provider ids,
  один tile root, quota, атомарные записи и offline behavior.
- QGC backend остаётся optional Qt 6 до устранения private Qt Location API.
- Оценить MapLibre Qt как второй production Qt 5 backend; принять только после
  feature/cache/license/package gate.
- Backend switch не меняет mission/overlay state и не создаёт второй cache.
- Общие overlays: vehicle, mission, trail, fence, rally, measure, proximity,
  KML/GeoJSON и terrain.

### Wave 10 — доверенный QML plugin API

- Сохранить один shared `QQmlEngine`, in-process без sandbox по решению проекта.
- Разбить документированный API на стабильные namespaces:
  application/version/paths, active vehicle telemetry, targets/links,
  parameters, commands, mission/fence/rally, maps/overlays, settings,
  notifications/audio, logs/files и UI action/page registration.
- Добавить subscription/lifetime rules, async result objects, cancellation и
  examples; compatibility только в пределах major version.
- Plugin reload/unload обязан удалить actions, pages, connections и QML objects
  без поздних callback.
- Native core остаётся Qt Widgets/C++; QML используется для пользовательских
  расширений и изолированных presentation surfaces, а не для переписывания
  mission/transport/cache ядра.

### Wave 11 — Simulation и Help

- SITL frame/version/location download/launch, JSBSim/FlightGear, console,
  process ownership, stop/restart, installed paths и native packaging.
- Help: version/support/licenses/shortcuts.
- Updater: собственный HTTPS manifest, signed metadata, package hash/size,
  stable/beta exact platform selection и безопасный installer launch.

### Wave 12 — кроссплатформенность и release engineering

- Linux Qt 5 build/package/install smoke.
- Windows MSVC/MinGW выбранная release matrix, `windeployqt`, serial/video/QML/
  SQL plugins, installer и signing.
- macOS universal/target architecture policy, `macdeployqt`, entitlements,
  camera/network/serial permissions, signing и notarization.
- CMake presets, reproducible dependency versions, CI artifacts и license BOM.
- Native tests: Unicode paths, writable cache/config/log dirs, plugin folder,
  child processes, serial hotplug, map/video backends.

### Wave 13 — визуальная parity и удаление лишних файлов

- Эталонные сцены MP10 для clean/offline/connected/multi-vehicle profiles.
- 1120×720 и 1280×800 как обязательные; 1920×1080 и 150/200% DPI как
  дополнительные.
- Geometry ±1 px и SSIM ≥ 0.98 после маскирования живых данных.
- Проверить Emerald colors/fonts/icons, keyboard focus, tab order,
  accessibility names и translations.
- Построить include/source/resource reachability отчёт.
- Удалять старый widget/service/resource только если новый route и все
  consumers проверены; отдельно удалить obsolete qmake/installer leftovers.
- Не удалять код только потому, что он сейчас не входит в одну build target:
  сначала доказать отсутствие пользовательского workflow.

## 4. Общие критерии готовности каждого вертикального среза

1. **Reference:** зафиксированы MP10 controls, defaults, order, validation,
   transport, statuses и lifecycle.
2. **Names:** class/widget/objectName соответствуют MP10, если концепция та же.
3. **Target safety:** immutable exact endpoint; stale replies игнорируются.
4. **State:** offline/loading/ready/busy/success/error/cancel/timeout видимы и
   детерминированы.
5. **No inert UI:** кнопка либо завершает workflow, либо disabled с причиной.
6. **Units:** SI внутри; presentation conversion только на UI boundary.
7. **Threading:** GUI не блокируется; worker stop/join cooperative.
8. **Tests:** pure logic, signal/lifecycle, target switch, malformed input,
   widget inventory и destruction.
9. **Integration:** CMake application target и отдельный тест target.
10. **Runtime:** real X11/native smoke; shutdown с активной операцией.
11. **Ledger:** parity/deviation/current state обновлены честно.
12. **Commit:** один тематический commit после полной проверки.

## 5. Что сознательно не делаем

- не поддерживаем старый binary plugin ABI и старые layout/settings migration;
- не вводим sandbox для QML plugins;
- не строим DATA/PLAN на KDDockWidgets или `QDockWidget`;
- не создаём отдельные tile caches для map backends;
- не включаем inert controls ради визуального сходства;
- не блокируем широкое функциональное портирование мелкими pixel differences,
  если они не ломают layout или использование; такие отличия идут в Wave 13.

## 6. Правило актуализации

После каждого пакета координатор обновляет этот документ только если изменился
порядок/состав работ. Фактический статус отдельных экранов всегда обновляется в
`MISSION_PLANNER_SCREEN_PARITY.tsv`, намеренные отличия — в
`PORTING_DEVIATIONS.tsv`, а последняя проверенная сборка и следующий handoff —
в `CURRENT_STATE.md`.
