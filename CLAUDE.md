# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Огляд

SavvyCAN — Qt5 (>= 5.14) десктопний інструмент для захоплення, перегляду, збереження та реверс-інжинірингу CAN-кадрів. Збірка через qmake (немає CMake). Гілка `QT6WIP` upstream збирається з Qt6; ця гілка — Qt5.

## Збірка та запуск

```sh
qmake                      # release за замовчуванням; для debug: qmake CONFIG+=debug
make -j$(sysctl -n hw.ncpu)
./SavvyCAN                 # на macOS: open SavvyCAN.app
```

- Після збою збірки спершу: `qmake && make clean && make`.
- Потрібні модулі Qt: `core gui printsupport qml serialbus serialport widgets help network opengl`.
- `libusb-1.0` потрібна для драйвера gs_usb (на macOS: `brew install libusb`; шляхи `/opt/homebrew` і `/usr/local` прописані в `SavvyCAN.pro`).
- У release-збірці вимкнено `qDebug()` (`QT_NO_DEBUG_OUTPUT`) — для діагностики збирай debug.
- Артефакти збірки (`*.o`, `moc_*`, `ui_*.h`, `Makefile`, `SavvyCAN.app`) лежать у корені дерева (in-source build) і ігноруються git — не редагуй їх.
- CI: `.github/workflows/build.yml` (Linux/macOS/Windows, `qmake CONFIG+=release`).

### Тести

`test/test.pro` — окремий QtTest-проєкт (`tst_cancon`, `tst_lfqueue`). Він не входить у основний `.pro` (там `CONFIG += NO_UNIT_TESTS`) і посилається на файли, яких вже нема (напр. `socketcan.cpp`), тож, імовірно, застарів. Запуск: `cd test && qmake && make && ./test`; один тест — через ім'я слота: `./test testName` (стандартний аргумент QtTest).

### Переклади

`TRANSLATIONS` у `SavvyCAN.pro`: `en`, `pt_BR`, `uk`. `.ts` — джерело (комітяться), `.qm` ігноруються. Оновити: `lupdate SavvyCAN.pro`; зібрати `.qm`: `lrelease SavvyCAN.pro`.

## Архітектура

Нові `.cpp/.h/.ui` треба вручну додавати до списків `SOURCES/HEADERS/FORMS` у `SavvyCAN.pro` (glob'ів нема). Каталог `ui/` містить `.ui`-файли, `ui_*.h` генерує uic у корінь.

### Шар підключень (`connections/`)

- `CANConnection` (`canconnection.h`) — абстрактний драйвер адаптера. Конкретні драйвери реалізують `pi*`-методи (`piStarted`, `piStop`, `piSetBusSettings`, `piSendFrame`, …). Публічні `start/stop/sendFrame/...` викликають їх у контексті робочого потоку драйвера.
- `CANConManager` (singleton, `CANConManager::getInstance()`) — володіє всіма активними підключеннями, агрегує кадри з усіх шин в одну чергу й сигналізує модель. Шини адресуються наскрізним індексом, а не по підключенню.
- `CanConFactory::create()` (`canconfactory.cpp`) — єдине місце, де `CANCon::type` (`canconconst.h`) відображається на клас драйвера.
- Драйвери: `gvretserial` (GVRET по USB-serial/TCP), `serialbusconnection` (будь-який QtSerialBus-плагін: socketcan, Vector, PEAK…), `lawicel_serial`, `socketcand` (Kayak), `mqtt_bus`, `canserver`/`canlogserver`, `gs_usb` (candleLight, через libusb).
- Додаючи новий тип підключення: значення в `enum type` (`canconconst.h`) вставляй **перед `NONE`** — числа зберігаються в налаштуваннях користувача і не мають зсуватися; далі `canconfactory.cpp`, `newconnectiondialog.cpp` (список типів/пристроїв), `canconnectionmodel.cpp` (назва в таблиці), `SavvyCAN.pro`. Детальний чекліст на прикладі gs_usb — у `INTEGRATION.md`.

### Модель кадрів та вікна

- `CANFrameModel` (`canframemodel.*`) — центральний `QAbstractTableModel` з усіма захопленими кадрами; його використовують головне вікно та більшість аналітичних вікон. Файловий I/O форматів логів — `framefileio.*` (+ `blfhandler`, `pcaplite`), відтворення/відправка — `frameplayback*`, `framesender*`.
- `MainWindow` (`mainwindow.cpp`, ~1900 рядків) створює й тримає решту вікон; кожне вікно — окремий клас з `.ui`.
- `re/` — вікна реверс-інжинірингу (sniffer, графіки, fuzzing, UDS scan/firmware, ISO-TP interpreter, порівняння файлів/DBC, state-вікна). `re/sniffer/` — окремий підмодуль сніфера.
- `dbc/` — парсер/редактори DBC (`dbchandler`, `dbc_classes`, редактори вузлів/повідомлень/сигналів).
- `bus_protocols/` — протокольні обробники `isotp_handler`, `uds_handler`, `j1939_handler`, спільні для кількох вікон.
- `mqtt/` — вбудований QMQTT-клієнт; `qcustomplot.*` — вбудований QCustomPlot; `scripts/` — приклади JS-скриптів для вбудованого скриптового вікна (`scriptingwindow`, `scriptcontainer`, QtQml).
- `utils/` — допоміжні структури (напр. lock-free черга).
- `help/` — HTML-довідка, що вбудовується у `helpwindow`.

## Поточна гілка

`add-gs-usb-driver`: додано драйвер `connections/gs_usb.*`, згадки в `canconfactory.cpp`/`SavvyCAN.pro`, українські переклади (`translations/SavvyCAN_uk.ts`). `INTEGRATION.md` описує, що звіряти в драйвері з поточним API (`isListenOnly()`, `setCapSuspended()`, відносні таймстемпи).

## Нативна збірка macOS

`./build_macos.sh` — qmake + make + `macdeployqt` + ad-hoc підпис; результат `SavvyCAN.app` самодостатній (Qt і libusb у бандлі), запуск: `open SavvyCAN.app`. Потрібно: `brew install qt@5 libusb`.
