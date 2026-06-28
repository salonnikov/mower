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
const int RX_A = 17, RX_B = 16;   // A=стрелка ↑ (GPIO17), B=стрелка ↓ (GPIO16)
uint32_t g_baud = 230400;         // НАЙДЕНО: протокол JSON на 230400

WebServer server(80);
WiFiClient logClient;

// --- кольцевой буфер исходящего: переживает обрывы Wi-Fi ---
#define OBUF 40000
static uint8_t obuf[OBUF];
static int ohead = 0, otail = 0;
static uint32_t g_drop = 0;
int bufUsed() { return (ohead - otail + OBUF) % OBUF; }
void bufPushByte(uint8_t c) {
  int nh = (ohead + 1) % OBUF;
  if (nh == otail) { otail = (otail + 1) % OBUF; g_drop++; }   // переполнение — выкидываем старое
  obuf[ohead] = c; ohead = nh;
}
void bufPush(const String& s) {
  for (uint16_t i = 0; i < s.length(); i++) bufPushByte((uint8_t)s[i]);
  bufPushByte('\n');
}
void bufDrain() {                          // сливаем буфер на сервер, не блокируя capture
  if (!logClient.connected()) return;
  int cap = 600;
  while (otail != ohead && cap > 0) {
    int chunk = (ohead > otail) ? (ohead - otail) : (OBUF - otail);
    if (chunk > cap) chunk = cap;
    int w = logClient.write(obuf + otail, chunk);
    if (w <= 0) break;
    otail = (otail + w) % OBUF; cap -= w;
  }
}
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
  bufPush(line);
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

const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>mower-sniff</title>
<style>body{font-family:system-ui,sans-serif;background:#0e1116;color:#cdd9e5;margin:0;padding:14px}
h2{margin:0 0 12px}.row{display:flex;gap:10px;flex-wrap:wrap}
.card{background:#171c24;border:1px solid #2a313c;border-radius:10px;padding:12px;flex:1;min-width:130px}
.k{color:#7a869a;font-size:12px}.v{font-size:20px;font-weight:600;margin-top:4px}
.ok{color:#3fb950}.no{color:#f85149}.warn{color:#d29922}
pre{background:#0a0d12;border:1px solid #2a313c;border-radius:8px;padding:10px;max-height:200px;overflow:auto;font-size:12px;white-space:pre-wrap;word-break:break-all}
.A{color:#6cf}.B{color:#fc6}</style>
<h2>📡 mower-sniff — состояние</h2>
<div class=row>
 <div class=card><div class=k>Режим</div><div class=v>Сниффер @<span id=bd>—</span></div></div>
 <div class=card><div class=k>Wi-Fi (<span id=ssid>—</span>)</div><div class=v id=rssi>—</div></div>
 <div class=card><div class=k>Связь с сервером</div><div class=v id=tcp>—</div></div>
 <div class=card><div class=k>Ошибки</div><div class=v id=err>нет</div></div>
</div>
<div class=row style=margin-top:10px>
 <div class=card><div class=k>Канал ↑ (A, GPIO17)</div><div class=v id=ca>0 Б</div></div>
 <div class=card><div class=k>Канал ↓ (B, GPIO16)</div><div class=v id=cb>0 Б</div></div>
</div>
<div class=card style=margin-top:10px><div class=k>Живые кадры (расшифровано)</div><pre id=log></pre></div>
<script>
function asc(parts){return parts.slice(3).map(h=>{let c=parseInt(h,16);return(c>=32&&c<127)?String.fromCharCode(c):'.';}).join('');}
async function tick(){try{let d=await(await fetch('/data')).json();
 bd.textContent=d.baud;ssid.textContent=d.ssid;ip&&0;
 rssi.innerHTML=d.ip!='—'?('<span class="'+(d.rssi>-67?'ok':d.rssi>-80?'warn':'no')+'">'+d.rssi+' dBm</span>'):'<span class=no>нет сети</span>';
 tcp.innerHTML=d.tcp?'<span class=ok>OK</span>':'<span class=no>нет</span>';
 let e=[]; if(d.ip=='—')e.push('нет Wi-Fi'); if(!d.tcp)e.push('нет сервера'); if(d.a==0&&d.b==0)e.push('нет данных от косилки');
 err.innerHTML=e.length?'<span class=warn>'+e.join(', ')+'</span>':'<span class=ok>нет</span>';
 ca.textContent=d.a.toLocaleString()+' Б';cb.textContent=d.b.toLocaleString()+' Б';
 log.innerHTML=d.lines.map(l=>{let p=l.split(' ');return '<span class="'+p[0]+'">'+p[0]+': '+asc(p)+'</span>';}).join('\n');
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
  pump(S_A, bufA, lenA, tA, 'A', cntA);
  pump(S_B, bufB, lenB, tB, 'B', cntB);
  ensureLog();
  bufDrain();
  server.handleClient();

  static uint32_t lastStatus = 0;          // статус в буфер раз в 2 с
  if (millis() - lastStatus > 2000) {
    lastStatus = millis();
    bufPush("S {\"rssi\":" + String(WiFi.RSSI()) + ",\"ssid\":\"" + WiFi.SSID() +
            "\",\"ip\":\"" + g_staIp + "\",\"baud\":" + String(g_baud) +
            ",\"a\":" + String(cntA) + ",\"b\":" + String(cntB) +
            ",\"buf\":" + String(bufUsed()) + ",\"drop\":" + String(g_drop) + "}");
  }
}
