# APM Planner 3.0 — полный рабочий план порта Mission Planner 10

Этот документ задаёт порядок работы от текущего состояния до пригодного к
ежедневному использованию и затем до проверенного кроссплатформенного порта.
Архитектурный контракт находится в `PORTING_PLAN.md`, а построчный реестр
экранов — в `MISSION_PLANNER_SCREEN_PARITY.tsv`. Здесь описаны приоритеты,
зависимости, известные пробелы и критерии завершения пакетов работ.

## 1. Текущее состояние и честная мера готовности

Актуальная проверенная точка и конкретная ближайшая очередь находятся в
`CURRENT_STATE.md`: 2026-09-06, Qt5/audio,293/293 теста; Developer32/32 маршрута,
Advanced14 complete+1 partial. Исходящие typed MAVLink пакеты теперь записываются
в TLOG при активном журнале, с полным исключением SETUP_SIGNING; Anon Log
консервативно удаляет пять opaque/secret классов и неоднозначные команды.
Tlog Convert теперь имеет рабочий MATLAB Level5 export: настоящее окно,
неизменяемый вход, default-Cancel/Save As, progress/cancel и no-overwrite.
Все283 переменные реального TLOG и160 переменных synthetic совпадают побитно
с actual MP10 .NET;24 scalar extensions добавлены только в offline schema.
Full293/293 (52.13s),10 повторов4 suites и просмотренный production X11 проходят.
Это не закрывает74 отсутствующих MAVLink message types, отдельный BIN/LOG
ProcessLog и отсутствующие DATA log tabs. Далее Terrain3D/Signing/прочие Tools,
затем Settings; Swarm последний. См. `MATLAB_TLOG_EXPORT_PORT.md`.
Split DataFlash Log теперь работает с BIN/LOG, целыми записями, метаданными,
отменой и честным описанием публикации нескольких файлов без перезаписи.
Create DashWare CSV работает offline через реальные диалоги: исходный порядок
строк/колонок, точные времена без исправления скачков, mode names, quoting,
отмена и атомарная публикация одного файла. Проверены реальный BIN и большой
105МБ/7million-record log; использование памяти не растёт с длиной лога.
Download MAVFTP File реализован отдельным MP10-потоком: remote path → Save →
progress/cancel на существующем общем сервисе, с фиксацией борта до диалогов,
30-секундным deadline и атомарной записью. Он и старый браузер используют
токены владения операциями; чужие Cancel/результаты не пересекаются.
Теперь и полный MAVFTP-браузер фиксирует борт/путь/тип до асинхронных диалогов,
проверяет lease при допуске операции и очищает списки при смене цели. Подтверждения
Upload/Delete показывают цель и путь, по умолчанию выбирается Cancel.
Полный набор243/243 проходит за30.20s; production X11 проверяет127-байтовый файл,
обе смены компонента во время подтверждения без destructive FTP requests,
пересоздание страниц и явный Refresh. Сетевой SITL не изменялся.
Начало записи только по heartbeat/enabled остаётся отдельным пробелом.
Embed Defaults in APJ реализован: два file dialogs, точный выходной файл
firmware+new.apj, default-Cancel overwrite, bounded worker/cancel, проверка и
пересчёт unsigned descriptor CRC, отказ от изменения signed firmware. Независимо
проверены байты реального CubeOrange и synthetic unsigned CRC; исходники целы.
Qt5/audio full244/244 (30.22s), focused7/7 и production X11 проходят.
Organize Log Directory теперь работает через Analyze → полный список файлов →
default-Cancel Execute: TLOG/RLOG/BIN/LOG, SMALL/BAD/SITL/type/SYSID/serial,
явные companions, без перезаписи, с отменой и честным partial-report. Исправлены
ошибки классификации и serial-path оригинала. Независимая проверка семи
перемещений и удаления пустого файла сохраняет все непустые SHA-256; повторный
анализ — no-op. Full246/246 (30.12s), X11 и просмотр финального плана проходят.
Upgrade Bootloader перенесён: два target-bound default-Cancel подтверждения,
один exact command42650/p5=290876, без автоповторов, bounded5min ACK/absolute
deadline и честный неопределённый результат при потере ACK. Исправлены callback
lifetime-дефекты общих сервисов и закрытия страницы во время prepare. Полный
набор246/246 (30.41s), production X11 и просмотр финального диалога проходят;
сетевой SITL и реальные платы не изменялись.
Restore Parameters и собственная Cancel теперь работают: file/target consent,
ENABLE-first и весь файл в исходном порядке, typed reads, проверка представимости
`_ID` до сброса в0, две reservations и честные partial/uncertain receipts.
Исправлены reentrant transmission-attempt accounting, вытеснение собственного
ответа чужими terminal reports и GUI callback после завершения QApplication.
Full249/249 (33.24s), production X11 и просмотр диалога проходят; реальные борта
и сетевой SITL не изменялись. Offline MagFit теперь открывает общее modeless-окно
из Developer Tools и Compass: BIN/LOG/TLOG, sphere/ellipsoid, таблица качества,
отмена и отдельно подтверждённое exact-target применение. TLOG остаётся
analysis-only; DataFlash требует явного стабильного контекста компенсации,
device ID и ориентации. Последовательные typed writes сохраняют подтверждённые
receipts и не обещают rollback. Реальный BIN:3x2299 образцов, независимый sphere
oracle отличается менее чем на0.00004мГс по OFS; слабое покрытие4/8 предупреждается.
Экспорта файла в MP10 окне нет; ошибочное ожидание export в реестре исправлено.
Start/Stop Remote DataFlash Log теперь реализованы: точные подтверждения,
application-owned запись после закрытия страницы, bounded worker, повторные ACK,
явное сохранение BIN/partial.BIN и unpublished .part при обрыве. Production X11
проверяет ровно600 байт, чужие пакеты, armed Stop и оба log-protocol interlock.
Протокол не даёт nonce/EOF/STOP ACK; отправленный STOP не доказывает остановку.
MAVLink Serial TCP Bridge теперь реализован: отдельное общее окно, все15 UART/
10 baud, точное default-Cancel подтверждение, один TCP-клиент, bounded binary
SERIAL_CONTROL и monotonic10ms pacing. Штатный FIN допередаёт принятый хвост;
Stop/arming/loss отменяют очередь. Освобождение UART только исходному ещё
известному instance/epoch через прежний маршрут, с честным статусом без ACK.
Full257/257 (46.54s), focused7/7,10 повторов каждого из3 bridge suites и X11
с281/74 binary bytes/Cancel/armed release/читаемыми окнами проходят. Реальных
SITL/UART записей не было. Shutdown-only, новый второй system на маршруте и
потеря исходного instance могут запретить release; baud/flow control не восстановлены.
Flight Log Index теперь реализован: общее offline modeless-окно, все13 колонок,
BIN/LOG/TLOG метрики, canonical cache-only JPEG sidecars, immutable exact-companion
delete и настоящее открытие выбранного файла в сохранённом LogAnalysis.
Тесты262/262 (47.36s), focused9/9 (35.73s) и production X11 проходят; root
просмотрел список, правые колонки и полное предупреждение удаления. Реальный
BIN и два независимых oracle выявили ошибку часов и подтвердили исправление:
242.800341s/552.7791954304238m, исходный SHA256 сохранён; неполный40-byte хвост
честно предупреждается. Ожидание Search удалено из реестра: в active MP10 его нет.
Download/Cancel Firmware Archive теперь реализован: настоящие offline-диалоги,
точное default-Cancel подтверждение, четыре bounded worker, HTTPS-first,
SHA256/UTF8 manifest/report, partial publication и owned Cancel. Полный набор
267/267 (47.78s), focused9/9, десять повторов каждого из4 suites и production
X11 проходят. Root просмотрел полное предупреждение и страницу. Официальный
многогигабайтный архив и сетевой SITL не затрагивались. Исправлены IPv6 naming,
корневой url по XDocument.Descendants и декларация кодировки после UTF8 rewrite.
Отдельная строка Serial TCP Bridge в реестре ошибочно оставалась not-started;
она приведена к уже проверенной реализации. Inventory129:74 in-progress,
36 partial,19 not-started; ни одна поверхность не strict-complete.
Convert Shapefile to POLY теперь реализован: offline-выбор SHP, bounded
геометрия/DBF, строгая GDAL/GEOS validity и private offline PROJ, точный
default-Cancel Create/Replace план, атомарные файлы и честные частичные итоги.
Тест выявил потерю семантики нулевого TOWGS84 при промежуточном WKT2;
исходный проверенный WKT сохраняется без ослабления strict/no-ballpark policy.
Дополнение NTF Paris: только уникальное100% EPSG-сопоставление с полной
эквивалентностью CRS и сохранением осей/единиц; явные bindings/grids/epochs
не заменяются. Все32 projection cases проходят по10 раз без пропусков.
Developer28/32; SETUP/Tools route counts и inventory129 не изменились.
MicroDrone Downlink теперь реализован: modeless serial-окно,8 baud/default57600,
семь записей и bounded exact-source service. HOME_POSITION/IMU variants/EMA
сверены с первичным CurrentState; .NET10.0.11 oracle подтверждает два полных
кадра и13 числовых случаев. Full278/278 (48.68s),10 повторов пяти новых suites
и NTF projection, production X11 и просмотр двух screenshots проходят.
Developer29/32; inventory129:75 in-progress/36 partial/18 not-started,
ни одна поверхность не strict-complete. См. MICRODRONE_DOWNLINK_PORT.md:
SI вместо display units, точные epochs/retirement, bounded backpressure и
непроверенное физическое оборудование остаются явными границами.
Probe MAVLink Camera теперь также перенесён: отдельный component100, frozen
default-Cancel consent, шесть точных all-zero команд, ACK/retries и modeless
progress. Общий command arbiter обрабатывает physical ACK один раз и повторно
проверяет операцию после signing перед writer. Full281/281 (48.78s), production
X11 и просмотр диалога/страницы проходят; Developer30/32. Подробности и границы
оборудования/протокола: CAMERA_PROBE_PORT.md. Первоначальные предположения Claude
об отсутствии ACK и необходимости убрать Camera Probe были опровергнуты:
оригинальный диалог прямо предупреждает об изменениях режима и streaming.
Translation/RESX Editor теперь перенесён целиком как функциональный offline
load/edit/filter/export/backup/resume workflow: реальное modeless окно, all-row
CSV, frozen loaded culture, default-Cancel экспорт, резервирование всех старых
файлов до первой замены, bounded cancel/close. Исправлена ошибка Linux-каталога
.NET с45 китайскими localized файлами/924 строками;858 культур,89 исходных файлов
и2264 строки точно совпадают с фактическим .NET oracle после исключения ошибки.
RESX/HTML экспорт совпал побайтно. Full285/285 (49.71s),10 повторов трёх suites,
production X11 и просмотр окна/consent проходят. См. RESX_TRANSLATION_EDITOR_PORT.md.
SFTP теперь перенесён: real SSH listing/download/delete/cancel, pre-auth host-key
pinning, BIN→LOG/KML, safe publication и полноценное modeless окно. Full290/290
(50.93s), localhost SSH, production X11 и побайтовый oracle реального BIN проходят.
См. SFTP_LOG_DOWNLOAD_PORT.md. Число32/32 относится только к Developer: внутри
Tools остаются disabled MATLAB export, Terrain3D guided click и Signing rekey/
recovery; вне меню — отсутствующие DATA log-tool routes и другие диалоги.
Следующий пакет — MATLAB export. TOOLS_REAUDIT_2026_09_06.md уточняет полный
аудит, включая15 MP10 DATA tabs против6 mapped+Quick Legacy. Далее Settings;
Swarm остаётся в конце.
Settings после Tools, Swarm в конце. Подробности: `TLOG_RECORDING.md` и
`DATAFLASH_LOG_SPLIT.md`, `DATAFLASH_DASHWARE_CSV.md` и
`MAVFTP_DEVELOPER_DOWNLOAD.md`, `MAVFTP_BROWSER_TARGET_CONSENT.md`,
`APJ_DEFAULTS_PORT.md`, `LOG_DIRECTORY_ORGANIZER.md`, `UPGRADE_BOOTLOADER_PORT.md`,
`LOG_INDEX_PORT.md`, `FIRMWARE_ARCHIVE_PORT.md`, `SHAPEFILE_POLY_PORT.md`,
`PARAMETER_RECOVERY_PORT.md`, `OFFLINE_MAGFIT_PORT.md`, `REMOTE_DATAFLASH_LOG_PORT.md`,
`MAVLINK_SERIAL_TCP_BRIDGE_PORT.md`.

