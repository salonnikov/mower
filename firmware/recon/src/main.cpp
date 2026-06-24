// recon/main.cpp — «пассажир-разведчик»: автономный логгер на ESP32.
// Косилку НЕ трогает. Слушает: катушку провода, доплер-радар, IMU. Пишет SD + MQTT.
// СКЕЛЕТ: тела модулей дописываются по мере появления железа. См. docs/07.

#include <Arduino.h>

// --- Заглушки сенсоров ---
namespace coil  { void begin(); float freqHz(); float magnitude(); bool symmetric(); }  // родной сигнал провода
namespace radar { void begin(); float speedMps(); }                                      // скорость по земле (доплер)
namespace imu   { void begin(); float turnRateDps(); float tiltDeg(); }
namespace sdlog { void begin(); void writeRaw(); }                                        // сырьё на microSD
namespace net   { void begin(); void publish(const char* topic, float v); void event(const char* e); }

// ============ Core0: сбор данных (быстро, локально) ============
void sampleTask(void*) {
  coil::begin(); radar::begin(); imu::begin(); sdlog::begin();
  for (;;) {
    sdlog::writeRaw();                 // сырые семплы катушки/радара (ADC-DMA) на SD

    float v   = radar::speedMps();     // скорость без проскальзывания колёс
    float turn= imu::turnRateDps();
    // Сигнатура «кручения на месте»: стоим (v~0), но крутимся (|turn| большой)
    static uint32_t spinTicks = 0;
    if (v < 0.05f && fabsf(turn) > 30.0f) { if (++spinTicks == 25) net::event("spin_detected"); }
    else spinTicks = 0;

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============ Core1: сводки по Wi-Fi/MQTT (медленно) ============
void netTask(void*) {
  net::begin();
  for (;;) {
    net::publish("mower/mi302/recon/speed",     radar::speedMps());
    net::publish("mower/mi302/recon/turn_rate", imu::turnRateDps());
    net::publish("mower/mi302/recon/wire_freq", coil::freqHz());
    net::publish("mower/mi302/recon/wire_mag",  coil::magnitude());
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(sampleTask, "sample", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(netTask,    "net",    8192, nullptr, 3, nullptr, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
