// mower-link — ESP32, 2 режима. Подключение к плате дисплея косилки (разъём J1):
//   J1 T   -> наш GPIO16   (RX: мейнборд-сторона / TX косилкиной ESP)
//   J1 R   -> наш GPIO17   (SNIFF: слушаем; BRIDGE: наш TX в косилкину ESP)
//   J1 P   -> наш GPIO5    (BRIDGE: держим в GND = вход в загрузчик)
//   J1 GND -> наш GND      (общая земля — обязательно)
// Питание нашей ESP — свой USB/повербанк. 3V3 c J1 НЕ брать.
//
// Сеть: цепляется к домашней (secrets.h) + своя точка "mower-link".
//   веб: http://mower-link.local/  (или IP, или точка) — переключение режимов.
//
// SNIFF: обе линии -> TCP-логгер (LOG_HOST:9000) + последние строки на странице.
// BRIDGE: TCP :3333 <-> UART2(16/17) @115200, GPIO5(P)=LOW. Дамп — esptool с сервера:
//   esptool --before no_reset --after no_reset --chip esp32 \
//           --port socket://<ip-этой-esp>:3333 read_flash 0 0x400000 mower-esp.bin
//   (перед запуском: войти в BRIDGE, затем ПЕРЕДЁРНУТЬ питание косилки — ESP уйдёт в загрузчик)
// read_flash — ТОЛЬКО чтение, ничего не пишем/не стираем.

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
#ifndef BRIDGE_HOST
  #define BRIDGE_HOST ""
  #define BRIDGE_PORT 7777
#endif

const char* AP_SSID = "mower-link";
const char* AP_PASS = "mower1234";
const int PIN_T = 16, PIN_R = 17, PIN_P = 5;

enum Mode { SNIFF, BRIDGE };
Mode mode = SNIFF;
uint32_t g_baud = 115200;
String g_staIp = "—";

HardwareSerial SerT(2);   // SNIFF: RX=16 (канал A);  BRIDGE: RX=16/TX=17 (мост)
HardwareSerial SerR(1);   // SNIFF: RX=17 (канал B)

WebServer server(80);
WiFiClient bridgeClient, logClient;   // bridgeClient = ИСХОДЯЩИЙ дозвон на сервер
uint32_t lastBridge = 0;
volatile uint32_t cntA = 0, cntB = 0;

uint8_t bA[256]; int lA = 0; uint32_t tA = 0;
uint8_t bB[256]; int lB = 0; uint32_t tB = 0;

#define NLINES 30
String lines[NLINES]; int lhead = 0;
void addLine(const String& s){ lines[lhead]=s; lhead=(lhead+1)%NLINES; }
void emit(char ch, uint8_t* b, int n, uint32_t t){
  String hex; char h[4];
  for(int i=0;i<n;i++){ snprintf(h,4,"%02X ",b[i]); hex+=h; }
  String line=String(ch)+" "+String(t)+" "+hex;
  addLine(line);
  if(logClient.connected()) logClient.println(line);
}
void pump(HardwareSerial& S, uint8_t* b, int& n, uint32_t& t, char ch, volatile uint32_t& c){
  while(S.available()){ b[n++]=S.read(); c++; t=millis(); if(n>=256){ emit(ch,b,n,t); n=0; } }
  if(n>0 && millis()-t>4){ emit(ch,b,n,t); n=0; }
}

void setSniff(){
  mode=SNIFF;
  if(bridgeClient) bridgeClient.stop();
  pinMode(PIN_P, INPUT);                 // P отпускаем (Hi-Z) — не мешаем
  SerT.begin(g_baud, SERIAL_8N1, PIN_T, -1);
  SerR.begin(g_baud, SERIAL_8N1, PIN_R, -1);
  cntA=cntB=0; lA=lB=0;
}
void setBridge(){
  mode=BRIDGE;
  SerR.end();
  pinMode(PIN_P, OUTPUT); digitalWrite(PIN_P, LOW);   // держим IO0 косилкиной ESP низким
  SerT.begin(115200, SERIAL_8N1, PIN_T, PIN_R);       // двунаправленно к ESP косилки
  if(bridgeClient.connected()) bridgeClient.stop();
  lastBridge = 0;                                     // дозвон на сервер в loop()
}

uint32_t lastTcp=0;
void ensureLog(){
  if(strlen(LOG_HOST)==0 || logClient.connected()) return;
  if(millis()-lastTcp<2000) return; lastTcp=millis();
  logClient.connect(LOG_HOST, LOG_PORT);
}

