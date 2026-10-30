// ============================================================
//  Test harness: menjalankan LOGIKA ASLI sketch.ino di komputer.
//  sketch.ino di-include apa adanya (bukan disalin ulang).
// ============================================================
#include "mock_arduino.h"

// ---- definisi global mock ----
unsigned long g_millis = 0;
int g_adcValue = 400;
int g_pinState[64] = {0};
int g_btnState = HIGH;
int g_toneFreq = 0;
SerialMock Serial;
WiFiMock WiFi;
WireMock Wire;

// ---- override library yang tidak kita uji ----
#define Wire mock_unused_wire_guard
#undef Wire
#include "sketch_for_test.ino"

int g_fail = 0, g_pass = 0;

void ck(const char* name, bool cond, const char* detail = "") {
  if (cond) { g_pass++; printf("  PASS  %s\n", name); }
  else { g_fail++; printf("  FAIL  %s  [%s]\n", name, detail); }
}

// majukan waktu + jalankan satu iterasi loop()
void tick(unsigned long ms = 1) {
  g_millis += ms;
  loop();
}

// set ADC lalu jalankan beberapa evaluasi penuh
void setAdc(int v) { g_adcValue = v; }

std::string serialTail() {
  std::string s = Serial.buffer;
  return s.size() > 400 ? s.substr(s.size() - 400) : s;
}

