// ============================================================
//  Mock Arduino/ESP32 minimal — untuk menguji LOGIKA SAFETY
//  sketch.ino di komputer (bukan di ESP32).
//
//  Tujuan: memverifikasi state machine, hysteresis, trend,
//  interlock reset, sensor fault, dan konversi ppm.
// ============================================================
#ifndef MOCK_ARDUINO_H
#define MOCK_ARDUINO_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <deque>

// ---------- tipe dasar ----------
typedef uint8_t byte;

#define HIGH 1
#define LOW  0
#define INPUT_PULLUP 2
#define OUTPUT 1
#define INPUT 0

#define WL_CONNECTED 3
#define WIFI_STA 1
#define HEX 16

// ---------- String minimal (cukup untuk sketch) ----------
struct String {
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(unsigned long v, int base = 10) {
    char b[32];
    if (base == 16) snprintf(b, sizeof(b), "%lx", v);
    else snprintf(b, sizeof(b), "%lu", v);
    s = b;
  }
  String operator+(const String& o) const { String r; r.s = s + o.s; return r; }
  String operator+(const char* o) const { String r; r.s = s + o; return r; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator==(const char* o) const { return s == std::string(o ? o : ""); }
  const char* c_str() const { return s.c_str(); }
};
inline String operator+(const char* a, const String& b) { return String(a) + b; }

#define ADC_11db 3

#define SSD1306_WHITE 1
#define SSD1306_SWITCHCAPVCC 2

#define pdTRUE 1
#define pdFALSE 0

// ---------- waktu & kontrol (dikendalikan test harness) ----------
extern unsigned long g_millis;
inline unsigned long millis() { return g_millis; }

// ---------- ADC simulasi ----------
extern int g_adcValue;
inline int analogRead(int) { return g_adcValue; }
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int, int) {}

// ---------- GPIO simulasi ----------
extern int g_pinState[64];
inline void pinMode(int, int) {}
inline void digitalWrite(int pin, int val) { if (pin >= 0 && pin < 64) g_pinState[pin] = val; }
extern int g_btnState;
inline int digitalRead(int pin) { return (pin == 25) ? g_btnState : HIGH; }

// ---------- tone ----------
extern int g_toneFreq;
inline void tone(int, int freq) { g_toneFreq = freq; }
inline void noTone(int) { g_toneFreq = 0; }

struct IPMock { std::string s{"192.168.1.50"}; std::string toString() const { return s; } };

// ---------- Serial mock ----------
struct SerialMock {
  std::string buffer;
  void begin(long) {}
  void printf(const char* fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buffer += tmp;
  }
  void println(const char* s = "") { buffer += s; buffer += "\n"; }
  void println(const std::string& s) { buffer += s; buffer += "\n"; }
  void print(const char* s) { buffer += s; }
  void print(const std::string& s) { buffer += s; }
  void print(const IPMock& ip) { buffer += ip.s; }
  void println(const IPMock& ip) { buffer += ip.s; buffer += "\n"; }
  template <typename T> void println(T v) { buffer += std::to_string(v); buffer += "\n"; }
};
extern SerialMock Serial;

// ---------- delay / task ----------
inline void delay(unsigned long ms) { g_millis += ms; }
inline void vTaskDelay(int) {}

// ---------- FreeRTOS queue mock ----------
struct QueueMock {
  std::deque<std::string> items;
};
typedef QueueMock* QueueHandle_t;

inline QueueHandle_t xQueueCreate(int, int) { return new QueueMock(); }

template <typename T>
inline int xQueueSend(QueueHandle_t q, T* item, int) {
  if (!q) return pdFALSE;
  q->items.push_back(std::string((const char*)item));
  return pdTRUE;
}

template <typename T>
inline int xQueueReceive(QueueHandle_t q, T* out, int) {
  if (!q || q->items.empty()) return pdFALSE;
  std::string s = q->items.front();
  q->items.pop_front();
  memset(out, 0, sizeof(T));
  strncpy((char*)out, s.c_str(), sizeof(T) - 1);
  return pdTRUE;
}

#define portTICK_PERIOD_MS 1
#define esp_random() 12345
#define xTaskCreatePinnedToCore(fn, name, stack, arg, prio, handle, core) \
  do { (void)(fn); (void)(name); (void)(stack); (void)(arg); (void)(prio); (void)(handle); (void)(core); } while (0)

// ---------- WiFi mock ----------
struct WiFiMock {
  int mode(int) { return 0; }
  int begin(const char*, const char*, int = 0) { return 0; }
  int status() { return WL_CONNECTED; }
  IPMock localIP() { return IPMock(); }
  int RSSI() { return -60; }
};
extern WiFiMock WiFi;

// ---------- OLED mock ----------
struct Adafruit_SSD1306 {
  Adafruit_SSD1306(int, int, void*, int) {}
  bool begin(int, int) { return true; }
  void clearDisplay() {}
  void setTextColor(int) {}
  void setTextSize(int) {}
  void setCursor(int, int) {}
  void print(const char*) {}
  void print(const std::string&) {}
  void printf(const char*, ...) {}
  void println(const char*) {}
  void display() {}
};

// ---------- Servo mock ----------
struct Servo {
  int angle = -1;
  void setPeriodHertz(int) {}
  void attach(int, int, int) {}
  void write(int a) { angle = a; }
};

struct ESP32PWM { static void allocateTimer(int) {} };

// ---------- Wire mock ----------
struct WireMock { void begin(int, int) {} };
extern WireMock Wire;

// ---------- WiFiClientSecure / PubSubClient mock ----------
struct WiFiClientSecure {
  void setInsecure() {}
  void setCACert(const char*) {}
};
struct WiFiClient {};

struct PubSubClient {
  PubSubClient(WiFiClientSecure&) {}
  PubSubClient(WiFiClient&) {}
  void setBufferSize(int) {}
  void setServer(const char*, int) {}
  void setCallback(void (*)(char*, byte*, unsigned int)) {}
  bool connect(const char*, const char*, const char*) { return true; }
  bool connect(const char*) { return true; }
  bool connected() { return true; }
  void loop() {}
  bool subscribe(const char*) { return true; }
  bool publish(const char*, const char*) { return true; }
  int state() { return 0; }
};

#endif // MOCK_ARDUINO_H
