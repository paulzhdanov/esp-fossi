# Fossibot F2400 — ESP32 REST API Gateway

ESP32-прошивка для моніторингу та керування портативною електростанцією **Fossibot F2400** через Bluetooth LE.

ESP32 підключається до станції по BLE, читає MODBUS-регістри і віддає дані через простий HTTP REST API. Можна інтегрувати з HomeKit (через HomeBridge), Home Assistant або будь-якою системою автоматизації.

## Що це вміє

- **Моніторинг**: рівень батареї, вхідна/вихідна потужність, час до зарядки/розрядки, стан виходів
- **Керування**: увімкнення/вимкнення AC розетки (220V), USB, DC виходів, LED підсвітки
- **REST API**: простий HTTP інтерфейс з JSON та окремими ендпоінтами
- **Визначення джерела живлення**: розетка чи батарея (через біти регістру 41)
- **Автоперезавантаження**: щоденний рестарт через NTP (або fallback через millis)
- **UDP логування**: весь дебаг летить бродкастом — слухай `nc -kulnw0 5678`

## Сумісність

Має працювати з будь-якою станцією на платформі SYDPOWER, що використовує додаток **BrightEMS**:

- Fossibot F2400, F3600 Pro
- AFERIY P210, P310
- SYDPOWER N052, N066

Тестувалось на **Fossibot F2400** + **ESP32-WROOM-32**.

## Що потрібно

- **ESP32-WROOM-32** (або DevKit, NodeMCU-32 — будь-яка плата з ESP32)
- **Arduino IDE** 2.x
- Bluetooth-відстань до станції (зазвичай до 10 метрів)
- MAC-адреса станції (знайти через BLE-сканер, пристрій називається `FOSSIBOT...` або `POWER...`)

## Встановлення залежностей в Arduino IDE

Відкрий **Sketch → Include Library → Manage Libraries** та встанови:

| Бібліотека | Автор | Версія | Посилання |
|------------|-------|--------|-----------|
| **ArduinoJson** | Benoit Blanchon | 7.4.2+ | [GitHub](https://github.com/bblanchon/ArduinoJson) |

Решта бібліотек вже входять до складу ESP32 Arduino Core:

- `WiFi.h`, `WebServer.h`, `WiFiUdp.h`, `ESPmDNS.h` — WiFi-стек ESP32
- `BLEDevice.h`, `BLEUtils.h`, `BLEClient.h` — ESP32 BLE
- `time.h` — стандартна бібліотека C

> **Важливо**: Інші бібліотеки (HomeSpan, WiFiManager, AsyncTCP тощо) **не потрібні** для цього проєкту.

## Налаштування ESP32 Board в Arduino IDE

Якщо ESP32 ще не додано:

1. **File → Preferences → Additional Board Manager URLs** — додай:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
2. **Tools → Board → Boards Manager** — знайди `esp32 by Espressif` та встанови

## Конфігурація прошивки

Відредагуй секцію `CONFIGURATION` в файлі `fossibot.ino`:

```cpp
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define FOSSIBOT_MAC "00:00:00:00:00:00"  // MAC-адреса твоєї станції
```

Також можна змінити:

- `LED_PIN` — GPIO для LED індикації (за замовчуванням 16)
- `NTP_SERVER_1` — NTP-сервер (за замовчуванням `ua.pool.ntp.org`)
- `TIMEZONE` — часовий пояс (за замовчуванням Київ UTC+2/+3)
- `RESTART_HOUR` / `RESTART_MINUTE` — час щоденного авторестарту

## Компіляція та прошивка

### Налаштування Board в Arduino IDE

**Tools** →

| Параметр | Значення |
|----------|----------|
| Board | ESP32 Dev Module |
| CPU Frequency | 240MHz (WiFi/BT) |
| Flash Frequency | 80MHz |
| Flash Mode | QIO |
| Flash Size | 4MB (32Mb) |
| Partition Scheme | Minimal SPIFFS (1.9MB APP with OTA / 190KB SPIFFS) |
| Upload Speed | 921600 |

### Прошивка через USB

1. Підключи ESP32 через USB
2. **Tools → Port** — обери COM-порт
3. Натисни **Upload**

### Прошивка через esptool (CLI)

```bash
esptool.py -b 921600 write_flash 0x0 fossibot.ino.merged.bin
```

## REST API

Після прошивки пристрій доступний за адресою:
- `http://<IP-адреса>/`
- `http://espfossi.local/` (через mDNS)

### Читання даних

| Ендпоінт | Опис | Приклад відповіді |
|----------|------|-------------------|
| `/api/battery` | Рівень батареї (%) | `85.50` |
| `/api/power` | Потужність (Вт) — вхідна при зарядці, вихідна при розрядці | `550.00` |
| `/api/status` | Статус: `1.00`=розетка, `2.00`=батарея, `0.01`=вимкнено | `1.00` |
| `/api/timeRemain` | Час (хвилини) — до повної зарядки або до розрядки | `120` |
| `/api/timeRemainHr` | Час (години.хвилини) | `2.00` |
| `/api/socket220` | Стан AC розетки (1/0) | `1` |
| `/api/socketUSB` | Стан USB (1/0) | `0` |
| `/api/all` | Всі дані в JSON | `{"power":550,...}` |

### Керування

| Ендпоінт | Дія |
|----------|-----|
| `/api/set?socket220=1` | Увімкнути AC розетку |
| `/api/set?socket220=0` | Вимкнути AC розетку |
| `/api/set?socketUSB=1` | Увімкнути USB |
| `/api/set?socketUSB=0` | Вимкнути USB |
| `/api/set?socketDC=1` | Увімкнути DC виходи |
| `/api/set?socketDC=0` | Вимкнути DC виходи |
| `/api/set?socketLight=1` | Увімкнути LED |
| `/api/set?socketLight=0` | Вимкнути LED |
| `/api/restart` | Перезавантажити ESP32 |

## UDP дебаг

Весь лог передається UDP бродкастом на порт 5678. Для перегляду на будь-якому комп'ютері в тій же мережі:

```bash
nc -kulnw0 5678
```

## LED індикація

| Режим | Значення |
|-------|----------|
| Горить постійно | WiFi + BLE — все ОК |
| Повільне мигання (1 сек) | WiFi є, BLE немає |
| Швидке мигання (0.2 сек) | Немає WiFi |

## Як це працює

1. ESP32 підключається до WiFi
2. Синхронізує час через NTP
3. Підключається до Fossibot по BLE (MAC-адреса)
4. Кожні 25 секунд надсилає MODBUS-запит (Read Input Registers, 80 регістрів)
5. Парсить відповідь і оновлює дані
6. Віддає дані через HTTP REST API
7. Визначає джерело живлення (розетка/батарея) через біт-флаги регістру 41
8. Якщо 10 опитувань підряд без відповіді → статус = 0.01 (вимкнено)

## Подяки

Цей проєкт базується на дослідженнях протоколу з:

- [ESP-FBot](https://github.com/Ylianst/ESP-FBot) (Apache-2.0) — BLE протокол, маппінг регістрів
- [sydpower-mqtt](https://github.com/schauveau/sydpower-mqtt) (MIT) — документація бітових флагів регістру 41
- [fossibot-reverse-engineering](https://github.com/iamslan/fossibot-reverse-engineering) — дослідження SYDPOWER API

## Ліцензія

Apache 2.0 — дивись [LICENSE](LICENSE).
