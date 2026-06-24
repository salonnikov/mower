// coil-scope — «осциллограф из ESP32» для родного сигнала провода периметра.
// Катушка (из витой пары) → GPIO36 (ADC1_CH0). Питание — повербанк по USB.
// Веб-морда: http://<ip-esp>/  — форма сигнала + спектр (FFT) + пиковая частота.
// MQTT: mower/mi302/recon/wire_freq, /wire_mag.
//
// ВЕРСИЯ 1 (first light). Цель — увидеть: ЕСТЬ ли сигнал, на какой ЧАСТОТЕ,
// и как он выглядит. Если сигнал слабый/кривой — итерируем (см. заметки внизу).
//
// !!! Перед прошивкой заполни Wi-Fi/MQTT ниже.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <driver/adc.h>

// Wi-Fi и брокер задаются в secrets.h (скопируй secrets.h.example → secrets.h).
// secrets.h в git НЕ попадает (см. .gitignore).
#include "secrets.h"

// АЦП/выборка
static const adc1_channel_t ADC_CH = ADC1_CHANNEL_0;   // GPIO36 (VP)
static const i2s_port_t I2S_PORT  = I2S_NUM_0;
static const uint32_t SAMPLE_RATE = 80000;             // Гц → Найквист 40 кГц
static const uint16_t SAMPLES     = 1024;              // окно FFT (бин = 78.1 Гц)

double vReal[SAMPLES];
double vImag[SAMPLES];
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, (double)SAMPLE_RATE);

// результаты последнего блока (для веб/MQTT)
volatile double g_peakFreq = 0, g_peakMag = 0;
uint16_t g_wave[256];     // форма (даунсемпл)
uint16_t g_spec[256];     // спектр 0..40 кГц (даунсемпл)

WebServer  server(80);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

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
  adc1_config_channel_atten(ADC_CH, ADC_ATTEN_DB_11);   // диапазон ~0..3.3В
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_adc_mode(ADC_UNIT_1, ADC_CH);
  i2s_adc_enable(I2S_PORT);
}

// набрать один блок SAMPLES из АЦП
void sampleBlock() {
  static uint16_t buf[SAMPLES];
  size_t bytesRead = 0;
  size_t need = SAMPLES * sizeof(uint16_t);
  size_t got = 0;
  while (got < need) {
    i2s_read(I2S_PORT, ((uint8_t*)buf) + got, need - got, &bytesRead, portMAX_DELAY);
    got += bytesRead;
  }
  for (uint16_t i = 0; i < SAMPLES; i++) {
    vReal[i] = (double)(buf[i] & 0x0FFF);  // 12 бит значения; старшие 4 бита — номер канала
    vImag[i] = 0.0;
  }
}

void analyzeBlock() {
  // форма (для веба) — до вычитания среднего
  for (int i = 0; i < 256; i++) g_wave[i] = (uint16_t)vReal[i * (SAMPLES / 256)];

  // убрать постоянную составляющую (среднее)
  double mean = 0; for (uint16_t i = 0; i < SAMPLES; i++) mean += vReal[i];
  mean /= SAMPLES;
  for (uint16_t i = 0; i < SAMPLES; i++) { vReal[i] -= mean; vImag[i] = 0; }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  g_peakFreq = FFT.majorPeak();
  // амплитуда на пике + спектр (бины 1..512 → 0..40кГц, даунсемпл до 256)
  double maxMag = 0;
  for (int i = 0; i < 256; i++) {
    double m = vReal[1 + i * 2];               // пропускаем бин 0 (DC)
    g_spec[i] = (uint16_t)constrain(m, 0, 65535);
    if (m > maxMag) maxMag = m;
  }
  g_peakMag = maxMag;
}

// ---------- Веб ----------
const char* PAGE = R"HTML(<!doctype html><meta charset=utf-8><title>coil-scope</title>
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
async function tick(){let d=await(await fetch('/data')).json();
document.getElementById('f').textContent=d.freq.toFixed(0);
document.getElementById('m').textContent=d.mag.toFixed(0);
draw(document.getElementById('w'),d.wave,4096);
draw(document.getElementById('s'),d.spec,Math.max(...d.spec,1));}
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
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.connect("coil-scope", strlen(MQTT_USER) ? MQTT_USER : nullptr,
                             strlen(MQTT_PASS) ? MQTT_PASS : nullptr);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  Serial.print("IP: "); Serial.println(WiFi.localIP());

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
// 1) Если в спектре пусто/шум: поднеси катушку ближе к проводу, поверни её
//    (ось катушки меняет чувствительность), добавь витков. Сигнал ищем по ПИКУ.
// 2) Без операционника сигнал может «упираться» в 0 (АЦП не видит отрицательную
//    полуволну). Частоту это НЕ скрывает (пик в FFT остаётся), но форма будет
//    обрезана снизу. Полную форму получим, когда добавим смещение Vcc/2 (нужны
//    2 резистора) или операционник — позже.
// 3) Если пик «прилипает» к краю/нулю — поменяй SAMPLE_RATE (напр. 40000 или
//    100000), чтобы родная частота попала в диапазон.
// 4) Байт-порядок/маска АЦП на ESP32 иногда требует правки (buf[i]&0x0FFF). Если
//    значения выглядят странно (две перемешанные дорожки) — скажи, поправим.
