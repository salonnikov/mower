// UART-снифер USB-порта косилки — Wemos D1 mini (ESP8266).
// Подключение к порту косилки:
//   USB GND (пин 4)        -> D1 mini  G (GND)
//   USB D+ (пин 3) или D-  -> D1 mini  RX   (через резистор ~1к, если есть)
//   (питание D1 mini — от своего USB/повербанка; либо USB +5V косилки -> 5V D1)
//
// Смотрим с телефона: сеть "mower-sniff" (пароль mower1234) -> http://192.168.4.1/
// (или по домашней сети / http://mower-sniff.local/)
//
// baud неизвестен — перебираем кнопками на странице. Ищем «осмысленный» поток.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef STA_SSID
  #define STA_SSID ""
  #define STA_PASS ""
#endif

const char* AP_SSID = "mower-sniff";
const char* AP_PASS = "mower1234";

ESP8266WebServer server(80);
String   g_staIp = "—";

uint32_t g_baud  = 9600;
uint32_t g_count = 0;            // всего принято байт
#define  BUFN 256
uint8_t  g_buf[BUFN];
int      g_head = 0;            // кольцевой буфер последних байт

void applyBaud(uint32_t b) {
  g_baud = b; g_count = 0; g_head = 0;
  Serial.end(); delay(5);
  Serial.begin(b);            // RX = GPIO3 (пин "RX" на D1 mini)
}

String dumpHex() {
  String s; char t[4];
  for (int i = 0; i < BUFN; i++) {
    uint8_t c = g_buf[(g_head + i) % BUFN];
    snprintf(t, sizeof(t), "%02X ", c); s += t;
  }
  return s;
}
String dumpAsc() {
  String s;
  for (int i = 0; i < BUFN; i++) {
    uint8_t c = g_buf[(g_head + i) % BUFN];
    s += (c >= 32 && c < 127) ? (char)c : '.';
  }
  return s;
}

const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>mower-sniff</title>
<style>body{font-family:monospace;background:#111;color:#6f6;margin:8px;font-size:13px}
b{color:#fff}.b{display:inline-block;background:#225;color:#fff;padding:6px 10px;margin:2px;border-radius:5px;text-decoration:none}
#hex{color:#9c9;word-break:break-all}#asc{color:#fc6;word-break:break-all}</style>
<h3>USB-снифер косилки</h3>
<div>baud: <b id=bd>—</b> &nbsp; принято байт: <b id=cnt>—</b> &nbsp; дом: <span id=ip>—</span></div>
<div>скорость:
<a class=b href="/baud?b=9600">9600</a><a class=b href="/baud?b=19200">19200</a>
<a class=b href="/baud?b=38400">38400</a><a class=b href="/baud?b=57600">57600</a>
<a class=b href="/baud?b=115200">115200</a></div>
<p>HEX:</p><div id=hex></div>
<p>TEXT:</p><div id=asc></div>
<script>
async function tick(){try{let d=await(await fetch('/data')).json();
bd.textContent=d.baud;cnt.textContent=d.count;ip.textContent=d.ip;
hex.textContent=d.hex;asc.textContent=d.asc;}catch(e){}}
setInterval(tick,400);tick();
</script>)HTML";

void handleData() {
  String s = "{\"baud\":" + String(g_baud) + ",\"count\":" + String(g_count) +
             ",\"ip\":\"" + g_staIp + "\",\"hex\":\"" + dumpHex() + "\",\"asc\":\"";
  // экранируем " и \ в ASCII-дампе
  String a = dumpAsc(), e;
  for (uint16_t i = 0; i < a.length(); i++) { char c = a[i]; if (c=='"'||c=='\\') e += '\\'; e += c; }
  s += e + "\"}";
  server.send(200, "application/json", s);
}

void setup() {
  Serial.begin(g_baud);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) >= 8 ? AP_PASS : nullptr);
  if (strlen(STA_SSID)) {
    WiFi.begin(STA_SSID, STA_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    if (WiFi.status() == WL_CONNECTED) g_staIp = WiFi.localIP().toString();
  }
  MDNS.begin("mower-sniff");

  server.on("/", []() { server.send(200, "text/html", PAGE); });
  server.on("/data", handleData);
  server.on("/baud", []() {
    if (server.hasArg("b")) applyBaud(server.arg("b").toInt());
    server.sendHeader("Location", "/"); server.send(302, "text/plain", "");
  });
  server.begin();
}

void loop() {
  while (Serial.available()) {
    g_buf[g_head] = (uint8_t)Serial.read();
    g_head = (g_head + 1) % BUFN;
    g_count++;
  }
  MDNS.update();
  server.handleClient();
}
