# ESP32-4848S040 PC Telemetry Dashboard & Smart Hub

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange.svg)](https://platformio.org/)
[![LVGL](https://img.shields.io/badge/LVGL-v8.3-blue.svg)](https://lvgl.io/)
[![Python](https://img.shields.io/badge/Python-3.10%2B-brightgreen.svg)](https://www.python.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

[English](#english) | [Русский](#русский)

---

<a name="english"></a>
## 🇬🇧 English

A desktop hardware monitoring dashboard and smart controller running on the 4.0" square **Guition ESP32-4848S040** board (ESP32-S3-WROOM-1, 480x480 RGB ST7701 display, GT911 capacitive touch). The Linux host sends system telemetry over UART0 via CH340 using a robust binary TLV protocol, while the ESP32 renders a **Clean Modern Flat Dark UI** with LVGL 8.3.

```
+-------------+   UART0@115200 (CH340)   +----------------------------+
| Linux Host  | <----------------------> | ESP32-S3 (4848S040)        |
| host.py     |   Binary Frame + CRC8    |   LVGL 8.3 Flat Dark UI    |
| psutil/DRM  |   /dev/ttyUSB0 (GPIO 43/44) |   480x480 ST7701 + GT911   |
+-------------+                          +----------------------------+
```

### ✨ Key Features

1. **Main Dashboard**:
   - **CPU**: Load arc gauge, current percentage, and per-core utilization bars.
   - **GPU**: Load arc gauge, VRAM usage bar, and `used / total` memory (via DRM `fdinfo`, zero root required).
   - **RAM**: Memory usage bar with detailed `used / total` MB.
   - **Dual Disk Telemetry**: Independent percentages for primary and secondary drives (`D1: 55%  D2: 60%`), real-time read and write speeds.
   - **Network**: Live dual-series RX/TX traffic graph.
   - **Header**: Live digital clock, host uptime, `LAMP` and `OFF` toggles.

2. **AI / LLM Model Manager & Profile Picker**:
   - Live integration with `llmcontrol` HTTP API.
   - 6-card paginated grid of available local LLMs with status badges (`IDLE`, `STARTING`, `RUNNING`, tokens/sec `TPS`).
   - Profile selection modal for each model (context window `16k/64k/256k`, quantization `q4_0/q8_0/f16`, FlashAttention, vision).
   - Global `STOP ALL` and per-model stop controls.

3. **Smart Lamp Control (BLE Tuya Beacon)**:
   - Direct Bluetooth Low Energy (BLE) control of Tuya-compatible RGBCW lamps without clouds, hubs, or apps.
   - Power switch, 6 fast color presets (*Cyan*, *Mint*, *Amber*, *Purple*, *Ruby*, *Daylight*), smooth Brightness & Warmth/CCT sliders.
   - **Ambient Sync**: Dynamically adapts lamp hue and intensity based on peak PC load (`Max(CPU, GPU, RAM)`).

4. **Top Processes & System Overlays**:
   - Tap any card (CPU, RAM, GPU, Disk) to open a detailed breakdown of top resource-consuming processes.
   - Global display brightness slider (PWM control via LEDC).
   - Instant touch wake-up with debounce protection.

---

### 🛠 Complete Hardware Pinout (Guition ESP32-4848S040)

> **Important**: The USB-C connector on this board is wired through a **CH340 USB-UART bridge to UART0 (GPIO 43/44)**, NOT to native USB-CDC (GPIO 19/20). The GT911 touch controller is connected to I2C0 (**SDA=19 / SCL=45**).

| Function | Pin / GPIO | Description |
|---|---|---|
| **RGB LCD R0..R4** | `11, 12, 13, 14, 0` | Red bus (5 bits) |
| **RGB LCD G0..G5** | `8, 20, 3, 46, 9, 10` | Green bus (6 bits) |
| **RGB LCD B0..B4** | `4, 5, 6, 7, 15` | Blue bus (5 bits) |
| **RGB Sync / Clock** | `HSYNC: 16, VSYNC: 17, DE: 18, PCLK: 21` | PCLK @ 16 MHz, HSYNC 10/8/50, VSYNC 10/8/20 |
| **LCD SPI Init** | `CS: 39, SCK: 48, MOSI: 47` | 3-wire 9-bit SPI for ST7701 initialization |
| **Touch (GT911)** | `SDA: 19, SCL: 45` | I2C0 bus |
| **UART0** | `TX: 43, RX: 44` | 115200 baud via CH340 & SP3485 RS485 |
| **Backlight** | `GPIO 38` | PWM dimming via LEDC (0..255) |
| **Onboard Relays** | `GPIO 1, 2, 40` | Switch channels |
| **MicroSD Slot** | `CS: 42, MISO: 41` | SPI bus |

---

### 💡 Smart Lamp Setup Guide (Tuya BLE Beacon)

Tuya BLE Beacon lamps (Phyplus chipsets) operate in **receive-only** mode via encrypted `ADV_IND` Bluetooth packets without establishing a permanent GATT connection.

#### Step 1: Obtain Lamp Credentials (`localKey` & `MAC`)
1. Add the lamp to the **Smart Life** or **Tuya Smart** mobile app.
2. Retrieve the 16-character ASCII `local_key` and MAC address using either:
   - **Tuya Developer Platform ([iot.tuya.com](https://iot.tuya.com))**: *Cloud* → *API Explorer* → *Smart Home Device System* → *Device Control* → *Get Device Details*.
   - **TinyTuya CLI**: Run `tinytuya wizard` in Python to export device keys from your Tuya cloud account.

#### Step 2: Configure `secrets.h`
Create your private credentials file from the template:
```bash
cp firmware/src/secrets.example.h firmware/src/secrets.h
```

Edit `firmware/src/secrets.h`:
```cpp
#pragma once
#include <stdint.h>

#define TUYA_LAMP_MAC   {0xDC, 0x23, 0x4F, 0xAA, 0xBB, 0xCC} // Lamp BT MAC
#define TUYA_LOCAL_KEY  "0123456789ABCDEF"                   // 16-char ASCII localKey
#define TUYA_CTRL_NAME  "SmartCtr"                           // Controller name (<= 8 chars)
```

#### Protocol Specification:
- **Encryption**: Big-endian **XXTEA** (19 rounds) with the 16-byte ASCII key inside the 128-bit Service UUID.
- **Packet Structure**: 16-byte plaintext with frame marker `0x2C`, 24-bit anti-replay `seq` counter, DP data, and CRC-8.
- **Anti-Replay**: Sequence counter is automatically persisted to ESP32 NVS flash.

| DP ID | Function | Type | Values |
|---|---|---|---|
| `0x01` | Power | Boolean | `0x00` (Off) / `0x01` (On) |
| `0x02` | Mode | Enum | `0x00` (White) / `0x01` (Colour) |
| `0x03` | Brightness | Value | `10..1000` (Big-Endian) |
| `0x04` | Warmth / CCT | Value | `0` (Warm) .. `1000` (Cool) |
| `0x0B` | Color HSV | Raw | `Hue (0..360 BE16), Sat (0..100), Val (0..100)` |

---

### 🚀 Getting Started

#### 1. Host Agent Setup (Linux)

```bash
cd host/
pip install -r requirements.txt

# Run manually
python3 host.py --port /dev/ttyUSB0 --baud 115200
```

To run as a persistent systemd user service:
```bash
systemctl --user daemon-reload
systemctl --user enable --now esp32-display.service
```

#### 2. Firmware Compilation & Flashing

```bash
# Stop backend to free /dev/ttyUSB0
systemctl --user stop esp32-display.service
cd firmware/
pio run -t upload
systemctl --user start esp32-display.service
```

---

<a name="русский"></a>
## 🇷🇺 Русский

Аппаратный монитор системной нагрузки ПК и пульт умного дома на базе 4.0" квадратного экрана **Guition ESP32-4848S040** (ESP32-S3-WROOM-1, 480x480 RGB ST7701, емкостный тач GT911). Хост под управлением Linux собирает метрики и транслирует их по UART0 через CH340 с помощью бинарного TLV-протокола, а ESP32 отрисовывает интерфейс в стиле **Clean Flat Dark UI** на библиотеке LVGL 8.3.

```
+-------------+   UART0@115200 (CH340)   +----------------------------+
| Linux Хост  | <----------------------> | ESP32-S3 (4848S040)        |
| host.py     |   Батч-кадр + CRC8       |   LVGL 8.3 Flat Dark UI    |
| psutil/DRM  |   /dev/ttyUSB0 (GPIO 43/44) |   480x480 ST7701 + GT911   |
+-------------+                          +----------------------------+
```

### ✨ Основные возможности

1. **Главный экран (Dashboard)**:
   - **CPU**: Круговая шкала загрузки, проценты и полосы по каждому ядру.
   - **GPU**: Круговая шкала, занятость VRAM и память `занято / всего` (через DRM `fdinfo`, права root не требуются).
   - **RAM**: Индикатор памяти и значения `занято / всего` в МБ.
   - **Два диска**: Раздельные проценты для основного и второго накопителя (`D1: 55%  D2: 60%`), реальные скорости чтения и записи.
   - **Сеть**: График сетевой активности (RX/TX) в реальном времени.
   - **Шапка**: Живые цифровые часы, аптайм ПК, кнопки `LAMP` и `OFF` (выключение экрана).

2. **Управление LLM и выбор профилей**:
   - Прямая интеграция с локальным API `llmcontrol`.
   - Сетка из 6 карточек моделей с пагинацией и статусами (`IDLE`, `STARTING`, `RUNNING`, токены/сек `TPS`).
   - Модальное окно выбора профилей запуска (размер контекста `16k/64k/256k`, квантование `q4_0/q8_0/f16`, FlashAttention, vision).
   - Кнопка аварийного выключения `STOP ALL` и остановка выбранной модели.

3. **Умная лампа (BLE Tuya Beacon)**:
   - Прямое управление Tuya-BLE лампой без шлюзов, облаков и сторонних приложений.
   - Питание, 6 быстрых цветовых пресетов (*Cyan*, *Mint*, *Amber*, *Purple*, *Ruby*, *Daylight*), ползунки яркости и цветовой температуры.
   - **Ambient Sync**: Адаптация цвета лампы под пиковую нагрузку ПК (`Max(CPU, GPU, RAM)`).

4. **Процессы и управление экраном**:
   - Тап по карточке (CPU, RAM, GPU, Disk) открывает список топ-процессов по соответствующему ресурсу.
   - Настройка яркости подсветки через ШИМ на GPIO 38.
   - Мгновенное пробуждение экрана по тапу с защитой от случайных нажатий.

---

### 🛠 Полная распиновка (Guition ESP32-4848S040)

> **Важно**: USB-C порт подключён через микросхему **CH340 к UART0 (GPIO 43/44)**, а НЕ к нативному USB-CDC (GPIO 19/20). Тач GT911 подключён к шине I2C0 (**SDA=19 / SCL=45**).

| Назначение | Пин / GPIO | Описание |
|---|---|---|
| **RGB LCD R0..R4** | `11, 12, 13, 14, 0` | Линии красного цвета (5 бит) |
| **RGB LCD G0..G5** | `8, 20, 3, 46, 9, 10` | Линии зелёного цвета (6 бит) |
| **RGB LCD B0..B4** | `4, 5, 6, 7, 15` | Линии синего цвета (5 бит) |
| **Синхронизация RGB** | `HSYNC: 16, VSYNC: 17, DE: 18, PCLK: 21` | PCLK 16 МГц, HSYNC 10/8/50, VSYNC 10/8/20 |
| **Инициализация LCD (SPI)** | `CS: 39, SCK: 48, MOSI: 47` | 3-проводной 9-битный SPI для ST7701 |
| **Тачскрин (GT911)** | `SDA: 19, SCL: 45` | Шина I2C0 |
| **UART0** | `TX: 43, RX: 44` | 115200 бод (CH340 и RS485 на SP3485) |
| **Подсветка** | `GPIO 38` | ШИМ яркости через LEDC (0..255) |
| **Реле на плате** | `GPIO 1, 2, 40` | Силовые выходы |
| **MicroSD** | `CS: 42, MISO: 41` | Шина SPI |

---

### 💡 Инструкция по подключению умной лампы (Tuya BLE)

Лампы стандарта Tuya Beacon (на чипах Phyplus) работают в режиме **receive-only** — они не держат постоянное GATT-соединение, а слушают шифрованные пакеты рекламы `ADV_IND`.

#### Шаг 1: Получение ключей лампы (`localKey` и `MAC`)
1. Привяжите лампу в мобильном приложении **Smart Life** или **Tuya Smart**.
2. Получите 16-значный ASCII-ключ `local_key` и Bluetooth MAC-адрес одним из способов:
   - **Tuya IoT Platform ([iot.tuya.com](https://iot.tuya.com))**: Раздел *Cloud* → *API Explorer* → *Smart Home Device System* → *Device Control* → *Get Device Details*.
   - **Утилита TinyTuya**: Запустите мастер `tinytuya wizard` в Python для выгрузки ключей привязанных устройств из аккаунта Tuya.

#### Шаг 2: Настройка `secrets.h`
Создайте файл секретов из шаблона:
```bash
cp firmware/src/secrets.example.h firmware/src/secrets.h
```

Заполните ваши данные в `firmware/src/secrets.h`:
```cpp
#pragma once
#include <stdint.h>

#define TUYA_LAMP_MAC   {0xDC, 0x23, 0x4F, 0xAA, 0xBB, 0xCC} // MAC-адрес лампы
#define TUYA_LOCAL_KEY  "0123456789ABCDEF"                   // 16-значный ASCII localKey
#define TUYA_CTRL_NAME  "SmartCtr"                           // Имя контроллера (<= 8 симв.)
```

#### Спецификация протокола:
- **Шифрование**: **XXTEA** (19 раундов, big-endian) с 16-байтным ASCII-ключом в поле 128-bit Service UUID.
- **Структура пакета**: 16 байт plaintext (маркер фрейма `0x2C`, 24-битный счётчик `seq`, тело DP-команды, CRC-8).
- **Счётчик Anti-Replay**: Значение `seq` автоматически сохраняется в энергонезависимую память (NVS) ESP32 при каждой команде.

| DP ID | Функция | Тип | Значения |
|---|---|---|---|
| `0x01` | Питание | Boolean | `0x00` (Выкл) / `0x01` (Вкл) |
| `0x02` | Режим | Enum | `0x00` (Белый) / `0x01` (Цветной) |
| `0x03` | Яркость | Value | `10..1000` (Big-Endian) |
| `0x04` | Температура / CCT | Value | `0` (Тёплый) .. `1000` (Холодный) |
| `0x0B` | Цвет HSV | Raw | `Hue (0..360 BE16), Sat (0..100), Val (0..100)` |

---

### 📦 Протокол передачи данных (v2)

Кадр упаковывается в бинарный пакет с контрольной суммой CRC-8/ATM:

```
[0xAA] [len_lo] [len_hi] [type=0xF1] [TLV_1] [TLV_2] ... [crc8]
```

- **0xAA** — синхробайт.
- **len_lo, len_hi** — 16-битная длина полезной нагрузки (little-endian).
- **type (0xF1)** — маркер фрейма телеметрии.
- **TLV** — последовательность полей: `[field_id: u8][field_len: u8][field_data...]`.
- **crc8** — контрольная сумма CRC-8/ATM по байтам `[type, payload...]`.

| ID | Поле | Описание |
|---|---|---|
| `0x01` | **CPU** | Загрузка CPU (%) и массив загрузки по ядрам |
| `0x02` | **RAM** | Процент, занятая и общая память в МБ |
| `0x03` | **GPU** | Загрузка GPU (%), занятость VRAM (%) и память в МБ |
| `0x04` | **NET** | Скорости приёма и передачи (KiB/s) |
| `0x05` | **DISK** | Скорость чтения/записи (KiB/s), процент D1, процент D2 |
| `0x06` | **HEADER** | Аптайм (с), unix epoch (с), имя хоста |
| `0x07` | **PROC** | Топ процессов по типам (CPU, RAM, GPU, Disk Rd, Disk Wr) |
| `0x08` | **LLM** | Статус активной модели, скорость генерации TPS, имя модели |
| `0x09` | **LLM_MODELS** | Список доступных моделей для сетки |
| `0x0A` | **LLM_PROFILES** | Профили запуска выбранной модели (контекст, тип) |
| `0x0B` | **SET_SCREEN** | Удалённое переключение экранов через хост |

---

### 🧪 Тестирование

Проект покрыт автоматическими тестами энкодера/декодера протокола, сборщика метрик и клиента LLM:

```bash
pytest host/test_unit.py
```

---

### 📄 Лицензия

MIT License. См. [LICENSE](LICENSE) для подробностей.