Ниже — историческая исходная Linux-точка после DataFlash Spectrogram, 3D Terrain, External Guided, Follow Me, Moving Base, RF Propagation, OSD Video, offline Swarm Sequence, Formation, Follow Path, Follow Leader и production Waypoint Leader:

- приложение и все цели CMake собираются одним `cmake --build build-codex-qt -j12`;
- проходят 194 из 194 тестов;
- реальный X11-запуск показывает точный заголовок
  `APM Planner 3.0.0 (...) — APM Planner`;
- DATA, PLAN и SETUP открываются, карта DATA работает, SETUP OSD создаёт
  отдельный `ConfigHWOSDView`, приложение закрывается без падения;
- сохранён единый exact-target стек `(link, system, component, generation)` для
  команд и параметров, единый tile cache и доверенная QML-система расширений;
- DATA/PLAN используют фиксированные `QWidget`/`QSplitter`, а не плавающие
  `QDockWidget` или KDDockWidgets.

Это не означает готовность продукта. В реестре 129 поверхностей. Таблица ниже
пересчитана непосредственно из TSV после Translation Editor; его отдельная
TOOLS строка перешла из not-started в in-progress. Это не strict-complete.

| Область | in-progress | partial | not-started | Всего |
|---|---:|---:|---:|---:|
| SHELL | 1 | 3 | 0 | 4 |
| DATA | 2 | 3 | 0 | 5 |
| PLAN | 5 | 2 | 4 | 11 |
| SETUP | 31 | 16 | 9 | 56 |
| CONFIG | 11 | 9 | 0 | 20 |
| TOOLS | 26 | 1 | 4 | 31 |
| SIMULATION | 0 | 1 | 0 | 1 |
| HELP | 0 | 1 | 0 | 1 |
| **Итого** | **76** | **36** | **17** | **129** |

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

