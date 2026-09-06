# Точка продолжения после перезапуска

Сохранено 2026-09-06 по просьбе пользователя. Репозиторий:
`/home/alex/SRC/apm_planner`, ветка `master`.
Последний функциональный коммит: **8591c65b** —
`feat: implement SETUP u-blox RTK receiver configuration`.
Следующий коммит сохраняет только этот контекст и ссылки на доказательства.
На момент передачи нет незакоммиченного кода, активных сборок или задач
помощников. Весь порт **не завершён**; не объявлять цель выполненной.

## С чего начать

Прочитать `AGENTS.md`, этот файл, верх `CURRENT_STATE.md`, архитектурный
`PORTING_PLAN.md`, `MASTER_PORTING_BACKLOG.md` и реестры
`MISSION_PLANNER_SCREEN_PARITY.tsv` / `PORTING_DEVIATIONS.tsv`.
Сначала проверить `git status`: новые изменения после этой точки принадлежат
пользователю или другому агенту и должны быть сохранены.

Приоритет пользователя: **практические востребованные страницы SETUP,
Tools/Advanced/Developer для одного дрона**. Работающие кнопки должны
показывать настоящие страницы/диалоги, а не пустые заглушки. Не уходить снова
в MATLAB, редкие экспорты и поиск особых случаев. Settings/CONFIG следует
после этого; **Swarm — последний**. Полезные старые модули можно сохранить.
Коммитить проверенные функциональные блоки. Пользователь разрешил Codex
субагентов; Claude использовать для независимых задач и альтернативного мнения.
Старое ограничение останавливаться при10% токенов пользователь отменил.

## Уже сделано и проверено

- `ff013c89`: встроенный Joystick — SDL2,16 RC-каналов, кнопки, профили,
  калибровка и приложение-владелец управления. Остатки — `JOYSTICK_PORT.md`.
- `97d800b5`: Calibrate Level p5=2 и Simple p5=4 через единый command/ACK
  путь, исправлено зависшее busy; Full/Legacy сохранены. `ACCEL_LEVEL_PORT.md`.
- `8591c65b`: реальный u-blox RTK driver вместо заглушек. Connect выполняет
  только setup, Restart запускает Survey In; saved-base Connect делает
  setup/disable-reset/fixed; connected Use — только fixed TMODE3 и poll.
  Live NAV, Save Current Position, ACK-диагностика и остановка по точной
  serial session подключены. Используется существующий QSerialPort.
- Исправлен важный обычный сценарий: до завершения Survey In нет RTCM,
  поэтому serial watchdog теперь учитывает NAV/raw activity; NTRIP по-прежнему
  требует CRC-valid RTCM. Устранено накопление ACK-текста в строке статуса.
- Qt5/audio: application build3 и test-only rebuild4 успешны. Финальные
  **317/317 тестов,110.60s**, production X11: exit0, runtime failures0.
  RTK runtime41.17s пересекает настоящий30s watchdog с1Hz NAV-only потоком,
  затем проверяет Save/Use. Просмотрены страница и подтверждение на скриншотах.

**Submitted означает принятие байтов очередью serial на компьютере, не
подтверждённую настройку GNSS.** ACK/NAK диагностические, Survey valid отдельный.
Физический GNSS, Windows/macOS и реальный caster этим срезом не проверены.
Septentrio driver, ECEF saved input, расширенные UBX counters/version и другие
ограничения описаны в `RTK_UBLOX_PORT.md`. Restart очищает выбранную активную
фиксированную базу, но сохраняет строки; это отличается от MP10.

Inventory129:81 in-progress /33 partial /15 not-started;160 deviations.
Ни одна поверхность не strict-complete. SETUP46 маршрутов, восемь отсутствующих
reference routes; Advanced14 working + Signing partial + Support Proxy gap.
Developer32 — число зарегистрированных реализаций, **не** полная готовность
диалогов. CONFIG15 factories, Planner21/64 controls. Не выдавать эти числа
за процент завершённости порта.

## Следующий конкретный блок: Radio Calibration

Независимо сверено Codex-помощником и Claude TCP c354. **Код ещё не менялся.**

Основные Qt-файлы: `src/ui/configuration/RadioCalibrationConfig.{h,cc,ui}`;
реальный маршрут уже подключён в `SetupView.cpp`.
MP10: `ViewModels/GCSViews/ConfigurationView/ConfigRadioInputViewModel.cs`
и `GCSViews/ConfigurationView/ConfigRadioInputView.axaml` в reference tree.

1. Добавить отсутствующие **Spektrum Bind DSM2 / DSMX / DSM8**.
   `MAV_CMD_START_RX_PAIR`, p1=0, p2=0/1/2 соответственно. Старый
   `UAS::pairRX()` показывает payload, но новая UI должна использовать общий
   exact-target command/ACK путь, не старый широковещательный отправитель.
   Не повторять автоматически. Accepted подтверждает запрос, не физическую
   привязку: поддержка протокола/платы может отсутствовать. Claude сообщает,
   что текущий ArduPilot может игнорировать DSM type и всё равно вернуть ACK.
2. Исправить Reverse: современный `RCn_REVERSED` использует0/1, старый
   `RCn_REV` использует-1/+1. Выбирать современное имя, если оно есть в
   актуальном parameter snapshot, иначе старое; никогда не писать оба.
   Qt сейчас читает/пишет только старое имя. MP10 прячет Reverse для Copter.
