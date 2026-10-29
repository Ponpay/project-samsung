/*
 * ============================================================
 *  SAFEGAS - Deteksi Kebocoran Gas LPG Rumah Tangga
 *  Versi: v2 (Safety core + MQTT HiveMQ non-blocking)
 *
 *  Arsitektur:
 *   - Core 1 (loop)  : sensor, logika safety, output, OLED  -> TIDAK PERNAH diblok jaringan
 *   - Core 0 (task)  : WiFi + MQTT (publish telemetry/event, terima command)
 *
 *  Topic (DEVICE_ID = safegas01):
 *   safegas/safegas01/telemetry  -> JSON tiap 1 detik
 *   safegas/safegas01/event      -> JSON tiap ada kejadian
 *   safegas/safegas01/status     -> "online" / "offline" (retained, LWT)
 *   safegas/safegas01/cmd        <- "reset"
 * ============================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <stdarg.h>

// ---------- KONFIGURASI JARINGAN ----------
#define DEVICE_ID    "safegas01"
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
const char* MQTT_HOST = "GANTI-DENGAN-HOST.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "GANTI_USER";
const char* MQTT_PASS = "GANTI_PASSWORD";

#define TOPIC_TELEMETRY "safegas/" DEVICE_ID "/telemetry"
#define TOPIC_EVENT     "safegas/" DEVICE_ID "/event"
#define TOPIC_STATUS    "safegas/" DEVICE_ID "/status"
#define TOPIC_CMD       "safegas/" DEVICE_ID "/cmd"

// ---------- PIN ----------
#define PIN_GAS      34
#define PIN_BUZZER   23
#define PIN_SERVO    18
#define PIN_RELAY    19
#define PIN_BTN_RST  25
#define PIN_LED_G    13
#define PIN_LED_R    32
#define PIN_SDA      21
#define PIN_SCL      22

// ---------- PARAMETER LOGIKA ----------
const int GAS_ON       = 1500;   // gas dianggap ada jika avg >= ini
const int GAS_OFF      = 1300;   // gas dianggap hilang jika avg < ini (hysteresis)
const int GAS_CRITICAL = 2800;   // langsung BAHAYA (kebocoran besar/mendadak)
const unsigned long PERSIST_LOCK_MS   = 15000; // gas bertahan selama ini -> BAHAYA
const unsigned long TREND_WINDOW_MS   = 1000;
const int           TREND_RISE_MIN    = 50;    // kenaikan avg per window
const int           TREND_CONFIRM     = 2;     // jumlah window naik beruntun -> BAHAYA

// ---------- WAKTU ----------
const unsigned long WARMUP_MS    = 5000;
const unsigned long SAMPLE_MS    = 250;
const unsigned long EVAL_MS      = 1000;
const unsigned long OLED_MS      = 500;
const unsigned long TELEMETRY_MS = 1000;

// ---------- SERVO ----------
const int SERVO_OPEN   = 90;
const int SERVO_CLOSED = 0;

// ---------- STATUS ----------
enum Status { ST_WARMUP, ST_NORMAL, ST_ADA_GAS, ST_MEMBURUK, ST_BAHAYA };
const char* STATUS_STR[] = { "WARMUP", "NORMAL", "ADA_GAS", "MEMBURUK", "BAHAYA" };

bool warmup      = true;
bool gasDetected = false;
bool worsening   = false;
bool bahayaLock  = false;
bool valveOpen   = false;
bool buzzerOn    = false;
int  riseCount   = 0;

unsigned long bootMs = 0;
unsigned long gasStartTime = 0;
unsigned long lastTrendTime = 0;
unsigned long tSample = 0, tEval = 0, tOled = 0;

// ---------- SENSOR ----------
#define BUF_N 8
int gasBuf[BUF_N];
int bufIdx = 0, bufCount = 0;
int gasRaw = 0, gasAvg = 0;
int gasAtTrendStart = 0;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo valveServo;

// ---------- SHARED DENGAN TASK JARINGAN ----------
struct Snapshot {
  int adc, raw;
  uint8_t status;
  unsigned long durasi, uptime;
  bool valve, alarm, lock;
};
Snapshot snap = {0, 0, ST_WARMUP, 0, 0, false, false, false};
portMUX_TYPE snapMux = portMUX_INITIALIZER_UNLOCKED;

struct EventMsg { unsigned long upS; char text[96]; };
QueueHandle_t eventQ;

volatile bool cmdResetPending = false;
volatile bool wifiOk = false;
volatile bool mqttOk = false;

// ============================================================
//  UTIL
// ============================================================
Status currentStatus() {
  if (warmup)       return ST_WARMUP;
  if (bahayaLock)   return ST_BAHAYA;
  if (worsening)    return ST_MEMBURUK;
  if (gasDetected)  return ST_ADA_GAS;
  return ST_NORMAL;
}

// Cetak ke Serial + antrikan ke MQTT (tidak memblok)
void logEvent(const char* fmt, ...) {
  EventMsg m;
  m.upS = millis() / 1000;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, ap);
  va_end(ap);
  Serial.printf("[EVENT] %s\n", m.text);
  xQueueSend(eventQ, &m, 0);
}

void updateSnapshot(unsigned long now) {
  Snapshot s;
  s.adc = gasAvg;
  s.raw = gasRaw;
  s.status = currentStatus();
  s.durasi = gasDetected ? (now - gasStartTime) / 1000 : 0;
  s.uptime = now / 1000;
  s.valve = valveOpen;
  s.alarm = buzzerOn;
  s.lock = bahayaLock;
  portENTER_CRITICAL(&snapMux);
  snap = s;
  portEXIT_CRITICAL(&snapMux);
}

// ============================================================
//  SENSOR
// ============================================================
void sampleSensors() {
  gasRaw = analogRead(PIN_GAS);
  gasBuf[bufIdx] = gasRaw;
  bufIdx = (bufIdx + 1) % BUF_N;
  if (bufCount < BUF_N) bufCount++;

  long sum = 0;
  for (int i = 0; i < bufCount; i++) sum += gasBuf[i];
  gasAvg = sum / bufCount;
}

// ============================================================
//  LOGIKA
// ============================================================
void setLock(const char* alasan) {
  if (bahayaLock) return;
  bahayaLock = true;
  logEvent("BAHAYA terkunci: %s (ADC=%d)", alasan, gasAvg);
}

bool tryReset(const char* src) {
  if (!bahayaLock) {
    logEvent("Reset diabaikan (%s): sistem sedang normal", src);
    return false;
  }
  if (gasAvg >= GAS_OFF || gasRaw >= GAS_OFF) {
    logEvent("Reset DITOLAK (%s): gas masih tinggi (ADC=%d)", src, gasAvg);
    return false;
  }
  bahayaLock = false;
  gasDetected = false;
  worsening = false;
  riseCount = 0;
  gasStartTime = 0;
  logEvent("Reset BERHASIL (%s): sistem normal kembali", src);
  return true;
}

void evaluate(unsigned long now) {
  if (warmup) {
    if (now - bootMs >= WARMUP_MS) {
      warmup = false;
      logEvent("Warm-up selesai");
    }
  }

  if (!warmup) {
    bool adaGas = gasDetected ? (gasAvg >= GAS_OFF) : (gasAvg >= GAS_ON);

    if (!adaGas) {
      if (gasDetected) {
        logEvent("Gas hilang (ADC=%d)", gasAvg);
        gasDetected = false;
        worsening = false;
        riseCount = 0;
        gasStartTime = 0;
        gasAtTrendStart = 0;
      }
    } else {
      if (!gasDetected) {
        gasDetected = true;
        gasStartTime = now;
        lastTrendTime = now;
        gasAtTrendStart = gasAvg;
        riseCount = 0;
        logEvent("Gas terdeteksi! ADC=%d", gasAvg);
      }

      // Pemicu 1: kadar sangat tinggi
      if (gasAvg >= GAS_CRITICAL) setLock("kadar gas kritis");

      // Pemicu 2: gas bertahan terlalu lama (kebocoran stabil)
      if (now - gasStartTime >= PERSIST_LOCK_MS) setLock("gas bertahan terlalu lama");

      // Pemicu 3: tren naik beruntun
      if (now - lastTrendTime >= TREND_WINDOW_MS) {
        int delta = gasAvg - gasAtTrendStart;
        if (delta >= TREND_RISE_MIN) {
          riseCount++;
          if (!worsening) {
            worsening = true;
            logEvent("Tren memburuk: %d -> %d (delta %+d)", gasAtTrendStart, gasAvg, delta);
          }
          if (riseCount >= TREND_CONFIRM) setLock("tren gas naik terus");
        } else {
          riseCount = 0;
          if (delta < 0) worsening = false;
        }
        lastTrendTime = now;
        gasAtTrendStart = gasAvg;
      }
    }
  }

  // --- LOG TIAP DETIK (status SAMA dengan OLED & MQTT) ---
  unsigned long durasi = gasDetected ? (now - gasStartTime) / 1000 : 0;
  Serial.printf("[LOG] t=%lus | ADC: %4d | Status: %-8s | Durasi: %2lus | Katup: %-5s | Listrik: %-5s | Alarm: %s | Net: %s\n",
                now / 1000, gasAvg, STATUS_STR[currentStatus()], durasi,
                valveOpen ? "BUKA" : "TUTUP",
                valveOpen ? "NYALA" : "PUTUS",
                buzzerOn ? "ON" : "OFF",
                mqttOk ? "MQTT" : (wifiOk ? "WIFI" : "OFF"));
}

// ============================================================
//  TOMBOL RESET (debounce benar)
// ============================================================
void cekTombolReset() {
  static bool stable = HIGH, lastRead = HIGH;
  static unsigned long lastChange = 0;

  bool r = digitalRead(PIN_BTN_RST);
  if (r != lastRead) { lastChange = millis(); lastRead = r; }

  if ((millis() - lastChange) > 30 && r != stable) {
    stable = r;
    if (stable == LOW) tryReset("tombol");
  }
}

// ============================================================
//  OUTPUT
// ============================================================
void updateOutputs(unsigned long now) {
  digitalWrite(PIN_LED_G, !gasDetected && !bahayaLock && !warmup);
  digitalWrite(PIN_LED_R, gasDetected || bahayaLock);

  // Katup/relay: menutup bila warmup atau bahayaLock. Buka hanya jika aman & tidak terkunci.
  bool wantOpen = (!warmup && !bahayaLock);
  if (wantOpen != valveOpen) {
    valveOpen = wantOpen;
    valveServo.write(valveOpen ? SERVO_OPEN : SERVO_CLOSED);
    digitalWrite(PIN_RELAY, valveOpen ? HIGH : LOW);
    logEvent(valveOpen ? "Katup DIBUKA, listrik NYALA" : "Katup DITUTUP, listrik PUTUS");
  }

  bool on = false;
  if (bahayaLock)       on = (now % 300) < 150;     // cepat = bahaya
  else if (gasDetected) on = (now % 1000) < 100;    // pendek = waspada

  if (on != buzzerOn) {
    buzzerOn = on;
    if (on) tone(PIN_BUZZER, bahayaLock ? 2500 : 1500);
    else    noTone(PIN_BUZZER);
  }
}

// ============================================================
//  OLED
// ============================================================
void drawOled(unsigned long now) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SAFEGAS");
  display.setCursor(66, 0);
  display.printf("W:%s M:%s", wifiOk ? "OK" : "--", mqttOk ? "OK" : "--");

  display.setTextSize(2);
  display.setCursor(0, 12);
  switch (currentStatus()) {
    case ST_WARMUP:   display.print("WARMUP");   break;
    case ST_NORMAL:   display.print("NORMAL");   break;
    case ST_ADA_GAS:  display.print("ADA GAS");  break;
    case ST_MEMBURUK: display.print("MEMBURUK"); break;
    case ST_BAHAYA:   display.print("BAHAYA");   break;
  }

  display.setTextSize(1);
  display.setCursor(0, 33);
  display.printf("Gas: %d", gasAvg);
  display.setCursor(0, 43);
  if (gasDetected) display.printf("Durasi: %lu s", (now - gasStartTime) / 1000);
  else             display.print("Durasi: -");
  display.setCursor(0, 53);
  display.printf("Katup: %s", valveOpen ? "BUKA" : "TUTUP");

  display.display();
}

// ============================================================
//  TASK JARINGAN (core 0) - tidak boleh mempengaruhi logika safety
// ============================================================
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[48];
  unsigned int n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;
  for (unsigned int i = 0; i < n; i++) buf[i] = tolower(buf[i]);
  if (strstr(buf, "reset")) cmdResetPending = true;   // dieksekusi di loop() utama
}

void publishTelemetry() {
  Snapshot s;
  portENTER_CRITICAL(&snapMux);
  s = snap;
  portEXIT_CRITICAL(&snapMux);

  char json[300];
  snprintf(json, sizeof(json),
    "{\"device\":\"%s\",\"uptime_s\":%lu,\"adc\":%d,\"adc_raw\":%d,"
    "\"status\":\"%s\",\"duration_s\":%lu,\"valve\":\"%s\",\"power\":\"%s\","
    "\"alarm\":%s,\"lock\":%s,\"threshold\":%d}",
    DEVICE_ID, s.uptime, s.adc, s.raw, STATUS_STR[s.status], s.durasi,
    s.valve ? "OPEN" : "CLOSED", s.valve ? "ON" : "OFF",
    s.alarm ? "true" : "false", s.lock ? "true" : "false", GAS_ON);
  mqtt.publish(TOPIC_TELEMETRY, json);
}

void netTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  secureClient.setInsecure();            // demo/simulasi; produksi: pasang root CA
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);
  mqtt.setBufferSize(512);

  unsigned long lastTry = 0, lastPub = 0;
  bool firstTry = true;

  for (;;) {
    wifiOk = (WiFi.status() == WL_CONNECTED);

    // buang event lama bila belum terhubung agar antrian tidak penuh
    EventMsg m;
    if (wifiOk && mqtt.connected()) {
      mqttOk = true;
      mqtt.loop();

      while (xQueueReceive(eventQ, &m, 0) == pdTRUE) {
        char json[200];
        snprintf(json, sizeof(json),
                 "{\"device\":\"%s\",\"uptime_s\":%lu,\"msg\":\"%s\"}", DEVICE_ID, m.upS, m.text);
        mqtt.publish(TOPIC_EVENT, json);
      }
      if (millis() - lastPub >= TELEMETRY_MS) {
        lastPub = millis();
        publishTelemetry();
      }
    } else {
      mqttOk = false;
      while (xQueueReceive(eventQ, &m, 0) == pdTRUE) { /* drop */ }

      if (wifiOk && (firstTry || millis() - lastTry > 5000)) {
        firstTry = false;
        lastTry = millis();
        String cid = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
        Serial.println("[NET] Menghubungkan MQTT...");
        if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
          mqtt.publish(TOPIC_STATUS, "online", true);
          mqtt.subscribe(TOPIC_CMD, 1);
          mqttOk = true;
          Serial.println("[NET] MQTT terhubung");
        } else {
          Serial.printf("[NET] MQTT gagal, rc=%d\n", mqtt.state());
        }
      }
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SafeGas booting ===");

  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BTN_RST, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GAS, ADC_11db);

  digitalWrite(PIN_RELAY, LOW);          // fail-safe: listrik putus saat boot
  ESP32PWM::allocateTimer(0);
  valveServo.setPeriodHertz(50);
  valveServo.attach(PIN_SERVO, 500, 2400);
  valveServo.write(SERVO_CLOSED);        // fail-safe: katup tertutup saat boot

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERR] OLED tidak ditemukan");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("SafeGas booting...");
  display.display();

  eventQ = xQueueCreate(16, sizeof(EventMsg));
  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, NULL, 0);

  bootMs = millis();
  Serial.println("Warm-up MQ-2...");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  cekTombolReset();
  if (cmdResetPending) { cmdResetPending = false; tryReset("MQTT"); }

  if (now - tSample >= SAMPLE_MS) { tSample = now; sampleSensors(); }
  if (now - tEval   >= EVAL_MS)   { tEval = now;   evaluate(now); }

  updateOutputs(now);

  if (now - tOled >= OLED_MS) { tOled = now; drawOled(now); updateSnapshot(now); }
}