const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>mower-link</title>
<style>body{font-family:monospace;background:#111;color:#6f6;margin:8px;font-size:12px}
b{color:#fff}.b{display:inline-block;background:#225;color:#fff;padding:7px 11px;margin:3px;border-radius:5px;text-decoration:none}
.on{background:#2a2}#log{white-space:pre-wrap;word-break:break-all}.A{color:#6cf}.B{color:#fc6}</style>
<h3>mower-link</h3>
<div>режим:<b id=md>—</b> дом:<span id=ip>—</span> baud:<b id=bd>—</b></div>
<div><a class=b href="/mode?m=sniff">SNIFF</a><a class=b href="/mode?m=bridge">BRIDGE (дамп)</a></div>
<div id=sn>
 <div>TCP-лог:<b id=tcp>—</b> A:<b id=ca>0</b> B:<b id=cb>0</b></div>
 <div>baud:<a class=b href="/baud?b=9600">9600</a><a class=b href="/baud?b=19200">19200</a>
 <a class=b href="/baud?b=38400">38400</a><a class=b href="/baud?b=57600">57600</a><a class=b href="/baud?b=115200">115200</a></div>
 <div id=log></div>
</div>
<div id=br style=display:none>
 <p>BRIDGE активен. P(IO0) держится в GND, мост дозванивается на сервер.</p>
 <p>связь с сервером:<b id=bc>—</b></p>
 <p>Теперь просто <b>ПЕРЕДЁРНИ питание косилки</b> — дамп снимется на сервере автоматически.</p>
</div>
<script>
async function tick(){try{let d=await(await fetch('/data')).json();
md.textContent=d.mode;ip.textContent=d.ip;bd.textContent=d.baud;
sn.style.display=d.mode=='SNIFF'?'block':'none';br.style.display=d.mode=='BRIDGE'?'block':'none';
if(d.mode=='SNIFF'){tcp.textContent=d.tcp?'OK':'нет';ca.textContent=d.a;cb.textContent=d.b;
log.innerHTML=d.lines.map(l=>'<span class="'+l[0]+'">'+l+'</span>').join('\n');}
else{bc.textContent=d.bc?'подключён':'нет';}
}catch(e){}}
setInterval(tick,500);tick();
</script>)HTML";

String jsonData(){
  String s="{\"mode\":\""+String(mode==SNIFF?"SNIFF":"BRIDGE")+"\",\"ip\":\""+g_staIp+"\",\"baud\":"+String(g_baud);
  s+=",\"tcp\":"+String(logClient.connected()?"true":"false")+",\"a\":"+String(cntA)+",\"b\":"+String(cntB);
  s+=",\"bc\":"+String((bridgeClient&&bridgeClient.connected())?"true":"false")+",\"lines\":[";
  bool first=true;
  for(int i=0;i<NLINES;i++){ String& l=lines[(lhead+i)%NLINES]; if(!l.length()) continue;
    if(!first) s+=','; first=false; String e;
    for(uint16_t k=0;k<l.length();k++){ char c=l[k]; if(c=='"'||c=='\\') e+='\\'; e+=c; } s+="\""+e+"\""; }
  s+="]}"; return s;
}

void setup(){
  Serial.begin(115200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASS)>=8?AP_PASS:nullptr);
  if(strlen(STA_SSID)){ WiFi.begin(STA_SSID, STA_PASS);
    for(int i=0;i<40 && WiFi.status()!=WL_CONNECTED;i++) delay(250);
    if(WiFi.status()==WL_CONNECTED) g_staIp=WiFi.localIP().toString(); }
  MDNS.begin("mower-link");
  server.on("/", [](){ server.send(200,"text/html",PAGE); });
  server.on("/data", [](){ server.send(200,"application/json",jsonData()); });
  server.on("/mode", [](){ String m=server.arg("m"); if(m=="bridge") setBridge(); else setSniff();
    server.sendHeader("Location","/"); server.send(302,"",""); });
  server.on("/baud", [](){ if(mode==SNIFF && server.hasArg("b")){ g_baud=server.arg("b").toInt(); setSniff(); }
    server.sendHeader("Location","/"); server.send(302,"",""); });
  server.begin();
  setSniff();
}

void loop(){
  server.handleClient();
  if(mode==SNIFF){
    pump(SerT,bA,lA,tA,'A',cntA);
    pump(SerR,bB,lB,tB,'B',cntB);
    ensureLog();
  } else { // BRIDGE: дозваниваемся на сервер и мостим UART<->сокет
    if(!bridgeClient.connected()){
      if(strlen(BRIDGE_HOST) && millis()-lastBridge>2000){
        lastBridge=millis(); bridgeClient.connect(BRIDGE_HOST, BRIDGE_PORT);
      }
      while(SerT.available()) SerT.read();   // нет связи — сливаем
    }
    if(bridgeClient.connected()){
      while(bridgeClient.available()) SerT.write(bridgeClient.read());
      while(SerT.available()) bridgeClient.write(SerT.read());
    }
  }
}
