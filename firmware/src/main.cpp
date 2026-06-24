// main.cpp — каркас мозга косилки VILLARTEC MI 302 на ESP32-S3.
// Два ядра FreeRTOS: Core0 = реал-тайм (офлайн-safe), Core1 = сеть.
// Это СКЕЛЕТ: тела модулей (perimeter/motor/imu/...) подключаются по мере фаз.
// Алгоритмы перименто/PID/docking — порт Ardumower Azurit (GPL), см. docs/06.

#include <Arduino.h>

// --- Заглушки модулей (реализация в src/<module>/) ---
namespace perimeter { void begin(); float signedMagnitude(); }  // знак=внутри/снаружи, |.|=дистанция
namespace motor     { void begin(); void setSpeed(float l, float r); void blade(bool on); }
namespace odom      { void begin(); float headingFromWheels(); }
namespace imu       { void begin(); float heading(); bool tilted(); }
namespace safety    { void begin(); bool liftDetected(); bool overcurrent(); }
namespace net       { void begin(); void publishTelemetry(); bool popCommand(int& cmd); }

// --- Состояния поведения (фикс «дока» живёт в RETURN/DOCK) ---
enum class State { IDLE, MOWING, FOLLOW_WIRE, RETURN, DOCK, CHARGING, ESTOP };
volatile State g_state = State::IDLE;
volatile bool  g_bladeAllowed = false;   // fail-safe: по умолчанию нож запрещён

// ============ Core0: real-time control (НЕ зависит от Wi-Fi) ============
void controlTask(void*) {
  perimeter::begin(); motor::begin(); odom::begin(); imu::begin(); safety::begin();
  const TickType_t dt = pdMS_TO_TICKS(20);   // 50 Гц цикл управления
  for (;;) {
    // 1) Безопасность — приоритет над всем (локально, без сервера)
    if (safety::liftDetected() || imu::tilted() || safety::overcurrent()) {
      g_state = State::ESTOP;
    }
    float wire = perimeter::signedMagnitude();   // + внутри / − снаружи, |.|=близость

    switch (g_state) {
      case State::ESTOP:
        g_bladeAllowed = false; motor::blade(false); motor::setSpeed(0, 0);
        break;
      case State::MOWING:
        g_bladeAllowed = true; motor::blade(true);
        if (wire < 0) g_state = State::FOLLOW_WIRE;   // вышли за провод → к проводу
        else motor::setSpeed(0.6f, 0.6f);             // прямо (упрощённо)
        break;
      case State::FOLLOW_WIRE:
        // PID по |wire| удерживает дистанцию до провода → нет «кручения на месте»
        // TODO: реальный регулятор (порт Azurit)
        break;
      case State::RETURN:
      case State::DOCK:
        // следование по проводу + одометрия/IMU + детект напряжения контактов
        // TODO: точная стыковка (фикс промаха), см. docs/05 §B
        break;
      default:
        motor::setSpeed(0, 0); motor::blade(false);
    }
    vTaskDelay(dt);
  }
}

// ================= Core1: network (Wi-Fi/MQTT/OTA) =================
void netTask(void*) {
  net::begin();
  for (;;) {
    int cmd;
    while (net::popCommand(cmd)) {
      // 0=start 1=stop 2=home 3=estop — кладём в g_state через очередь/мьютекс
    }
    net::publishTelemetry();          // состояние, wire, курс, токи, GPS-трек
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void setup() {
  Serial.begin(115200);
  // Core0 — control (real-time), Core1 — net
  xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(netTask,     "net",     8192, nullptr, 3, nullptr, 1);
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }  // вся работа в задачах
