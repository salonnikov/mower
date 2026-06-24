// coil-scope — «осциллограф из ESP32» для родного сигнала провода периметра.
// Катушка (из витой пары) → GPIO36 (ADC1_CH0). Питание — повербанк по USB.
// Веб-морда: http://<ip-esp>/  — форма сигнала + спектр (FFT) + пиковая частота.
// MQTT: mower/mi302/recon/wire_freq, /wire_mag.
//
// Настройка БЕЗ пересборки (WiFiManager): при первом включении ESP поднимает
// точку "coil-scope-setup". Подключись телефоном → откроется форма → впиши Wi-Fi
// и адрес брокера. Данные сохраняются во flash. Чтобы сбросить — держи кнопку
// BOOT (GPIO0) при включении.
//
// ВЕРСИЯ 1 (first light): цель — увидеть ЕСТЬ ли сигнал, на какой ЧАСТОТЕ, и форму.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>          // tzapu/WiFiManager
#include <Preferences.h>
#include <PubSubClient.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <driver/adc.h>

// АЦП/выборка
static const adc1_channel_t ADC_CH = ADC1_CHANNEL_0;   // GPIO36 (VP)
static const i2s_port_t I2S_PORT  = I2S_NUM_0;
static const uint32_t SAMPLE_RATE = 80000;             // Гц → Найквист 40 кГц
static const uint16_t SAMPLES     = 1024;              // окно FFT (бин = 78.1 Гц)

// настройки брокера (грузятся из flash / задаются с телефона)
char     mqtt_host[40] = "192.168.1.10";
char     mqtt_port_s[6] = "1883";
char     mqtt_user[24] = "";
char     mqtt_pass[24] = "";
uint16_t mqtt_port = 1883;

double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, (double)SAMPLE_RATE);

volatile double g_peakFreq = 0, g_peakMag = 0;
uint16_t g_wave[256];
uint16_t g_spec[256];

WebServer    server(80);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
Preferences  prefs;

// ---------- I2S ADC ----------
void adcSetup() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
  };
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_CH, ADC_ATTEN_DB_11);
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_adc_mode(ADC_UNIT_1, ADC_CH);
  i2s_adc_enable(I2S_PORT);
}

void sampleBlock() {
  static uint16_t buf[SAMPLES];
  size_t bytesRead = 0, need = SAMPLES * sizeof(uint16_t), got = 0;
  while (got < need) {
    i2s_read(I2S_PORT, ((uint8_t*)buf) + got, need - got, &bytesRead, portMAX_DELAY);
    got += bytesRead;
  }
  for (uint16_t i = 0; i < SAMPLES; i++) {
    vReal[i] = (double)(buf[i] & 0x0FFF);
    vImag[i] = 0.0;
  }
}

void analyzeBlock() {
  for (int i = 0; i < 256; i++) g_wave[i] = (uint16_t)vReal[i * (SAMPLES / 256)];
  double mean = 0; for (uint16_t i = 0; i < SAMPLES; i++) mean += vReal[i];
  mean /= SAMPLES;
  for (uint16_t i = 0; i < SAMPLES; i++) { vReal[i] -= mean; vImag[i] = 0; }
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();
  g_peakFreq = FFT.majorPeak();
  double maxMag = 0;
  for (int i = 0; i < 256; i++) {
    double m = vReal[1 + i * 2];
    g_spec[i] = (uint16_t)constrain(m, 0, 65535);
    if (m > maxMag) maxMag = m;
  }
  g_peakMag = maxMag;
}

