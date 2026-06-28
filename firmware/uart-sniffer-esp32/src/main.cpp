// Дуал-UART сниффер на ESP32 — слушает ОБЕ линии J1 платы дисплея косилки.
//   J1 T (TX ESP32 дисплея, дисплей->мейнборд)  -> ESP32-сниффер GPIO4  (канал A)
//   J1 R (RX ESP32 дисплея, мейнборд->дисплей)  -> ESP32-сниффер GPIO16 (канал B)
//   J1 GND                                       -> ESP32-сниффер GND (общая земля!)
//   Питание сниффера — свой USB/повербанк. Линии только СЛУШАЕМ (RX), не трогаем TX.
//
// Куда смотреть: цепляется к домашней сети (secrets.h), плюс своя точка "mower-sniff2".
//   - веб: http://<ip>/  (или http://mower-sniff2.local/) — статус + последние строки;
//   - весь поток стримится по TCP в контейнер-логгер (LOG_HOST:LOG_PORT) — полный лог в файл.
//
// Формат строки лога:  "<A|B> <millis> <hex-байты>"
// Baud по умолчанию 115200 (грузится ESP32 косилки); меняется кнопками на странице.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

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

const char* AP_SSID = "mower-sniff2";
const char* AP_PASS = "mower1234";

HardwareSerial S_A(1);     // канал A: J1 T
HardwareSerial S_B(2);     // канал B: J1 R
const int RX_A = 4, RX_B = 16;
uint32_t g_baud = 115200;

WebServer server(80);
WiFiClient logClient;
String   g_staIp = "—";
volatile uint32_t cntA = 0, cntB = 0;

uint8_t bufA[256]; int lenA = 0; uint32_t tA = 0;
uint8_t bufB[256]; int lenB = 0; uint32_t tB = 0;

#define NLINES 40
String lines[NLINES]; int lhead = 0;
void addLine(const String& s) { lines[lhead] = s; lhead = (lhead + 1) % NLINES; }

void emit(char ch, uint8_t* buf, int len, uint32_t t) {
  String hex; char h[4];
  for (int i = 0; i < len; i++) { snprintf(h, 4, "%02X ", buf[i]); hex += h; }
  String line = String(ch) + " b" + String(g_baud) + " " + String(t) + " " + hex;
  addLine(line);
  if (logClient.connected()) logClient.println(line);
}

void pump(HardwareSerial& S, uint8_t* buf, int& len, uint32_t& t, char ch, volatile uint32_t& cnt) {
  while (S.available()) {
    buf[len++] = S.read(); cnt++; t = millis();
    if (len >= 256) { emit(ch, buf, len, t); len = 0; }
  }
  if (len > 0 && millis() - t > 4) { emit(ch, buf, len, t); len = 0; }  // флуш по паузе = конец кадра
}

void applyBaud(uint32_t b) {
  g_baud = b;
  S_A.begin(b, SERIAL_8N1, RX_A, -1);   // только RX
  S_B.begin(b, SERIAL_8N1, RX_B, -1);
  cntA = cntB = 0; lenA = lenB = 0;
}

uint32_t lastTcp = 0;
void ensureLog() {
  if (strlen(LOG_HOST) == 0 || logClient.connected()) return;
  if (millis() - lastTcp < 2000) return;
  lastTcp = millis();
  logClient.connect(LOG_HOST, LOG_PORT);
}

const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>mower-sniff2</title>
<style>body{font-family:monospace;background:#111;color:#6f6;margin:8px;font-size:12px}
b{color:#fff}.b{display:inline-block;background:#225;color:#fff;padding:6px 9px;margin:2px;border-radius:5px;text-decoration:none}
#log{white-space:pre-wrap;word-break:break-all}.A{color:#6cf}.B{color:#fc6}</style>
<h3>UART-сниффер (A=T дисплей→мейн, B=R мейн→дисплей)</h3>
<div>baud:<b id=bd>—</b> дом:<span id=ip>—</span> TCP:<b id=tcp>—</b> A:<b id=ca>0</b> B:<b id=cb>0</b></div>
<div>baud:
<a class=b href="/baud?b=9600">9600</a><a class=b href="/baud?b=19200">19200</a>
<a class=b href="/baud?b=38400">38400</a><a class=b href="/baud?b=57600">57600</a>
<a class=b href="/baud?b=115200">115200</a><a class=b href="/baud?b=230400">230400</a>
<a class=b href="/baud?b=921600">921600</a></div>
<div id=log></div>
<script>
async function tick(){try{let d=await(await fetch('/data')).json();
bd.textContent=d.baud;ip.textContent=d.ip;tcp.textContent=d.tcp?'OK':'нет';
ca.textContent=d.a;cb.textContent=d.b;
log.innerHTML=d.lines.map(l=>'<span class="'+l[0]+'">'+l+'</span>').join('\n');
}catch(e){}}
setInterval(tick,400);tick();
</script>)HTML";

String jsonData() {
  String s = "{\"baud\":" + String(g_baud) + ",\"ip\":\"" + g_staIp + "\",\"tcp\":" +
             (logClient.connected() ? "true" : "false") + ",\"a\":" + String(cntA) +
             ",\"b\":" + String(cntB) + ",\"lines\":[";
  bool first = true;
  for (int i = 0; i < NLINES; i++) {
    String& l = lines[(lhead + i) % NLINES];
    if (l.length() == 0) continue;
    if (!first) s += ',';
    first = false;
    String e; for (uint16_t k = 0; k < l.length(); k++) { char c = l[k]; if (c == '"' || c == '\\') e += '\\'; e += c; }
    s += "\"" + e + "\"";
  }
  s += "]}";
  return s;
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASS) >= 8 ? AP_PASS : nullptr);
  if (strlen(STA_SSID)) {
    WiFi.begin(STA_SSID, STA_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    if (WiFi.status() == WL_CONNECTED) g_staIp = WiFi.localIP().toString();
  }
  MDNS.begin("mower-sniff2");

  server.on("/", []() { server.send(200, "text/html", PAGE); });
  server.on("/data", []() { server.send(200, "application/json", jsonData()); });
  server.on("/baud", []() {
    if (server.hasArg("b")) applyBaud(server.arg("b").toInt());
    server.sendHeader("Location", "/"); server.send(302, "text/plain", "");
  });
  server.begin();

  applyBaud(g_baud);
}

void loop() {
  // авто-перебор baud по кругу (~1.6 с на каждый) — найдём правильный по логу
  static const uint32_t BAUDS[] = {9600, 19200, 38400, 57600, 115200, 230400, 921600};
  static uint32_t lastSwap = 0; static int bi = 4;
  if (millis() - lastSwap > 1600) { lastSwap = millis(); bi = (bi + 1) % 7; applyBaud(BAUDS[bi]); }

  pump(S_A, bufA, lenA, tA, 'A', cntA);
  pump(S_B, bufB, lenB, tB, 'B', cntB);
  ensureLog();
  server.handleClient();
}
