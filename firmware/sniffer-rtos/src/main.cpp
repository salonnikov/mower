// sniffer-2line-rtos v2 — стабильный сниффер шлейфа дисплей<->мейнборд косилки.
//   ↑ -> GPIO17 (A), ↓ -> GPIO16 (B), GND -> GND. Питание — повербанк. 230400, JSON.
//
// Надёжность («сделать хорошо»):
//   * Ядро 1: captureTask — только чтение UART (реалтайм) -> статический кольцевой буфер (48К, lock-free).
//   * Ядро 0: netTask — Wi-Fi + TCP к VPS (авто-реконнект, детект полу-открытого сокета) + ЛОКАЛЬНАЯ веб-морда.
//   * ЛОКАЛЬНЫЙ интерфейс на самой ESP: своя точка "mower-sniff" (192.168.4.1) И по IP в домашней сети.
//     Видно состояние + живые кадры ДАЖЕ если VPS/Wi-Fi недоступны.
//   LED GPIO2: медленно = связь+слив идут, быстро = нет.

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
#ifndef LOG_HOST
  #define LOG_HOST ""
  #define LOG_PORT 9000
#endif

static const int RX_UP = 17, RX_DN = 16, LED = 2;
static const uint32_t BAUD = 230400;

// --- статический кольцевой буфер для VPS (writer=captureTask, reader=netTask) ---
static const uint32_t RB = 48 * 1024;
static uint8_t rb[RB];
volatile uint32_t rhead = 0, rtail = 0, g_dropped = 0;
static inline uint32_t rbUsed() { uint32_t h = rhead, t = rtail; return (h >= t) ? (h - t) : (RB - t + h); }
static inline uint32_t rbFree() { return RB - 1 - rbUsed(); }

// --- последние кадры для ЛОКАЛЬНОЙ веб-морды ---
static const int NREC = 60;
char recent[NREC][200];
volatile int recHead = 0;
volatile uint32_t cntA = 0, cntB = 0;

void rbPushLine(const char* s, int n) {
  // в локальный показ — всегда
  int m = n < 196 ? n : 196, idx = recHead;
  memcpy(recent[idx], s, m); recent[idx][m] = 0; recHead = (idx + 1) % NREC;
  // в буфер на VPS — если влезает
  if ((uint32_t)(n + 1) > rbFree()) { g_dropped += n + 1; return; }
  uint32_t h = rhead;
  for (int i = 0; i < n; i++) { rb[h] = (uint8_t)s[i]; h = (h + 1) % RB; }
  rb[h] = '\n'; h = (h + 1) % RB; rhead = h;
}

volatile int g_rssi = 0; volatile bool g_netUp = false; volatile uint32_t g_reconn = 0;
char g_ip[24] = "-";
WiFiClient cli;

struct Chan { HardwareSerial* ser; char tag; uint8_t buf[256]; int len; uint32_t last; };
Chan chA = {&Serial1, 'A', {0}, 0, 0};
Chan chB = {&Serial2, 'B', {0}, 0, 0};
void flushChan(Chan& c) {
  if (!c.len) return;
  if (c.tag == 'A') cntA += c.len; else cntB += c.len;
  char line[256 * 3 + 32];
  int p = snprintf(line, sizeof(line), "%c %lu %lu ", c.tag, (unsigned long)BAUD, (unsigned long)millis());
  for (int i = 0; i < c.len; i++) p += snprintf(line + p, sizeof(line) - p, "%02X ", c.buf[i]);
  rbPushLine(line, p); c.len = 0;
}
void pumpChan(Chan& c) {
  while (c.ser->available()) { c.buf[c.len++] = (uint8_t)c.ser->read(); c.last = millis(); if (c.len >= 256) flushChan(c); }
  if (c.len > 0 && millis() - c.last > 4) flushChan(c);
}

void captureTask(void*) {
  Serial1.setRxBufferSize(2048); Serial2.setRxBufferSize(2048);
  Serial1.begin(BAUD, SERIAL_8N1, RX_UP, -1);
  Serial2.begin(BAUD, SERIAL_8N1, RX_DN, -1);
  uint32_t lastStatus = 0;
  for (;;) {
    pumpChan(chA); pumpChan(chB);
    if (millis() - lastStatus > 2000) {
      lastStatus = millis();
      char s[220];
      int n = snprintf(s, sizeof(s),
        "S {\"rssi\":%d,\"ssid\":\"" STA_SSID "\",\"ip\":\"%s\",\"baud\":%lu,\"buf\":%lu,\"drop\":%lu,\"up\":%lu,\"rc\":%lu}",
        (int)g_rssi, g_ip, (unsigned long)BAUD, (unsigned long)rbUsed(),
        (unsigned long)g_dropped, (unsigned long)(millis() / 1000), (unsigned long)g_reconn);
      rbPushLine(s, n);
    }
    vTaskDelay(1);
  }
}

