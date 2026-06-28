// mower-auto — ESP32 автономно. Подключение к J1 косилки:
//   J1 T -> GPIO16, J1 R -> GPIO17, J1 P -> GPIO5, J1 GND -> GND
//   Питание ESP — повербанк. (картинка: hardware/wiring-esp.png; пины те же)
//
// Логика: подключается к домашнему Wi-Fi, дозванивается на сервер:
//   data  (SRV_HOST:DATA_PORT) — двунаправленный мост UART<->сервер (сниф И дамп);
//   ctrl  (SRV_HOST:CTRL_PORT) — сервер шлёт '0' (P=LOW, режим дампа) или '1' (P release, сниф).
// Никаких кнопок: оркестратор на сервере сам решает, что делать.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef STA_SSID
  #define STA_SSID ""
  #define STA_PASS ""
#endif
#ifndef SRV_HOST
  #define SRV_HOST ""
  #define DATA_PORT 7777
  #define CTRL_PORT 7778
#endif

const int PIN_T = 16, PIN_R = 17, PIN_P = 5;
HardwareSerial Mow(2);
WiFiClient dataC, ctrlC;
WebServer server(80);
uint32_t lastD = 0, lastC = 0, rx = 0, tx = 0;
char pstate = '1';

const char* PAGE_FMT =
  "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<style>body{font-family:monospace;background:#111;color:#6f6;margin:10px}</style>"
  "<h3>mower-auto</h3>IP: %s<br>сервер data: %s<br>сервер ctrl: %s<br>"
  "P: %s<br>от косилки (rx): %u байт<br>в косилку (tx): %u байт<br>"
  "<p>Режимом рулит сервер. Просто включи и (для дампа) передёрни питание косилки.</p>";

void handleRoot() {
  char buf[600];
  snprintf(buf, sizeof(buf), PAGE_FMT,
           WiFi.localIP().toString().c_str(),
           dataC.connected() ? "OK" : "нет",
           ctrlC.connected() ? "OK" : "нет",
           pstate == '0' ? "LOW (дамп)" : "release (сниф)", rx, tx);
  server.send(200, "text/html", buf);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_P, INPUT);                 // P отпущен по умолчанию (сниф)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("mower-auto", "mower1234");
  WiFi.begin(STA_SSID, STA_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
  Mow.begin(115200, SERIAL_8N1, PIN_T, PIN_R);
  server.on("/", handleRoot);
  server.begin();
}

void loop() {
  server.handleClient();
  if (!dataC.connected() && millis() - lastD > 2000) { lastD = millis(); dataC.connect(SRV_HOST, DATA_PORT); }
  if (!ctrlC.connected() && millis() - lastC > 2000) { lastC = millis(); ctrlC.connect(SRV_HOST, CTRL_PORT); }

  while (ctrlC.connected() && ctrlC.available()) {       // команда P от сервера
    char c = ctrlC.read();
    if (c == '0') { pstate = '0'; pinMode(PIN_P, OUTPUT); digitalWrite(PIN_P, LOW); }
    else if (c == '1') { pstate = '1'; pinMode(PIN_P, INPUT); }
  }

  if (dataC.connected()) {                                // мост UART<->сервер
    while (dataC.available()) { Mow.write(dataC.read()); tx++; }
    while (Mow.available())   { dataC.write(Mow.read());  rx++; }
  } else {
    while (Mow.available()) Mow.read();
  }
}
