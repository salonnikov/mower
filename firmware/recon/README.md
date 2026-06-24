# firmware/recon — прошивка «пассажира-разведчика» (фаза 0.5)

Автономный логгер на ESP32. Косилку не трогает. Пишет на SD сырой сигнал
катушки провода + шлёт сводки по MQTT. На этих данных потом отлаживаем
matched filter и модель движения. См. [../../docs/07-recon-passenger.md](../../docs/07-recon-passenger.md).

## Сенсоры → пины (ESP32 WROOM-32, своё питание)

| Сигнал | Пин | Тип |
|--------|-----|-----|
| Катушка провода (через ОУ) | 36 (VP) | ADC1, in-only |
| IMU SDA / SCL (BNO085/MPU6050) | 21 / 22 | I²C |
| microSD (SPI): SCK/MISO/MOSI/CS | 18/19/23/5 | SPI |
| GPS RX/TX (опц.) | 16 / 17 | UART2 |
| Статусный LED | 2 | out |

> Питание модуля — свой LiPo + buck, в косилку не врезаемся.

## Что делает прошивка

- **Core0:** непрерывный ADC (DMA) катушки → буфер; FFT/zero-crossing →
  `wire_freq`, `wire_mag`; чтение IMU → `turn_rate`, `tilt`; запись сырья на SD.
- **Core1:** Wi-Fi + MQTT-сводки (`mower/mi302/recon/*`), отметки событий
  (`spin_suspected`, `wire_cross`).

## Сборка

PlatformIO, цель `esp32dev` (см. `platformio.ini`). Сборка — в контейнере:
`docker run --rm -v $PWD:/p -w /p infinitecoding/platformio-for-vscode pio run`

## Статус

Скелет. Реализуются по мере появления железа разведчика (IMU + катушка + SD).
