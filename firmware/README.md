# Прошивка ESP32-S3 — «мозг» косилки

Мозг — это **кастомная прошивка на C++ (PlatformIO)**, а не ESPHome.
Почему: matched-filter приёма провода + жёсткий цикл управления моторами в
реальном времени ESPHome не тянет (см. [../docs/05-control-and-navigation.md](../docs/05-control-and-navigation.md), §E).

База — алгоритмы **Ardumower Azurit** (perimeter/motor/odometry/IMU/docking),
адаптированные под ESP32-S3 + наш сетевой слой MQTT (см. [../docs/06-open-source-landscape.md](../docs/06-open-source-landscape.md)).

## Структура (план)

```
firmware/
├── platformio.ini          цель esp32-s3, зависимости
├── src/
│   ├── main.cpp            старт, создание FreeRTOS-задач на 2 ядра
│   ├── hal/               HAL ESP32-S3: ADC-DMA, ledc-PWM, I2C, Холлы
│   ├── perimeter/         matched filter (порт Azurit perimeter.*)  ← фикс «кручения»
│   ├── motor/             PID скорости колёс + нож (порт Azurit motor.*)
│   ├── odometry/          счёт Холлов → путь/скорость
│   ├── imu/               BNO085/ICM драйвер + курс
│   ├── behavior/          конечный автомат: MOW/FOLLOW/RETURN/DOCK/ESTOP  ← фикс «дока»
│   ├── safety/            локальный E-stop (lift/tilt/overcurrent/theft)
│   └── net/               Wi-Fi, MQTT (HA + свой сервер), OTA, расписание-кэш
└── esphome/
    └── mower.yaml         ⚠️ DEPRECATED как мозг. Оставлен ТОЛЬКО как быстрый
                           стенд телеметрии (read-only), пока пишется C++-прошивка.
```

## Разделение по ядрам (FreeRTOS)

- **Core 0 (real-time, без сети):** задачи `perimeterTask` (ADC-DMA+matched filter),
  `controlTask` (PID + FSM поведения + одометрия/IMU), `safetyTask` (E-stop).
- **Core 1 (сеть):** `netTask` (Wi-Fi/MQTT/OTA/команды/телеметрия).

Связь между ядрами — очереди/`portMUX` (без блокировки real-time-цикла сетью).

## Порядок ввода (безопасно)

1. Собрать пустой каркас, проверить Wi-Fi+MQTT (телеметрия-заглушки в HA).
2. Подключить и проверить ADC катушки → видеть signed magnitude в реальном времени.
3. IMU + одометрия → видеть курс и путь.
4. **E-stop ножа** — отладить ПЕРВЫМ из управления (стенд, без травы).
5. PID моторов на колёсах (вывесить колёса), потом — поведение/следование/док.
6. Полевые тесты на малой зоне.

## Зависимости (ориентир)

- `bblanchon/ArduinoJson` — payload MQTT.
- `knolleary/PubSubClient` или `256dpi/arduino-mqtt` — MQTT.
- IMU: `adafruit/Adafruit BNO08x` (если BNO085).
- Перименто/PID — портируем из Azurit (в репозиторий как vendored, с пометкой GPL).
