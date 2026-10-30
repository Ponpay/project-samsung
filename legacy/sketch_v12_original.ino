/*
 * ============================================================
 *  SAFEGAS - Deteksi Kebocoran Gas LPG Rumah Tangga
 *  Versi: v12 (WiFi + Safety + MQTT Ready (Commented))
 * 
 *  Petunjuk:
 *  - Kode ini jalan 100% untuk WiFi & Safety.
 *  - Untuk mengaktifkan MQTT, hapus tanda komentar (//) pada
 *    bagian yang ditandai dengan [AKTIFKAN MQTT].
 * ============================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <stdarg.h>

// [AKTIFKAN MQTT 1]: Hapus // pada 2 baris di bawah ini jika ingin pakai MQTT
// #include <PubSubClient.h>
// WiFiClient espClient; // Client untuk Port 1883 (Non-TLS)
// PubSubClient mqtt(espClient);

// ---------- KONFIGURASI WIFI ----------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// [AKTIFKAN MQTT 2]: Isi kredensial HiveMQ Anda di sini
const char* MQTT_HOST = "GANTI-DENGAN-HOST-ANDA.s1.eu.hivemq.cloud"; 
const int   MQTT_PORT = 1883; 
const char* MQTT_USER = "GANTI_USER";                                
const char* MQTT_PASS = "GANTI_PASSWORD";                            

#define DEVICE_ID "safegas01"
#define TOPIC_TELEMETRY "safegas/" DEVICE_ID "/telemetry"
#define TOPIC_EVENT     "safegas/" DEVICE_ID "/event"

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
const int GAS_ON       = 1500;   
const int GAS_OFF      = 1300;   
const int GAS_CRITICAL = 2800;   
const unsigned long PERSIST_LOCK_MS   = 15000; 
const unsigned long TREND_WINDOW_MS   = 1000;
const int           TREND_RISE_MIN    = 50;    
const int           TREND_CONFIRM     = 2;     

// ---------- WAKTU ----------
const unsigned long WARMUP_MS    = 5000;
const unsigned long SAMPLE_MS    = 250;
const unsigned long EVAL_MS      = 1000;
const unsigned long OLED_MS      = 500;

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
bool powerOn     = false; 
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

volatile bool wifiOk = false;
volatile bool mqttOk = false; // Akan jadi true jika MQTT aktif & connect

// ============================================================
//  UTIL & LOGIKA SAFETY
// ============================================================
Status currentStatus() {
  if (warmup)       return ST_WARMUP;
  if (bahayaLock)   return ST_BAHAYA;
  if (worsening)    return ST_MEMBURUK;
  if (gasDetected)  return ST_ADA_GAS;
  return ST_NORMAL;
}

void logEvent(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[96];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[EVENT] %s\n", buf);
  
  // [AKTIFKAN MQTT 3]: Kirim event ke MQTT jika terhubung
  // if (mqtt.connected()) {
  //   char json[150];
  //   snprintf(json, sizeof(json), "{\"device\":\"%s\",\"uptime_s\":%lu,\"msg\":\"%s\"}", 
  //            DEVICE_ID, millis()/1000, buf);
  //   mqtt.publish(TOPIC_EVENT, json);
  // }
}

void sampleSensors() {
  gasRaw = analogRead(PIN_GAS);
  gasBuf[bufIdx] = gasRaw;
  bufIdx = (bufIdx + 1) % BUF_N;
  if (bufCount < BUF_N) bufCount++;

  long sum = 0;
  for (int i = 0; i < bufCount; i++) sum += gasBuf[i];
  gasAvg = sum / bufCount;
}

void setLock(const char* alasan) {
  if (bahayaLock) return;
  bahayaLock = true;
  logEvent("BAHAYA terkunci: %s (ADC=%d)", alasan, gasAvg);
}

bool tryReset(const char* src) {
  if (warmup) {
    logEvent("Reset diabaikan (%s): masih warm-up", src);
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
  
  bahayaLock = false;
  gasDetected = false;
  worsening = false;
  riseCount = 0;
  gasStartTime = 0;
  gasAtTrendStart = 0;
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
        lastTrendTime = now;
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

  Serial.printf("[LOG] t=%lus | ADC: %4d | Status: %-8s | Katup: %-5s | Listrik: %-5s | Alarm: %s | Net: %s\n",
                now / 1000, gasAvg, STATUS_STR[currentStatus()],
                valveOpen ? "BUKA" : "TUTUP",
                powerOn ? "NYALA" : "PUTUS",
                buzzerOn ? "ON" : "OFF",
                mqttOk ? "MQTT" : (wifiOk ? "WIFI" : "OFF"));
}

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
//  UPDATE OUTPUTS (TIERED SAFETY RESPONSE)
// ============================================================
void updateOutputs(unsigned long now) {
  digitalWrite(PIN_LED_G, !gasDetected && !bahayaLock && !warmup);
  digitalWrite(PIN_LED_R, gasDetected || bahayaLock);

  bool wantValveOpen = (!warmup && !bahayaLock && !gasDetected && !worsening);
  if (wantValveOpen != valveOpen) {
    valveOpen = wantValveOpen;
    valveServo.write(valveOpen ? SERVO_OPEN : SERVO_CLOSED);
    logEvent(valveOpen ? "Katup Gas DIBUKA" : "Katup Gas DITUTUP");
  }

  bool wantPowerOn = (!warmup && !bahayaLock); 
  if (wantPowerOn != powerOn) {
    powerOn = wantPowerOn;
    digitalWrite(PIN_RELAY, powerOn ? HIGH : LOW);
    logEvent(powerOn ? "Listrik NYALA" : "Listrik DIPUTUS TOTAL (Darurat)");
  }

  bool on = false;
  if (bahayaLock)       on = (now % 300) < 150;     
  else if (gasDetected) on = (now % 1000) < 100;    

  if (on != buzzerOn) {
    buzzerOn = on;
    if (on) tone(PIN_BUZZER, bahayaLock ? 2500 : 1500);
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
  display.printf("Katup:%s Listrik:%s", valveOpen ? "BUKA" : "TUTUP", powerOn ? "ON" : "OFF");

  display.setCursor(0, 53);
  if (wifiOk) display.print(WiFi.localIP().toString());
  else        display.print("Mencari WiFi...");

  display.display();
}

// ============================================================
//  TASK JARINGAN (core 0)
// ============================================================
void netTask(void*) {
  Serial.println("[WIFI] Memulai koneksi ke Wokwi-GUEST...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);

  unsigned long lastPrint = 0;
  // [AKTIFKAN MQTT 4]: Variabel untuk timer MQTT
  // unsigned long lastMqttTry = 0, lastMqttPub = 0;

  for (;;) {
    wifiOk = (WiFi.status() == WL_CONNECTED);

    if (!wifiOk) {
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
      
      // ========================================================
      // [AKTIFKAN MQTT 5]: Blok Koneksi & Publish MQTT
      // Hapus tanda // pada blok ini untuk mengaktifkan MQTT
      // ========================================================
      /*
      if (!mqtt.connected()) {
        mqttOk = false;
        if (millis() - lastMqttTry > 5000) {
          lastMqttTry = millis();
          Serial.println("[MQTT] Mencoba connect ke HiveMQ...");
          mqtt.setServer(MQTT_HOST, MQTT_PORT);
          
          String cid = String(DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
          if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
            Serial.println("[MQTT] Terhubung!");
            mqttOk = true;
          } else {
            Serial.printf("[MQTT] Gagal, rc=%d\n", mqtt.state());
          }
        }
      } else {
        mqtt.loop();
        mqttOk = true;
        
        // Publish Telemetry tiap 1 detik
        if (millis() - lastMqttPub > 1000) {
          lastMqttPub = millis();
          char json[250];
          snprintf(json, sizeof(json),
            "{\"device\":\"%s\",\"uptime_s\":%lu,\"adc\":%d,\"adc_raw\":%d,"
            "\"status\":\"%s\",\"valve\":\"%s\",\"power\":\"%s\","
            "\"alarm\":%s,\"lock\":%s,\"threshold\":%d}",
            DEVICE_ID, millis()/1000, gasAvg, gasRaw, 
            STATUS_STR[currentStatus()], 
            valveOpen ? "OPEN" : "CLOSED", 
            powerOn ? "ON" : "OFF",
            buzzerOn ? "true" : "false", 
            bahayaLock ? "true" : "false", 
            GAS_ON);
          mqtt.publish(TOPIC_TELEMETRY, json);
        }
      }
      */
      // ========================================================
      // AKHIR DARI BLOK MQTT
      // ========================================================
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n=== SafeGas v12 Booting ===");

  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BTN_RST, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetPinAttenuation(PIN_GAS, ADC_11db);

  digitalWrite(PIN_RELAY, LOW);          
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
  display.println("SafeGas booting...");
  display.display();

  xTaskCreatePinnedToCore(netTask, "net", 4096, NULL, 1, NULL, 0);

  bootMs = millis();
  Serial.println("Warm-up MQ-2...");
}

void loop() {
  unsigned long now = millis();

  cekTombolReset();

  if (now - tSample >= SAMPLE_MS) { tSample = now; sampleSensors(); }
  if (now - tEval   >= EVAL_MS)   { tEval = now;   evaluate(now); }

  updateOutputs(now);

  if (now - tOled >= OLED_MS) { tOled = now; drawOled(now); }
}