// ---------- Веб ----------
const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>coil-scope</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;margin:8px}
canvas{background:#000;display:block;margin:6px 0;width:100%;height:160px}
b{color:#6f6;font-size:20px}</style>
<h3>coil-scope — сигнал провода</h3>
<div>Пиковая частота: <b id=f>—</b> Гц &nbsp; амплитуда: <span id=m>—</span></div>
<div>Форма сигнала:</div><canvas id=w width=512 height=160></canvas>
<div>Спектр (0…40 кГц):</div><canvas id=s width=512 height=160></canvas>
<script>
function draw(c,arr,max){let x=c.getContext('2d'),W=c.width,H=c.height;x.clearRect(0,0,W,H);
x.strokeStyle='#6f6';x.beginPath();for(let i=0;i<arr.length;i++){let y=H-arr[i]/max*H;
i?x.lineTo(i/arr.length*W,y):x.moveTo(0,y);}x.stroke();}
async function tick(){try{let d=await(await fetch('/data')).json();
document.getElementById('f').textContent=d.freq.toFixed(0);
document.getElementById('m').textContent=d.mag.toFixed(0);
draw(document.getElementById('w'),d.wave,4096);
draw(document.getElementById('s'),d.spec,Math.max(...d.spec,1));}catch(e){}}
setInterval(tick,300);tick();
</script>)HTML";

String jsonData() {
  String s = "{\"freq\":" + String(g_peakFreq, 1) + ",\"mag\":" + String(g_peakMag, 0) + ",\"wave\":[";
  for (int i = 0; i < 256; i++) { s += g_wave[i]; if (i < 255) s += ','; }
  s += "],\"spec\":[";
  for (int i = 0; i < 256; i++) { s += g_spec[i]; if (i < 255) s += ','; }
  s += "]}";
  return s;
}

void mqttEnsure() {
  if (mqtt.connected()) return;
  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.connect("coil-scope", strlen(mqtt_user) ? mqtt_user : nullptr,
                             strlen(mqtt_pass) ? mqtt_pass : nullptr);
}

void setup() {
  Serial.begin(115200);

  // загрузить сохранённые настройки брокера
  prefs.begin("cfg", false);
  prefs.getString("mhost", "192.168.1.10").toCharArray(mqtt_host, sizeof(mqtt_host));
  prefs.getString("mport", "1883").toCharArray(mqtt_port_s, sizeof(mqtt_port_s));
  prefs.getString("muser", "").toCharArray(mqtt_user, sizeof(mqtt_user));
  prefs.getString("mpass", "").toCharArray(mqtt_pass, sizeof(mqtt_pass));

  // сброс настроек по кнопке BOOT (GPIO0) при включении
  pinMode(0, INPUT_PULLUP);

  WiFiManager wm;
  if (digitalRead(0) == LOW) wm.resetSettings();

  WiFiManagerParameter p_host("host", "MQTT broker IP", mqtt_host, sizeof(mqtt_host));
  WiFiManagerParameter p_port("port", "MQTT port", mqtt_port_s, sizeof(mqtt_port_s));
  WiFiManagerParameter p_user("user", "MQTT user (опц.)", mqtt_user, sizeof(mqtt_user));
  WiFiManagerParameter p_pass("pass", "MQTT pass (опц.)", mqtt_pass, sizeof(mqtt_pass));
  wm.addParameter(&p_host); wm.addParameter(&p_port);
  wm.addParameter(&p_user); wm.addParameter(&p_pass);
  wm.setConfigPortalTimeout(300);

  // station-режим; если нет сохранённого Wi-Fi — поднимет точку "coil-scope-setup"
  if (!wm.autoConnect("coil-scope-setup")) { delay(2000); ESP.restart(); }

  // забрать введённые значения и сохранить
  strncpy(mqtt_host, p_host.getValue(), sizeof(mqtt_host));
  strncpy(mqtt_port_s, p_port.getValue(), sizeof(mqtt_port_s));
  strncpy(mqtt_user, p_user.getValue(), sizeof(mqtt_user));
  strncpy(mqtt_pass, p_pass.getValue(), sizeof(mqtt_pass));
  mqtt_port = (uint16_t)atoi(mqtt_port_s);
  prefs.putString("mhost", mqtt_host);
  prefs.putString("mport", mqtt_port_s);
  prefs.putString("muser", mqtt_user);
  prefs.putString("mpass", mqtt_pass);

  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.printf("MQTT: %s:%u\n", mqtt_host, mqtt_port);

  server.on("/", []() { server.send(200, "text/html", PAGE); });
  server.on("/data", []() { server.send(200, "application/json", jsonData()); });
  server.begin();

  adcSetup();
}

uint32_t lastPub = 0;
void loop() {
  sampleBlock();
  analyzeBlock();
  server.handleClient();
  if (millis() - lastPub > 500) {
    lastPub = millis();
    mqttEnsure();
    if (mqtt.connected()) {
      mqtt.publish("mower/mi302/recon/wire_freq", String(g_peakFreq, 1).c_str());
      mqtt.publish("mower/mi302/recon/wire_mag",  String(g_peakMag, 0).c_str());
    }
    Serial.printf("peak=%.0f Hz  mag=%.0f\n", g_peakFreq, g_peakMag);
  }
}

// ----------------- ЗАМЕТКИ ПО ИТЕРАЦИИ -----------------
// 1) Пусто/шум в спектре: поднеси катушку ближе к проводу, поверни её, добавь витков.
// 2) Без ОУ АЦП не видит нижнюю полуволну — частоту это не скрывает (пик в FFT есть),
//    форма обрезана снизу. Полную форму добудем смещением Vcc/2 (2 резистора) позже.
// 3) Пик «прилип» к краю/нулю — поменяй SAMPLE_RATE (40000/100000), чтобы родная
//    частота попала в диапазон.
// 4) Странные значения (перемешанные дорожки) — нюанс маски АЦП (buf[i]&0x0FFF) —
//    скажи, поправлю.
