/* ============================================================
 *  SAFEGAS — Smart LPG Leakage Prevention System
 *  Versi: v13 (MQTT TLS + Dashboard Ready)
 *
 *  Berbasis sketch.ino v12 milik tim. Perubahan v13:
 *   [1] MQTT DIAKTIFKAN: HiveMQ Cloud, TLS port 8883
 *       (v12 memakai port 1883 non-TLS -> tidak akan connect
 *        ke HiveMQ Cloud; lihat catatan di bagian KONFIGURASI)
 *   [2] Event queue FreeRTOS: Core 1 tidak lagi memanggil
 *       mqtt.publish() langsung (v12: race condition antar-core)
 *   [3] vTaskDelay(1) di loop(): mencegah task WDT starvation
 *   [4] Heartbeat lintas-core: jika safety loop hang, Core 0
 *       memaksa kondisi fail-safe (katup tutup + listrik putus)
 *   [5] Deteksi sensor fault: ADC tidak wajar -> SENSOR_FAULT
 *       (v12: sensor putus terbaca 0 = "udara bersih" = BAHAYA)
 *   [6] Konversi ADC -> Rs/R0 -> ppm (kurva LPG datasheet MQ-2)
 *   [7] Reset butuh dwell time, bukan satu sampel
 *   [8] Baris CSV di Serial untuk dashboard tanpa MQTT
 *   [9] Topik cmd: reset dari dashboard (tetap lewat interlock)
 *
 *  Pin & wiring TIDAK diubah -> diagram.json tetap valid.
 * ============================================================ */

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ============================================================
//  KONFIGURASI — ISI BAGIAN INI
// ============================================================

// --- WiFi ---
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

/* --- MQTT ---
 * HiveMQ Cloud WAJIB TLS port 8883.
 * Ambil kredensial di https://console.hivemq.cloud
 *   Cluster URL : xxxx.s1.eu.hivemq.cloud
 *   Port        : 8883 (TLS)
 *   Username/Pass: yang dibuat di tab "Access Management"
 *
 * PENTING: v12 memakai port 1883 (non-TLS). HiveMQ Cloud
 * menolak koneksi non-TLS, jadi MQTT v12 tidak akan pernah
 * connect walaupun tanda komentar dihapus.
 */
/* AKTIFKAN_MQTT = 0 (default) -> firmware 100% jalan di Wokwi tanpa
 * kredensial. Serial CSV tetap mengalir, dashboard tetap realtime.
 * Set 1 setelah kredensial HiveMQ diisi. */
#define AKTIFKAN_MQTT 0

#define MQTT_HOST "GANTI-CLUSTER-ANDA.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "GANTI_USERNAME"
#define MQTT_PASS "GANTI_PASSWORD"
#define MQTT_USE_TLS 1        // 1 = TLS (wajib HiveMQ Cloud)

// Set 1 hanya untuk uji cepat tanpa akun HiveMQ (broker publik, non-TLS).
// JANGAN dipakai untuk pengumpulan: guideline mewajibkan HiveMQ Cloud.
#define MQTT_USE_PUBLIC_FALLBACK 0

#define DEVICE_ID       "safegas01"
#define TOPIC_TELEMETRY "safegas/" DEVICE_ID "/telemetry"
#define TOPIC_EVENT     "safegas/" DEVICE_ID "/event"
#define TOPIC_CMD       "safegas/" DEVICE_ID "/cmd"

// ============================================================
//  PIN  (identik v12 / diagram.json)
// ============================================================
#define PIN_GAS      34
#define PIN_BUZZER   23
#define PIN_SERVO    18
#define PIN_RELAY    19
#define PIN_BTN_RST  25
#define PIN_LED_G    13
#define PIN_LED_R    32
#define PIN_SDA      21
#define PIN_SCL      22

/* Modul relay di diagram.json: COM->3V3, NO->LED putih.
 * Artinya IN=HIGH menutup kontak (active-HIGH) DI SIMULASI.
 * Di hardware nyata modul relay umumnya active-LOW, jadi
 * jadikan konfigurasi — jangan diubah untuk demo Wokwi.
 */
#define RELAY_ACTIVE_LOW 0

// ============================================================
//  PARAMETER LOGIKA (sama dengan v12)
// ============================================================
const int GAS_ON       = 1500;
const int GAS_OFF      = 1300;
const int GAS_CRITICAL = 2800;