int main() {
  printf("=== UJI LOGIKA SAFETY (sketch.ino asli, mock Arduino) ===\n\n");

  // ---------- boot ----------
  setup();
  ck("boot: relay OFF (listrik putus)", g_pinState[PIN_RELAY] == LOW,
     std::to_string(g_pinState[PIN_RELAY]).c_str());
  ck("boot: servo TUTUP (0 deg)", valveServo.angle == SERVO_CLOSED,
     std::to_string(valveServo.angle).c_str());
  ck("boot: status WARMUP", currentStatus() == ST_WARMUP,
     STATUS_STR[currentStatus()]);

  // ---------- warm-up ----------
  setAdc(400);
  for (int i = 0; i < 6000; i++) tick(1);
  ck("warmup selesai -> NORMAL", currentStatus() == ST_NORMAL, STATUS_STR[currentStatus()]);
  ck("NORMAL: katup BUKA", valveOpen == true);
  ck("NORMAL: listrik NYALA", g_pinState[PIN_RELAY] == HIGH,
     std::to_string(g_pinState[PIN_RELAY]).c_str());
  ck("NORMAL: LED hijau ON", g_pinState[PIN_LED_G] == HIGH);
  ck("NORMAL: LED merah OFF", g_pinState[PIN_LED_R] == LOW);
  ck("NORMAL: alarm OFF", buzzerOn == false);

  // ---------- gas ringan ----------
  // buffer moving-average 8 x 250ms = 2s; beri margin cukup
  setAdc(1600);
  for (int i = 0; i < 10000; i++) tick(1);
  ck("gas 1600 -> ADA_GAS", currentStatus() == ST_ADA_GAS, STATUS_STR[currentStatus()]);
  ck("ADA_GAS: katup TUTUP", valveOpen == false);
  { char dbg[200]; snprintf(dbg, sizeof(dbg),
      "status=%s relay=%d powerOn=%d gasDetected=%d worsening=%d lock=%d",
      STATUS_STR[currentStatus()], g_pinState[PIN_RELAY], (int)powerOn,
      (int)gasDetected, (int)worsening, (int)bahayaLock);
    ck("ADA_GAS: listrik PUTUS", g_pinState[PIN_RELAY] == LOW, dbg); }
  ck("ADA_GAS: LED merah ON", g_pinState[PIN_LED_R] == HIGH);

  // ---------- hysteresis: turun ke 1400 tetap ADA_GAS ----------
  setAdc(1400);
  for (int i = 0; i < 4000; i++) tick(1);
  ck("hysteresis: 1400 masih ADA_GAS", currentStatus() == ST_ADA_GAS,
     STATUS_STR[currentStatus()]);

  // ---------- turun di bawah GAS_OFF -> NORMAL ----------
  setAdc(1200);
  for (int i = 0; i < 4000; i++) tick(1);
  ck("turun ke 1200 -> NORMAL", currentStatus() == ST_NORMAL, STATUS_STR[currentStatus()]);

  // ---------- gas kritis -> BAHAYA ----------
  setAdc(2900);
  for (int i = 0; i < 10000; i++) tick(1);
  ck("gas 2900 -> BAHAYA", currentStatus() == ST_BAHAYA, STATUS_STR[currentStatus()]);
  ck("BAHAYA: lock aktif", bahayaLock == true);
  ck("BAHAYA: katup TUTUP", valveOpen == false);
  ck("BAHAYA: listrik PUTUS", g_pinState[PIN_RELAY] == LOW);

  // ---------- reset DITOLAK saat gas masih tinggi ----------
  setAdc(1600);
  for (int i = 0; i < 4000; i++) tick(1);
  Serial.buffer.clear();
  bool r1 = tryReset("test", g_millis);
  ck("reset saat gas tinggi DITOLAK", r1 == false);
  ck("reset ditolak: tetap BAHAYA", currentStatus() == ST_BAHAYA);
  ck("reset ditolak: ada log 'DITOLAK'",
     Serial.buffer.find("DITOLAK") != std::string::npos, serialTail().c_str());

  // ---------- reset DITOLAK saat gas bersih tapi belum dwell ----------
  setAdc(1000);
  for (int i = 0; i < 1200; i++) tick(1);   // 1.2 dtk < RESET_DWELL_MS (3 dtk)
  Serial.buffer.clear();
  bool r2 = tryReset("test", g_millis);
  ck("reset tanpa dwell DITOLAK", r2 == false);
  ck("reset tanpa dwell: ada log 'tunggu'",
     Serial.buffer.find("tunggu") != std::string::npos, serialTail().c_str());

  // ---------- reset BERHASIL setelah dwell ----------
  for (int i = 0; i < 4000; i++) tick(1);   // lewati dwell
  Serial.buffer.clear();
  bool r3 = tryReset("test", g_millis);
  ck("reset setelah dwell BERHASIL", r3 == true);
  ck("reset berhasil: NORMAL", currentStatus() == ST_NORMAL, STATUS_STR[currentStatus()]);
  ck("reset berhasil: ada log 'BERHASIL'",
     Serial.buffer.find("BERHASIL") != std::string::npos, serialTail().c_str());

  // ---------- sensor fault: ADC 0 ----------
  setAdc(0);
  for (int i = 0; i < 4000; i++) tick(1);
  ck("ADC 0 -> SENSOR_FAULT", currentStatus() == ST_SENSOR_FAULT, STATUS_STR[currentStatus()]);
  ck("SENSOR_FAULT: katup TUTUP", valveOpen == false);
  ck("SENSOR_FAULT: listrik PUTUS", g_pinState[PIN_RELAY] == LOW);

  // ---------- sensor pulih ----------
  setAdc(500);
  for (int i = 0; i < 4000; i++) tick(1);
  ck("sensor pulih -> bukan SENSOR_FAULT", currentStatus() != ST_SENSOR_FAULT,
     STATUS_STR[currentStatus()]);

  // ---------- konversi ppm monoton ----------
  float p_low  = rsToPpm(400);
  float p_mid  = rsToPpm(1600);
  float p_high = rsToPpm(2900);
  ck("ppm naik seiring ADC", p_low < p_mid && p_mid < p_high);
  printf("        ppm: adc400=%.0f  adc1600=%.0f  adc2900=%.0f\n", p_low, p_mid, p_high);
  ck("ppm di rentang wajar (0..10000)", p_low >= 0 && p_high <= 10000);

  // ---------- CSV line ----------
  Serial.buffer.clear();
  printCsv(g_millis);
  ck("printCsv menghasilkan baris CSV",
     Serial.buffer.rfind("CSV,safegas01,", 0) == 0, serialTail().c_str());
  printf("        %s", Serial.buffer.c_str());

  // ---------- event queue ----------
  // Queue hanya dibuat saat AKTIFKAN_MQTT=1 (mode Wokwi serial tidak butuh).
#if AKTIFKAN_MQTT
  ck("event queue dibuat saat MQTT aktif", eventQueue != NULL);
#else
  ck("mode serial: tidak ada queue MQTT (tidak membebani memori)",
     eventQueue == NULL);
#endif

  printf("\n=== HASIL: %d pass, %d fail ===\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
