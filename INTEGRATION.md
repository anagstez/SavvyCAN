# Вбудовування gs_usb-драйвера в SavvyCAN

## 1. Файли

Покласти `gs_usb.h` і `gs_usb.cpp` у `connections/`.

## 2. connections/canconconst.h

```c
    enum type
    {
        GVRET_SERIAL,
        KVASER,
        SERIALBUS,
        REMOTE,
        KAYAK,
        MQTT,
        LAWICEL,
        CANSERVER,
        CANLOGSERVER,
        GSUSB,          // <-- додати тут, перед NONE
        NONE
    };
```

Додавати саме перед `NONE`: значення всіх наявних типів не зсуваються, тому
збережені у налаштуваннях підключення лишаються валідними.

## 3. connections/canconfactory.cpp

```cpp
#include "gs_usb.h"
...
    case GSUSB:
        return new GSUsbConnection(pPortName);
```

`pPortName` тут — рядок "vid:pid" у hex, який кладе вікно підключення.

## 4. SavvyCAN.pro

```pro
SOURCES += connections/gs_usb.cpp
HEADERS += connections/gs_usb.h

macx {
    INCLUDEPATH += /opt/homebrew/include/libusb-1.0 /usr/local/include/libusb-1.0
    LIBS        += -L/opt/homebrew/lib -L/usr/local/lib -lusb-1.0
}
unix:!macx {
    CONFIG    += link_pkgconfig
    PKGCONFIG += libusb-1.0
}
```

## 5. Вікно підключення

Знайти всі місця, де фігурує вже наявний тип, і продублювати логіку:

```bash
grep -rn "LAWICEL" --include=*.cpp --include=*.h --include=*.ui .
```

Мінімум це `newconnectiondialog.cpp` (пункт у списку типів + заповнення
списку пристроїв) і `canconnectionmodel.cpp` (людська назва типу в таблиці).

Список пристроїв заповнюється так:

```cpp
#include "gs_usb.h"
...
ui->cbDevices->addItems(GSUsbConnection::enumerateDevices());
```

`enumerateDevices()` повертає рядки "1d50:606f" для кожного USB-пристрою,
що має vendor-class інтерфейс з парою bulk-ендпоінтів.

## 6. Складання

```bash
brew install libusb
export PATH="$(brew --prefix qt)/bin:$PATH"
qmake && make -j$(sysctl -n hw.ncpu)
otool -L SavvyCAN.app/Contents/MacOS/SavvyCAN | grep usb
```

## 7. Місця, які ймовірно доведеться підправити під вашу гілку

- `pBus.isListenOnly()` у `piSetBusSettings()` — звірити геттер у `canbus.h`.
- `setCapSuspended()` / `isCapSuspended()` — звірити імена в `canconnection.h`.
- `QThread::create()` вимагає Qt 5.10+; для старішого Qt замінити на власний
  підклас QThread або std::thread.
- Якщо відносний час у таблиці виглядає дивно — відняти
  `CANConManager::getInstance()->getTimeBase()` від системної мітки в
  `readerLoop()`.