const unsigned long PERSIST_LOCK_MS = 15000;
const unsigned long TREND_WINDOW_MS = 1000;
const int           TREND_RISE_MIN  = 50;
const int           TREND_CONFIRM   = 2;

// --- validasi sensor (baru) ---
const int           ADC_MIN_VALID = 30;
const int           ADC_MAX_VALID = 4080;
const unsigned long RESET_DWELL_MS = 3000;

// --- watchdog lintas-core ---
const unsigned long HEARTBEAT_TIMEOUT = 3000;

// ============================================================
//  KALIBRASI MQ-2 (baru)
// ============================================================
const float ADC_VREF       = 3.3f;
const int   ADC_MAX        = 4095;
const float RL_OHM         = 5000.0f;   // resistor beban modul MQ-2
const float R0_CLEAN_RATIO = 9.83f;     // Rs(udara bersih)/R0, datasheet
const float PPM_A          = 574.25f;   // kurva LPG: ppm = A*(Rs/R0)^B
const float PPM_B          = -2.222f;

// ============================================================
//  WAKTU
// ============================================================
const unsigned long WARMUP_MS = 5000;
const unsigned long SAMPLE_MS = 250;
const unsigned long EVAL_MS   = 1000;
const unsigned long OLED_MS   = 500;
const unsigned long PUB_MS    = 1000;   // telemetri MQTT
const unsigned long CSV_MS    = 500;    // baris CSV Serial (2 sampel/detik)

// ============================================================
//  SERVO
// ============================================================
const int SERVO_OPEN   = 90;
const int SERVO_CLOSED = 0;

// ============================================================
//  STATUS
// ============================================================
enum Status { ST_WARMUP, ST_NORMAL, ST_ADA_GAS, ST_MEMBURUK, ST_BAHAYA, ST_SENSOR_FAULT };
const char* STATUS_STR[] = { "WARMUP", "NORMAL", "ADA_GAS", "MEMBURUK", "BAHAYA", "SENSOR_FAULT" };

// ============================================================
//  STATE
// ============================================================
bool warmup      = true;
bool gasDetected = false;
bool worsening   = false;
bool bahayaLock  = false;
bool valveOpen   = false;
bool powerOn     = false;
bool buzzerOn    = false;
bool sensorFault = false;
int  riseCount   = 0;

unsigned long bootMs = 0;
unsigned long gasStartTime = 0;
unsigned long lastTrendTime = 0;
unsigned long gasClearSince = 0;
unsigned long tSample = 0, tEval = 0, tOled = 0, tPub = 0, tCsv = 0;

volatile unsigned long safetyBeat = 0;
volatile bool netForcedSafe = false;
// Permintaan reset dari MQTT (Core 0) di-queue ke Core 1.
// v13 awal: onMqttMessage() memanggil tryReset() langsung dari Core 0
// sementara evaluate() menulis state yang sama di Core 1 -> data race.
volatile bool resetRequested = false;

// ============================================================
//  SENSOR
// ============================================================
#define BUF_N 8
int gasBuf[BUF_N];
int bufIdx = 0, bufCount = 0;
int gasRaw = 0, gasAvg = 0;
int gasAtTrendStart = 0;

float R0 = 10000.0f;
bool  r0Calibrated = false;
float gasPpm = 0.0f;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo valveServo;

volatile bool wifiOk = false;
volatile bool mqttOk = false;

// ============================================================
//  EVENT QUEUE (perbaikan race condition v12)
//  Core 1 menulis ke queue, Core 0 yang publish.
// ============================================================
struct EventMsg { char text[96]; uint8_t level; };  // level: 0 info, 1 warning, 2 danger
QueueHandle_t eventQueue = NULL;

#if AKTIFKAN_MQTT
  #include <WiFiClientSecure.h>
  #include <PubSubClient.h>
  #if MQTT_USE_PUBLIC_FALLBACK
    #include <WiFiClient.h>
    WiFiClient plainClient;
    PubSubClient mqtt(plainClient);
  #elif MQTT_USE_TLS
    WiFiClientSecure secureClient;
    PubSubClient mqtt(secureClient);
  #else
    WiFiClient plainClient2;
    PubSubClient mqtt(plainClient2);
  #endif
#endif

