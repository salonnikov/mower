// sniffer-2line-rtos — стабильный сниффер шлейфа дисплей<->мейнборд косилки.
//
// Подключение к нашей ESP32 (на плате косилки — стрелки на гребёнке OK STA GND ↓ ↑ ON 5V):
//   стрелка ↑  -> GPIO17  (канал A)
//   стрелка ↓  -> GPIO16  (канал B)
//   GND        -> GND     (общая земля — обязательно)
//   питание ESP — повербанк. 230400, JSON-протокол.
//
// Архитектура (надёжность = «прошил раз и забыл»):
//   * Ядро 1: captureTask — только чтение UART (реалтайм). Сеть его НЕ блокирует => не виснет.
//   * Ядро 0: netTask — Wi-Fi + TCP к VPS, авто-реконнект, слив буфера.
//   * Между ними FreeRTOS StreamBuffer 48К — переживает провалы Wi-Fi (мотает «за угол»).
//   Светодиод GPIO2: медленно мигает = связь есть, быстро = нет.

#include <Arduino.h>
#include <WiFi.h>
#include "freertos/stream_buffer.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef STA_SSID
  #define STA_SSID ""
  #define STA_PASS ""
#endif
#ifndef LOG_HOST
  #define LOG_HOST ""
  #define LOG_PORT 9000
#endif

static const int RX_UP = 17, RX_DN = 16, LED = 2;
static const uint32_t BAUD = 230400;
static const size_t SB_SIZE = 48 * 1024;

StreamBufferHandle_t sb;
WiFiClient cli;

// общий статус: netTask пишет, captureTask читает (простые типы; мелкие гонки косметичны)
volatile int      g_rssi = 0;
volatile bool     g_netUp = false;
volatile uint32_t g_reconn = 0, g_dropped = 0;
char              g_ip[24] = "-";

// единственный писатель буфера = captureTask
void sbLine(const char* s, size_t n) {
  if (xStreamBufferSpacesAvailable(sb) < n + 1) { g_dropped += n + 1; return; }  // полно — роняем новую строку
  xStreamBufferSend(sb, s, n, 0);
  uint8_t nl = '\n';
  xStreamBufferSend(sb, &nl, 1, 0);
}

struct Chan { HardwareSerial* ser; char tag; uint8_t buf[256]; int len; uint32_t last; };
Chan chA = {&Serial1, 'A', {0}, 0, 0};
Chan chB = {&Serial2, 'B', {0}, 0, 0};

void flushChan(Chan& c) {
  if (c.len == 0) return;
  char line[256 * 3 + 32];
  int p = snprintf(line, sizeof(line), "%c %lu %lu ", c.tag, (unsigned long)BAUD, (unsigned long)millis());
  for (int i = 0; i < c.len; i++) p += snprintf(line + p, sizeof(line) - p, "%02X ", c.buf[i]);
  sbLine(line, p);
  c.len = 0;
}
void pumpChan(Chan& c) {
  while (c.ser->available()) {
    c.buf[c.len++] = (uint8_t)c.ser->read();
    c.last = millis();
    if (c.len >= 256) flushChan(c);
  }
  if (c.len > 0 && millis() - c.last > 4) flushChan(c);   // кадр кончился (пауза >4мс)
}

void captureTask(void*) {
  Serial1.setRxBufferSize(2048);                 // большой HW RX на прерываниях — не теряет на переключениях задач
  Serial2.setRxBufferSize(2048);
  Serial1.begin(BAUD, SERIAL_8N1, RX_UP, -1);    // только RX
  Serial2.begin(BAUD, SERIAL_8N1, RX_DN, -1);
  uint32_t lastStatus = 0;
  for (;;) {
    pumpChan(chA);
    pumpChan(chB);
    if (millis() - lastStatus > 2000) {          // статус — через тот же буфер (один писатель)
      lastStatus = millis();
      char s[220];
      int n = snprintf(s, sizeof(s),
        "S {\"rssi\":%d,\"ssid\":\"" STA_SSID "\",\"ip\":\"%s\",\"baud\":%lu,\"buf\":%u,\"drop\":%lu,\"up\":%lu,\"rc\":%lu}",
        (int)g_rssi, g_ip, (unsigned long)BAUD,
        (unsigned)(SB_SIZE - xStreamBufferSpacesAvailable(sb)),
        (unsigned long)g_dropped, (unsigned long)(millis() / 1000), (unsigned long)g_reconn);
      sbLine(s, n);
    }
    vTaskDelay(1);
  }
}

void netTask(void*) {
  pinMode(LED, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(STA_SSID, STA_PASS);
  static uint8_t chunk[1024];
  uint32_t lastTry = 0, lastLed = 0; bool led = false;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      g_rssi = WiFi.RSSI();
      strncpy(g_ip, WiFi.localIP().toString().c_str(), sizeof(g_ip) - 1); g_ip[sizeof(g_ip) - 1] = 0;
      if (!cli.connected()) {
        g_netUp = false;
        if (millis() - lastTry > 2000 && strlen(LOG_HOST)) {
          lastTry = millis();
          if (cli.connect(LOG_HOST, LOG_PORT)) { cli.setNoDelay(true); g_reconn++; }
        }
        vTaskDelay(10);
      } else {
        g_netUp = true;
        int avail = cli.availableForWrite();
        if (avail > (int)sizeof(chunk)) avail = sizeof(chunk);
        if (avail > 0) {
          size_t got = xStreamBufferReceive(sb, chunk, avail, pdMS_TO_TICKS(20));
          if (got > 0 && cli.write(chunk, got) <= 0) cli.stop();
        } else {
          vTaskDelay(3);
        }
      }
    } else {
      g_netUp = false; strcpy(g_ip, "-");
      if (cli.connected()) cli.stop();
      if (millis() - lastTry > 5000) { lastTry = millis(); WiFi.disconnect(); WiFi.begin(STA_SSID, STA_PASS); }
      vTaskDelay(50);
    }
    uint32_t period = g_netUp ? 1000 : 150;       // LED: связь есть — медленно, нет — быстро
    if (millis() - lastLed > period) { lastLed = millis(); led = !led; digitalWrite(LED, led); }
    vTaskDelay(1);
  }
}

void setup() {
  Serial.begin(115200);
  sb = xStreamBufferCreate(SB_SIZE, 1);
  if (!sb) { Serial.println("StreamBuffer alloc FAILED"); }
  xTaskCreatePinnedToCore(captureTask, "cap", 6144, NULL, 10, NULL, 1);  // ядро 1 — реалтайм UART
  xTaskCreatePinnedToCore(netTask,     "net", 8192, NULL,  5, NULL, 0);  // ядро 0 — Wi-Fi/сеть
}

void loop() { vTaskDelay(1000); }