Основной TOOLS/диалоговый поток доведён через Device Operations, DataFlash
Spectrogram, 3D Terrain View, External Guided, Follow Me, Moving Base, RF
Propagation, OSD Video, Swarm Sequence editor, Formation,
Follow Path и Follow Leader. Waypoint Leader теперь имеет application-owned
exact mission adapter, центральные COMMAND_ACK/Parameter владельцы,
safety-critical core, all-or-nothing executor и включённое bounded native
окно. Для Sequence теперь также подключён отдельный application-owned exact
Run Step/Takeoff executor с immutable revalidation, общей Swarm/COMMAND_ACK
резервацией и fail-closed partial/uncertain состояниями; focused и полный
194-test suite проходят, новый X11 smoke ещё не зафиксирован. Дальнейшая Swarm
работа перенесена за оставшиеся single-vehicle Tools. Регрессия action-binding
в SETUP Advanced/Developer Tools исправлена и покрыта page-before-actions
тестом. `Connection Options` теперь открывает точную форму настроек MP10, а
прежний add-link workflow сохранён отдельной кнопкой `+`; теперь портируются
остальные одиночные диалоги,
после чего приоритет переходит к Settings/CONFIG.
Оставшиеся специализированные Tools сохраняются в точном меню, но
включаются только после полноценной реализации. Пункты 3–4 остаются важными пробелами;
Qt Widgets и доверенный QML API можно сочетать по назначению.