3. Исправить обычную калибровку: MP10 запрашивает RC_CHANNELS10Hz на время
   калибровки, затем2Hz, отслеживает значения800..2200, пишет MIN/MAX/TRIM
   только для валидных каналов с min<max и сообщает результат записи.
   Qt сейчас пассивно ждёт поток; завершение записи не представлено полноценно.
   Сохранить полезное подтверждение «стики по центру, газ вниз» и Dead Zones.
4. Смежный явный дефект: Qt elevonsCh1Rev/Ch2Rev передаёт имя
   `ELEVON_CH1_REV`/`ELEVON_CH2_REV` в функцию, ожидающую RCMAP-параметр;
   в результате получается запись RC0_REV/RC1_REV вместо elevon parameter.
   Писать существующий параметр напрямую, показывать elevons только для Plane;
   на современной прошивке этих старых параметров может не быть.

Начать с полезного цельного Radio-среза, подтвердить реальные контролы и
команды/parameter receipts, затем коммит. После Radio — firmware и реальные
остатки Advanced/Developer; не возвращаться к старой export-first очереди.

## Claude: только TCP4096, не MCP

Последняя связь живая: c350 — RTK reference; c351/c352 — watchdog blocker;
c354 — Radio notes, ответ получен2026-09-06 20:03:08 по времени bridge log.
На конец сессии у Claude нет выданной build lease или незавершённой задачи
на изменение файлов. Codex-помощники также завершили работу и освободили файлы.
После перезапуска не рассчитывать на сохранение agent/session identifiers.

Рабочий отправитель (сохранён вне `/tmp`):

```sh
python3 /home/alex/SRC/porting-evidence/2026-09-06-rtk-ublox-8591c65b/claude-submit.py 'Короткая задача или ping'
```

Это обычный TCP к `127.0.0.1:4096`, UTF-8, newline-delimited frames,
`sendall` и `shutdown(SHUT_WR)`. **Не более2048 UTF-8 байт на inline frame**.
Большой результат — файл в `/home/alex/SRC/claude-reports` и короткое сообщение
с путём, размером, SHA256; получить и проверить файл полностью.
При необходимости сначала проверить, что listener4096 жив после рестарта.
Успешная отправка — ещё не ответ Claude; ждать его явного ACK/ответа.

Последний bridge inbox (временный путь, после reboot может исчезнуть):
`/tmp/claude-1000/-home-alex-SRC/16700445-98b7-4e22-9523-62ef70fa5e2f/scratchpad/bridge/inbox.log`.
Исходный `submit.py` лежит рядом. При исчезновении inbox обнаружить текущий
bridge, не подменять TCP на MCP. Назначать независимые файлы/задачи; сборку
Claude разрешать только явной командной lease, в соответствии с AGENTS.md.

## Сборка и безопасность проверок

Координатор запускает configure/build/tests. Только одна сборка глобально,
не более12 задач; никто не меняет исходники во время сборки. Перед **каждым**
configure/build выполнить отдельной командой:

```sh
pgrep -af '(^|/)(cc1plus|clang\+\+|g\+\+|c\+\+)( |$)' || true
```

При свободной build lease стандартные команды из корня репозитория:

```sh
cmake -S . -B build-codex-qt -DAPM_QT_MAJOR=5 -DBUILD_TESTING=ON -DAPM_REQUIRE_QT_AUDIO=ON
cmake --build build-codex-qt --parallel 12
ctest --test-dir build-codex-qt --output-on-failure --parallel 12
```

Бинарник: `build-codex-qt/apmplanner3`. Native X11 переменные:
`DISPLAY=:0 QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software`.
RTK runtime: `--ublox-audit`, screenshot variable `APM_UBLOX_AUDIT_SCREENSHOT`.
В сети пользовательский SITL постоянно передаёт телеметрию: не отправлять
на него calibration/bind/flight/parameter команды из тестов. Использовать
изолированные fixtures; RTK audit отключает startup UDP и использует private PTY.

## Доказательства вне временной директории

Постоянная копия логов, снимков и Claude sender:
`/home/alex/SRC/porting-evidence/2026-09-06-rtk-ublox-8591c65b/`.
Скопированы все результаты из `/tmp/apm-ublox-port.thxRlW/`; исходные файлы
побайтово проверены через `diff`. Логи прошлых неудачных проверок сохранены
вместе с финальным `full4.log`; не путать их с финальным состоянием.
Также сохранён `c350-rtk-ubx-reference.md`, SHA256:
`dcf23d6a5697bee7abc3c88198c4bfe2ff24396c12d9012588cd261566d27b6d`.
Большие логи и PNG не добавлялись в исходный Git-репозиторий.

Reference trees: MP10 `/home/alex/SRC/MP/MissionPlanner`, WinForms
`/home/alex/SRC/MP/Oroginal/MissionPlanner`, QGC
`/home/alex/SRC/qgroundcontrol`, Hermes `/home/alex/SRC/AgroSky/GTU`.
Не изменять reference trees. Для следующего практического блока достаточно
обычного source/workflow сравнения и пропорциональных тестов; повторные
сложные оракулы уже работающих экспортёров не являются приоритетом.