// --- локальная веб-морда ---
WebServer web(80);
const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>mower-sniff</title>
<style>body{font-family:system-ui,sans-serif;background:#0e1116;color:#cdd9e5;margin:0;padding:12px}
.row{display:flex;gap:8px;flex-wrap:wrap}.card{background:#171c24;border:1px solid #2a313c;border-radius:9px;padding:10px;flex:1;min-width:110px}
.k{color:#7a869a;font-size:12px}.v{font-size:18px;font-weight:600}.ok{color:#3fb950}.no{color:#f85149}.warn{color:#d29922}
pre{background:#0a0d12;border:1px solid #2a313c;border-radius:8px;padding:8px;max-height:300px;overflow:auto;font-size:12px;white-space:pre-wrap;word-break:break-all}.A{color:#6cf}.B{color:#fc6}.S{color:#9a9}</style>
<h3>&#128225; mower-sniff (локально на ESP)</h3>
<div class=row>
<div class=card><div class=k>Wi-Fi</div><div class=v id=rssi>&mdash;</div></div>
<div class=card><div class=k>VPS</div><div class=v id=srv>&mdash;</div></div>
<div class=card><div class=k>Буфер</div><div class=v id=buf>0</div></div>
<div class=card><div class=k>Потеряно</div><div class=v id=drop>0</div></div>
<div class=card><div class=k>&#8593; A</div><div class=v id=a>0</div></div>
<div class=card><div class=k>&#8595; B</div><div class=v id=b>0</div></div>
</div>
<p>Живые кадры (расшифровано):</p><pre id=log></pre>
<script>
function asc(p){return p.slice(3).map(h=>{let c=parseInt(h,16);return(c>=32&&c<127)?String.fromCharCode(c):'.';}).join('');}
async function t(){try{let d=await(await fetch('/data')).json();
rssi.innerHTML=d.ip!='-'?('<span class="'+(d.rssi>-67?'ok':d.rssi>-80?'warn':'no')+'">'+d.rssi+' dBm</span>'):'<span class=no>нет</span>';
srv.innerHTML=d.srv?'<span class=ok>OK</span>':'<span class=no>нет</span>';
buf.textContent=d.buf;drop.textContent=d.drop;a.textContent=d.a;b.textContent=d.b;
log.innerHTML=d.lines.map(l=>{let p=l.split(' ');let c=p[0];return '<span class="'+c+'">'+(c=='S'?l:(c+': '+asc(p)))+'</span>';}).join('\n');
}catch(e){}}
setInterval(t,500);t();
</script>)HTML";

String jsonEsc(const char* s) { String o; for (const char* p = s; *p; p++) { if (*p == '"' || *p == '\\') o += '\\'; o += *p; } return o; }
void handleData() {
  String j = "{\"rssi\":" + String(g_rssi) + ",\"ip\":\"" + String(g_ip) + "\",\"srv\":" + (cli.connected() ? "true" : "false") +
             ",\"buf\":" + String(rbUsed()) + ",\"drop\":" + String(g_dropped) +
             ",\"a\":" + String(cntA) + ",\"b\":" + String(cntB) + ",\"lines\":[";
  bool first = true;
  for (int i = 0; i < NREC; i++) {
    int idx = (recHead + i) % NREC;
    if (!recent[idx][0]) continue;
    if (!first) j += ','; first = false;
    j += "\"" + jsonEsc(recent[idx]) + "\"";
  }
  j += "]}";
  web.send(200, "application/json", j);
}

void netTask(void*) {
  pinMode(LED, OUTPUT);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("mower-sniff", "mower1234");          // локальная точка всегда
  WiFi.persistent(false); WiFi.setAutoReconnect(true);
  WiFi.begin(STA_SSID, STA_PASS);
  web.on("/", []() { web.send(200, "text/html", PAGE); });
  web.on("/data", handleData);
  web.begin();
  uint32_t lastTry = 0, lastLed = 0, lastProgress = 0; bool led = false;
  for (;;) {
    web.handleClient();
    if (WiFi.status() == WL_CONNECTED) {
      g_rssi = WiFi.RSSI();
      strncpy(g_ip, WiFi.localIP().toString().c_str(), sizeof(g_ip) - 1); g_ip[sizeof(g_ip) - 1] = 0;
      if (!cli.connected()) {
        g_netUp = false;
        if (millis() - lastTry > 2000 && strlen(LOG_HOST)) {
          lastTry = millis();
          if (cli.connect(LOG_HOST, LOG_PORT)) { cli.setNoDelay(true); g_reconn++; lastProgress = millis(); }
        }
      } else {
        g_netUp = true;
        uint32_t used = rbUsed();
        if (used) {
          int avail = cli.availableForWrite();
          if (avail > 512) avail = 512;
          if (avail > 0) {
            uint32_t t = rtail, chunk = (rhead >= t) ? (rhead - t) : (RB - t);
            if ((int)chunk > avail) chunk = avail;
            int w = cli.write(rb + t, chunk);
            if (w > 0) { rtail = (t + w) % RB; lastProgress = millis(); }
            else if (w < 0) cli.stop();
          }
        } else lastProgress = millis();
        if (used > 0 && millis() - lastProgress > 8000) cli.stop();   // полу-открытый сокет -> reconnect
      }
    } else {
      g_netUp = false; strcpy(g_ip, "-");
      if (cli.connected()) cli.stop();
      if (millis() - lastTry > 5000) { lastTry = millis(); WiFi.disconnect(); WiFi.begin(STA_SSID, STA_PASS); }
    }
    uint32_t period = g_netUp ? 1000 : 150;
    if (millis() - lastLed > period) { lastLed = millis(); led = !led; digitalWrite(LED, led); }
    vTaskDelay(2);
  }
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(captureTask, "cap", 6144, NULL, 10, NULL, 1);
  xTaskCreatePinnedToCore(netTask,     "net", 8192, NULL,  5, NULL, 0);
}
void loop() { vTaskDelay(1000); }