1. Убрать пустую полосу у правой границы PLAN и зафиксировать геометрию при
   1120×720, 1280×800, 1920×1080 и HiDPI.
2. Tools → MAVLink Inspector теперь использует отдельные modeless окна с
   exact physical-link/replay source и раздельными sysid/component/message
   ключами; Pause/Resume, Clear, Filter, Show GCS Traffic и независимый Graph It
   работают. Следующий slice — безопасный Download Logs (точная цель,
   отмена, ограниченные retries, atomic output).
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
- Follow Me и external guided имеют основные безопасные срезы; moving base и formation/swarm отсутствуют как завершённые workflow;
- NMEA/CoT output, MAVLink serial/TCP bridge и serial pass-through;
- OSD video overlay и translation editor; MicroDrone имеет основной рабочий
  serial-срез, но ждёт hardware/native evidence; Device Operations уже
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

Состояние 2026-09-05: новый `MAVLinkInspectorView` заменяет legacy content
в независимых modeless окнах. Реализованы Pause/Resume, Clear, фильтр из cache,
дерево `Vehicle → Component → Message → Field`, точные integer/array значения,
333 ms refresh и MP10 three-second Hz/Bps. Cache ограничен 4096 identities и
200 samples/key; source закрепляет physical-link QObject + epoch либо replay
generation. Отключение сохраняет снимок, новое соединение того же объекта
очищает старую сессию даже при Pause. Legacy heartbeat/UAS gate обходится
отдельным диагностическим сигналом только для уже разобранных пакетов.

