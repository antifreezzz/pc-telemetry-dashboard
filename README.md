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

### 🛠 Hardware Specifications (Guition ESP32-4848S040)

> **Important**: The USB-C connector on this board is wired through a **CH340 USB-UART bridge to UART0 (GPIO 43/44)**, NOT to native USB-CDC (GPIO 19/20). The GT911 touch controller is connected to I2C0 (**SDA=19 / SCL=45**).

| Signal | Pin / GPIO | Notes |
|---|---|---|
| **RGB LCD Data** | R0..R4: 11, 12, 13, 14, 0<br>G0..G5: 8, 20, 3, 46, 9, 10<br>B0..B4: 4, 5, 6, 7, 15 | 16-bit RGB565 bus |
| **RGB Control** | HSYNC: 16, VSYNC: 17, DE: 18, PCLK: 21 | PCLK @ 16 MHz, vsync 10/8/20 |
| **LCD SPI Init** | CS: 39, SCK: 48, MOSI: 47 | 3-wire 9-bit SPI for ST7701 init |
| **Touch (GT911)** | SDA: 19, SCL: 45 | I2C0 bus |
| **UART0** | TX: 43, RX: 44 | 115200 baud via CH340 |
| **Backlight** | GPIO 38 | PWM dimming (LEDC) |

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
   - Прямое управление Tuya-BLE лампой без шлюзов, облаков и приложений.
   - Питание, 6 быстрых цветовых пресетов (*Cyan*, *Mint*, *Amber*, *Purple*, *Ruby*, *Daylight*), ползунки яркости и цветовой температуры.
   - **Ambient Sync**: Адаптация цвета лампы под пиковую нагрузку ПК (`Max(CPU, GPU, RAM)`).

4. **Процессы и управление экраном**:
   - Тап по карточке (CPU, RAM, GPU, Disk) открывает список топ-процессов по соответствующему ресурсу.
   - Настройка яркости подсветки через ШИМ на GPIO 38.
   - Мгновенное пробуждение экрана по тапу с защитой от случайных нажатий.

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