// ============================================================
//  UTIL
// ============================================================
Status currentStatus() {
  if (sensorFault) return ST_SENSOR_FAULT;
  if (warmup)      return ST_WARMUP;
  if (bahayaLock)  return ST_BAHAYA;
  if (worsening)   return ST_MEMBURUK;
  if (gasDetected) return ST_ADA_GAS;
  return ST_NORMAL;
}

void logEvent(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[96];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Serial.printf("[EVENT] %s\n", buf);

#if AKTIFKAN_MQTT
  // Kirim ke queue — TIDAK memanggil mqtt di sini (aman dari Core 1)
  if (eventQueue != NULL) {
    EventMsg m;
    strncpy(m.text, buf, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    m.level = bahayaLock ? 2 : (gasDetected ? 1 : 0);
    // non-blocking: jika queue penuh, event dibuang (safety > logging)
    xQueueSend(eventQueue, &m, 0);
  }
#endif
}

// ============================================================
//  KONVERSI MQ-2
// ============================================================
float adcToRs(int adc) {
  float vout = (adc / (float)ADC_MAX) * ADC_VREF;
  if (vout < 0.01f) vout = 0.01f;
  return RL_OHM * (ADC_VREF - vout) / vout;
}

float rsToPpm(int adc) {
  float rs = adcToRs(adc);
  float ratio = rs / R0;
  if (ratio < 0.01f) ratio = 0.01f;
  float ppm = PPM_A * powf(ratio, PPM_B);
  if (ppm < 0.0f) ppm = 0.0f;
  if (ppm > 10000.0f) ppm = 10000.0f;
  return ppm;
}

void calibrateR0(int adcCleanAir) {
  float rs = adcToRs(adcCleanAir);
  float r = rs / R0_CLEAN_RATIO;
  if (r < 1000.0f)    r = 1000.0f;
  if (r > 900000.0f)  r = 900000.0f;
  R0 = r;
  r0Calibrated = true;
  logEvent("Kalibrasi R0=%.0f ohm (Rs udara=%.0f)", R0, rs);
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

bool sensorPlausible(int adc) {
  return (adc >= ADC_MIN_VALID && adc <= ADC_MAX_VALID);
}

void raiseSensorFault(const char* why) {
  if (sensorFault) return;
  sensorFault = true;
  logEvent("SENSOR FAULT: %s (ADC=%d) -> FAIL-SAFE", why, gasRaw);
}

void setLock(const char* alasan) {
  if (bahayaLock) return;
  bahayaLock = true;
  logEvent("BAHAYA terkunci: %s (ADC=%d)", alasan, gasAvg);
}

bool tryReset(const char* src, unsigned long now) {
  if (warmup) {
    logEvent("Reset diabaikan (%s): masih warm-up", src);
    return false;
  }
  if (sensorFault) {
    logEvent("Reset DITOLAK (%s): sensor fault, periksa perangkat", src);
    return false;
  }
  if (!bahayaLock) {
    logEvent("Reset diabaikan (%s): sistem sedang normal", src);
    return false;
  }
  if (gasAvg >= GAS_OFF) {
    logEvent("Reset DITOLAK (%s): gas masih tinggi (ADC=%d)", src, gasAvg);
    return false;
  }
  if (gasClearSince == 0) gasClearSince = now;
  unsigned long clearFor = now - gasClearSince;
  if (clearFor < RESET_DWELL_MS) {
    logEvent("Reset DITOLAK (%s): tunggu %lu ms lagi", src, RESET_DWELL_MS - clearFor);
    return false;
  }

  bahayaLock = false;
  gasDetected = false;
  worsening = false;
  riseCount = 0;
  gasStartTime = 0;
  gasAtTrendStart = 0;
  gasClearSince = 0;
  logEvent("Reset BERHASIL (%s): sistem normal kembali", src);
  return true;
}

// ============================================================
//  EVALUASI
// ============================================================
void evaluate(unsigned long now) {
  if (!sensorPlausible(gasRaw)) {
    raiseSensorFault("nilai ADC tidak wajar (kabel/sensor)");
  } else if (sensorFault) {
    sensorFault = false;
    logEvent("Sensor pulih, kembali normal");
  }

  if (warmup) {
    if (now - bootMs >= WARMUP_MS) {
      warmup = false;
      if (gasAvg < GAS_OFF) calibrateR0(gasAvg);
      else logEvent("Warm-up selesai, kalibrasi R0 dilewati (udara tidak bersih)");
      logEvent("Warm-up selesai");
    }
  }

  if (!warmup && !sensorFault) {
    bool adaGas = gasDetected ? (gasAvg >= GAS_OFF) : (gasAvg >= GAS_ON);

    if (!adaGas) {
      if (gasClearSince == 0) gasClearSince = now;
      if (gasDetected) {
        logEvent("Gas hilang (ADC=%d)", gasAvg);
        gasDetected = false;
        worsening = false;
        riseCount = 0;
        gasStartTime = 0;
        gasAtTrendStart = 0;
        lastTrendTime = now;
      }
    } else {
      gasClearSince = 0;
      if (!gasDetected) {
        gasDetected = true;
        gasStartTime = now;
        lastTrendTime = now;
        gasAtTrendStart = gasAvg;
        riseCount = 0;
        logEvent("Gas terdeteksi! ADC=%d (%.0f ppm)", gasAvg, gasPpm);
      }

      if (gasAvg >= GAS_CRITICAL) setLock("kadar gas kritis");
      if (now - gasStartTime >= PERSIST_LOCK_MS) setLock("gas bertahan terlalu lama");

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

  Serial.printf("[LOG] t=%lus | ADC:%4d | %5.0fppm | R0:%.0f | %-12s | Katup:%-5s | Listrik:%-5s | Alarm:%s | Net:%s\n",
                now / 1000, gasAvg, gasPpm, R0, STATUS_STR[currentStatus()],
                valveOpen ? "BUKA" : "TUTUP", powerOn ? "NYALA" : "PUTUS",
                buzzerOn ? "ON" : "OFF",
                mqttOk ? "MQTT" : (wifiOk ? "WIFI" : "OFF"));
}

// Baris CSV untuk dashboard berbasis Serial (tanpa MQTT)
/* Baris CSV untuk dashboard (mode serial).
   Dicetak 2x/detik agar laju data ~2 sampel/s dan grafik tidak patah.
   Format: CSV,device,uptime_s,adc,adc_raw,ppm,status,valve,power,alarm,lock,threshold,r0 */
void printCsv(unsigned long now) {
  Serial.printf("CSV,%s,%lu,%d,%d,%.0f,%s,%s,%s,%d,%d,%d,%.0f\n",
                DEVICE_ID, now / 1000, gasAvg, gasRaw, gasPpm,
                STATUS_STR[currentStatus()],
                valveOpen ? "OPEN" : "CLOSED",
                powerOn ? "ON" : "OFF",
                buzzerOn ? 1 : 0, bahayaLock ? 1 : 0, GAS_ON, R0);
}

void cekTombolReset() {
  static bool stable = HIGH, lastRead = HIGH;
  static unsigned long lastChange = 0;

  bool r = digitalRead(PIN_BTN_RST);
  if (r != lastRead) { lastChange = millis(); lastRead = r; }

  if ((millis() - lastChange) > 30 && r != stable) {
    stable = r;
    if (stable == LOW) tryReset("tombol", millis());
  }
}

// ============================================================
//  OUTPUT
// ============================================================
void relayWrite(bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
#else
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
#endif
}

void updateOutputs(unsigned long now) {
  bool forceSafe = sensorFault || netForcedSafe;

  digitalWrite(PIN_LED_G, (!gasDetected && !bahayaLock && !warmup && !forceSafe));
  digitalWrite(PIN_LED_R, (gasDetected || bahayaLock || forceSafe));

  bool wantValveOpen = (!warmup && !bahayaLock && !gasDetected && !worsening && !forceSafe);
  if (wantValveOpen != valveOpen) {
    valveOpen = wantValveOpen;
    valveServo.write(valveOpen ? SERVO_OPEN : SERVO_CLOSED);
    logEvent(valveOpen ? "Katup Gas DIBUKA" : "Katup Gas DITUTUP");
  }

  // Listrik diputus SEGERA saat gas terdeteksi, bukan hanya saat BAHAYA.
  // v12 memakai (!warmup && !bahayaLock) sehingga relay tetap NYALA saat
  // ADA_GAS — bertentangan dengan FR-04 PRD dan skenario demo halaman 2.
  // Akibatnya sumber percikan api tetap hidup padahal gas sudah ada.
  bool wantPowerOn = (!warmup && !bahayaLock && !gasDetected && !worsening && !forceSafe);
  if (wantPowerOn != powerOn) {
    powerOn = wantPowerOn;
    relayWrite(powerOn);
    logEvent(powerOn ? "Listrik NYALA" : "Listrik DIPUTUS TOTAL (Darurat)");
  }

  bool on = false;
  if (bahayaLock || forceSafe) on = (now % 300) < 150;
  else if (gasDetected)        on = (now % 1000) < 100;

  if (on != buzzerOn) {
    buzzerOn = on;
    if (on) tone(PIN_BUZZER, (bahayaLock || forceSafe) ? 2500 : 1500);
    else    noTone(PIN_BUZZER);
  }
}

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
    case ST_WARMUP:       display.print("WARMUP");   break;
    case ST_NORMAL:       display.print("NORMAL");   break;
    case ST_ADA_GAS:      display.print("ADA GAS");  break;
    case ST_MEMBURUK:     display.print("MEMBURUK"); break;
    case ST_BAHAYA:       display.print("BAHAYA");   break;
    case ST_SENSOR_FAULT: display.print("SENSOR!");  break;
  }

  display.setTextSize(1);
  display.setCursor(0, 33);
  display.printf("ADC:%d %5.0fppm", gasAvg, gasPpm);
  display.setCursor(0, 43);
  display.printf("Katup:%s Listrik:%s", valveOpen ? "BUKA" : "TUTUP", powerOn ? "ON" : "OFF");
  display.setCursor(0, 53);
  if (wifiOk) display.print(WiFi.localIP().toString());
  else        display.print("Mencari WiFi...");
  display.display();
}

// ============================================================
//  MQTT CALLBACK (reset dari dashboard)
// ============================================================
#if AKTIFKAN_MQTT
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  char msg[32];
  unsigned int n = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  if (String(topic) == TOPIC_CMD) {
    Serial.printf("[MQTT] cmd: %s\n", msg);
    if (strcasecmp(msg, "reset") == 0) {
      // JANGAN panggil tryReset() di sini: callback berjalan di Core 0,
      // sedangkan tryReset() memodifikasi state yang dipakai evaluate()
      // di Core 1. Cukup pasang flag; loop() yang mengeksekusi.
      // Interlock tetap berlaku karena tryReset() yang memutuskan.
      resetRequested = true;
    }
  }
}
#endif


// ============================================================
//  TASK JARINGAN (Core 0)
// ============================================================
void netTask(void*) {
#if !AKTIFKAN_MQTT
  /* Mode Serial saja (default Wokwi):
     Tidak ada WiFi/MQTT sama sekali -> tidak ada yang bisa memblokir boot.
     Safety core (Core 1) berjalan penuh, CSV mengalir ke Serial Monitor. */
  wifiOk = false;
  mqttOk = false;
  Serial.println("[NET] Mode Serial saja (MQTT nonaktif). "
                 "Data dikirim lewat baris CSV di Serial Monitor.");
  for (;;) {
    // Tidak ada yang dikerjakan: safety loop di Core 1 berjalan penuh dan
    // baris CSV mengalir ke Serial Monitor. Task ini hanya harus tetap hidup
    // agar struktur program sama dengan mode MQTT.
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
#else
  Serial.println("[WIFI] Memulai koneksi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);

  unsigned long lastPrint = 0, lastMqttTry = 0, lastPub = 0;

#if MQTT_USE_TLS && !MQTT_USE_PUBLIC_FALLBACK
  secureClient.setInsecure();   // demo: tanpa verifikasi CA. Produksi: setCACert()
#endif
  mqtt.setBufferSize(512);
  mqtt.setCallback(onMqttMessage);

  for (;;) {
    // ---------- WATCHDOG LINTAS-CORE ----------
    unsigned long beat = safetyBeat;
    if (beat != 0 && (millis() - beat) > HEARTBEAT_TIMEOUT) {
      if (!netForcedSafe) {
        netForcedSafe = true;
        relayWrite(false);
        valveServo.write(SERVO_CLOSED);
        tone(PIN_BUZZER, 2500);
        Serial.println("[WDT] !! SAFETY LOOP HANG -> FAIL-SAFE DIPAKSA !!");
      }
    } else if (netForcedSafe) {
      netForcedSafe = false;
      noTone(PIN_BUZZER);
      Serial.println("[WDT] Safety loop pulih");
    }

    wifiOk = (WiFi.status() == WL_CONNECTED);

    if (!wifiOk) {
      mqttOk = false;
      if (millis() - lastPrint > 2000) {
        lastPrint = millis();
        Serial.printf("[WIFI] Status: %d (Mencoba...)\n", WiFi.status());
      }
    } else {
      static bool printed = false;
      if (!printed) {
        Serial.println("\n[WIFI] ========================");
        Serial.println("[WIFI] BERHASIL TERHUBUNG!");
        Serial.print  ("[WIFI] IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.println("[WIFI] ========================\n");
        printed = true;
      }

      if (!mqtt.connected()) {
        mqttOk = false;
        if (millis() - lastMqttTry > 5000) {
          lastMqttTry = millis();
          Serial.println("[MQTT] Mencoba connect...");
#if MQTT_USE_PUBLIC_FALLBACK
          mqtt.setServer("broker.hivemq.com", 1883);
          String cid = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
          bool ok = mqtt.connect(cid.c_str());
#else
          mqtt.setServer(MQTT_HOST, MQTT_PORT);
          String cid = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
          bool ok = mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS);
#endif
          if (ok) {
            Serial.println("[MQTT] Terhubung!");
            mqttOk = true;
            mqtt.subscribe(TOPIC_CMD);
            logEvent("MQTT terhubung");
          } else {
            Serial.printf("[MQTT] Gagal, rc=%d\n", mqtt.state());
          }
        }
      } else {
        mqtt.loop();
        mqttOk = true;

        EventMsg em;
        while (eventQueue != NULL && xQueueReceive(eventQueue, &em, 0) == pdTRUE) {
          char json[200];
          snprintf(json, sizeof(json),
                   "{\"device\":\"%s\",\"uptime_s\":%lu,\"level\":%u,\"msg\":\"%s\"}",
                   DEVICE_ID, millis() / 1000, em.level, em.text);
          mqtt.publish(TOPIC_EVENT, json);
        }

        if (millis() - lastPub > PUB_MS) {
          lastPub = millis();
          char json[320];
          snprintf(json, sizeof(json),
                   "{\"device\":\"%s\",\"uptime_s\":%lu,\"adc\":%d,\"adc_raw\":%d,"
                   "\"ppm\":%.0f,\"r0\":%.0f,\"status\":\"%s\",\"valve\":\"%s\","
                   "\"power\":\"%s\",\"alarm\":%s,\"lock\":%s,\"threshold\":%d,\"rssi\":%d}",
                   DEVICE_ID, millis() / 1000, gasAvg, gasRaw,
                   gasPpm, R0, STATUS_STR[currentStatus()],
                   valveOpen ? "OPEN" : "CLOSED",
                   powerOn ? "ON" : "OFF",
                   buzzerOn ? "true" : "false",
                   bahayaLock ? "true" : "false",
                   GAS_ON, WiFi.RSSI());
          mqtt.publish(TOPIC_TELEMETRY, json);
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
#endif
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SafeGas v13 Booting ===");

  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BTN_RST, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GAS, ADC_11db);

  // FAIL-SAFE BOOT
  relayWrite(false);
  ESP32PWM::allocateTimer(0);
  valveServo.setPeriodHertz(50);
  valveServo.attach(PIN_SERVO, 500, 2400);
  valveServo.write(SERVO_CLOSED);

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERR] OLED tidak ditemukan");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("SafeGas v13 booting...");
  display.display();

#if AKTIFKAN_MQTT
  eventQueue = xQueueCreate(8, sizeof(EventMsg));
#endif

  safetyBeat = millis();
  xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 1, NULL, 0);

  bootMs = millis();
  Serial.println("Warm-up MQ-2...");
}

void loop() {
  unsigned long now = millis();
  safetyBeat = now;

  cekTombolReset();

  // Reset dari MQTT dieksekusi di Core 1 (bukan di callback Core 0)
  if (resetRequested) {
    resetRequested = false;
    tryReset("mqtt", now);
  }

  if (now - tSample >= SAMPLE_MS) { tSample = now; sampleSensors(); }
  if (now - tEval   >= EVAL_MS)   { tEval = now;   evaluate(now); }

  gasPpm = rsToPpm(gasAvg);

  updateOutputs(now);

  if (now - tOled >= OLED_MS) { tOled = now; drawOled(now); }
  if (now - tCsv  >= CSV_MS)  { tCsv  = now; printCsv(now); }

  vTaskDelay(1);   // cegah task WDT starvation
}