- Сохранять оба входа: верхнее Tools menu/Ctrl+I и SETUP Advanced Tools.
- Exact outbound observations после успешной typed-записи и Show GCS Traffic
  реализованы без sniffing произвольного `writeRawBytes`.
- Graph It реализован для numeric scalar/array fields: исходный pin не меняется,
  history 10..100000 (default 500), независимые modeless графики и 100ms refresh.
- Обновлять vendored MAVLink dialect отдельным глобальным slice с parser и
  service regression tests; старый dialect не расширять в обход parser.
- Подтвердить real replay-file unload/reload и native-platform evidence;
  source generation unit tests не считать проверкой настоящего tlog.
- Позже выровнять monospaced reference tree, component enum labels и порядок;
  сохранённые отдельные Value/Type колонки — явное GUI deviation.

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
- Video: локальный file+tlog HUD-to-video export уже доступен в TOOLS через
  Qt Multimedia и bounded MJPEG AVI; для DATA остаются live MAVLink camera
  stream, snapshot/record и их teardown.
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

### Wave 5 — SETUP, 56 поверхностей

#### 5A. Завершить уже начатые native Qt vertical slices

- ADSB: live uAvionix hardware, profile gate, screenshots, native platforms.
- Default Settings: cancellation/cache/symlink/live-network evidence.
- HW ID: реальные CAN/I2C/SPI/UAVCAN ids, export/copy parity.
- Initial Params: live ACK and vehicle-family coverage.
- Serial, Servo Output, ESC Calibration, Motor Test, GPS Order, HW CAN,
  Bluetooth, Parachute, ESP8266, Battery Monitor 2: live devices, target switch,
  native serial/USB, screenshots.
- Developer Tools: после SFTP все32 маршрута имеют реализацию; это не полная
  функциональная готовность всех диалогов Tools. Следующий MATLAB export,
  Terrain3D guided click и Signing transitions остаются отдельными пакетами.
  Advanced отдельно:14 complete, Signing partial, Support Proxy unavailable.
- Elevation Sources и Mission Command List: native/package evidence.
- SETUP OSD: live 24-write full/partial path и profile gate.
- Current Compass: parameter/priority, onboard multi-compass, Large Vehicle
  fixed-yaw и отдельный Compass/Motor slices сделаны; остаются
  physical-hardware evidence. OfflineMagFit теперь имеет общее окно и guarded
  apply; широкая совместимость компенсированных логов остаётся отдельным gate.

#### 5B. Исправить partial legacy pages

- Setup shell/order/profile gating;
- Install Firmware и legacy firmware route;
- Frame Class/Type, Accel и Compass Legacy;
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

#### 5C. Реализовать 9 оставшихся отсутствующих MP10-страниц

Два пробела исходного списка из 11 закрыты: Joystick — честный legacy launcher,
FFT Setup — нативная страница и независимое окно с exact-параметрами и анализом
логов (`FFT_PORT.md`, 213 тестов и X11). Это закрытие маршрутов, не заявление
о полном аппаратном/визуальном паритете.

- `Install Firmware Legacy`;
- `ConfigAntennaTrackerParamView`;
- `ConfigCubeIDView`;
- `ConfigOnboardReplView`, `ConfigScriptReplView`;
- `ConfigPX4FlowView`;
- `ConfigSecureView`, `ConfigSecureApView`;
- `NvModemView`;

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
  точный аудит насчитывает 64 MP10 controls, из них 21 уже имеет рабочие
  нативные эквиваленты после live HUD Overlay, полного event Speech slice,
  Message Severity и Startup UDP. Центральный
  announcer применяет MP10 gates/templates для Armed Only, Waypoint, Mode,
  Battery, Arm/Disarm, periodic Custom/Alt Warning/Low Speed и No Data. Далее —
  общий unit service и Speed Units, OSD Color, Speech Level/Vario, затем
  shortcuts, target-safe telemetry rates/identity и map overlays.
- Planner Advanced и User Defined — завершить screenshots/native evidence.
- Param Compare — полный merge/diff/apply workflow.
- CONFIG Onboard OSD — Wave 2.
- Shared MAVFTP UI: завершить streaming/burst, capability gate, exclusive
  no-replace local save и live-hardware evidence.
- FFT analysis window перенесён; остаются field-log/native-platform и
  screenshot gates (`FFT_PORT.md`). Warning Manager пока отсутствует.

Gate: изменение параметров всегда проходит typed metadata validation и ACK,
dirty state никогда не переносится на новый target.

### Wave 7 — TOOLS, 31 поверхность

#### Основные инструменты P1

- MAVLink Inspector — Wave 1B;
- MAVLink log player: seek/speed/pause/state isolation/Inspector integration;
- DataFlash Log Download: list/selected/all/erase/cancel/export;
- Log Browse: graphs/messages/params/map/report/export;
- DataFlash Spectrogram: основной modeless/direct+batch/cancellation срез
  сделан; остаются reference screenshot и native-platform evidence;
- Terrain 3D: основной modeless/exact-target/software-rendered DEM/camera/
  hover/cancellation срез сделан; остаются imagery через единый tile cache,
  guided click через существующий shared service с current-altitude/confirmation,
  reference screenshot и native-platform evidence;
- External Guided: основной modeless/bounded-file/exact-target/ACK-gated срез
  сделан; остаются legacy Plane/current=2 и position-target fallback с честной
  семантикой результата, reference screenshot, older-autopilot и native-platform evidence;
- Follow Me: основной modeless/manual+serial-NMEA/exact-target/ACK-gated срез
  сделан и доступен из TOOLS/SETUP Advanced; остаются physical-serial/vehicle,
  older-autopilot, reference screenshot и native-platform evidence;
- Moving Base: основной modeless Serial/TCP/UDP, bounded-log,
  exact-target service и cyan `BASE` marker срез сделан и доступен из
  TOOLS/SETUP Advanced; остаются Rally mission transaction, relative-altitude
  home source, TCP reconnect, physical/network/live-marker и native evidence;
- RF Propagation: основной modeless settings/exact-target/worker-safe срез
  сделан; DATA и PLAN получают независимые elevation/terrain, RF-contour и
  battery-distance overlays через backend-neutral map contract. SRTM admission
  ограничен восемью уникальными tiles до GUI dispatch; остаются raster
  dateline splitting, ещё один production map backend, representative live
  terrain/vehicle, reference screenshot и native-platform evidence;
- OSD Video: основной singleton/modeless file+tlog/offset/HUD-to-silent-MJPEG
  срез сделан через Qt Multimedia с bounded lossless handoff, cancellable
  partial-file finalize, no-overwrite, synthetic decode и writer-output E2E;
  остаются audio copy, полное firmware-specific CurrentState/mode покрытие,
  representative real-log/reference и native-platform evidence;
- Device Operations: основной modeless/exact-target срез сделан; остаются
  hardware/native evidence и визуальная полировка;
- Formation Control: singleton modeless leader/table/canvas/capture surface и
  точная Copter/Rover position+velocity отправка до 10 Гц сделаны через общий
  multi-endpoint sender; остаются Plane PID, yaw/gimbal, bulk flight commands,
  dedicated Serial route, checked enqueue, live/reference/native evidence;
- Swarm Follow Path: singleton modeless leader/order/table surface, bounded
  newest-first trail и точная Copter/Rover position-only отправка на 5 Гц
  сделаны через общий multi-endpoint sender; family-specific ArduPilot
  `custom_mode` проверяется до подтверждения, каждый tick и повторно после
  route callback непосредственно перед отправкой; остаются automatic GUIDED,
  Plane guided-waypoint/ACK, bulk flight commands и live/reference/native evidence;
- Swarm Follow Leader: production modeless ground/air/follower surface,
  exact-GUIDED group control, MP10 trail/velocity/turn geometry и 10 Гц sender
  сделаны; остаются подключение exact mission/current-navigation snapshot,
  bulk commands через центральный ACK arbiter и live/reference/native evidence;
- Swarm Sequence: singleton editor подключён к application-owned exact executor;
  Run Step требует уже подтверждённый exact GUIDED, запрашивает POSITION 10 Гц и
  отправляет zero-velocity global targets, а Takeoff проходит GUIDED/heartbeat/
  ARM/heartbeat/TAKEOFF 2 m ACK-цепочку по назначенным машинам. Immutable план
  повторно проверяется после default-Cancel подтверждения и после Swarm →
  VehicleCommand reservation, reverse drain и `PartialEffect`/
  `OutcomeUncertain` блокируют безопасный повтор. Focused и полный 194-test
  suite проходят; остаются production X11, live multi-vehicle/reference/native evidence и операторское
  восстановление после заблокированного результата;
- Swarm Waypoint Leader: exact mission cache/coordinator, центральные exact
  endpoint/COMMAND_ACK/Parameter владельцы, transport-free staged-flight core,
  all-or-nothing intent executor и production 1320x790 окно подключены; пункт
  TOOLS включён и X11 lifecycle проверен. Остаются operator acknowledgement
  после OutcomeUncertain, live multi-vehicle/reference/native evidence;
- QML Plugin Manager: широкая документация и API coverage.

#### Отсутствующие инструменты P2

- SFTP Log Download; Log Index реализован, остаются native/filesystem gates;
- Photo/video GeoRef;
- Terrain Maker;
- Microdrone Downlink реализован; физическая совместимость/native gates остаются;
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
  и синхронизация Advanced Mode — сделано; 31 SETUP-флаг уже gate активные
  фабрики, четыре ожидают отсутствующие routes, также остаются profile consumers
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
