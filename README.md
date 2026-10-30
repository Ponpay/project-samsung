# SafeGas — Smart LPG Leakage Prevention System

**Samsung Innovation Campus Batch 8 · Study Case Stage 2 (IoT: Device & Hardware Discovery)**

Sistem deteksi **dan pencegahan** kebocoran gas LPG rumah tangga berbasis ESP32.
Berbeda dari alarm gas konvensional yang hanya memberi peringatan, SafeGas melakukan
**mitigasi tertutup (closed-loop)**: menutup katup regulator secara mekanis **dan**
memutus aliran listrik rumah untuk menghilangkan sumber percikan api.

Dashboard web menampilkan seluruh telemetri **secara realtime** (push lewat SSE,
latensi terukur ±0,1–0,3 detik) — tanpa akun, tanpa broker, tanpa internet.

---

## Daftar Isi

1. [Struktur Repositori](#1-struktur-repositori)
2. [Arsitektur Sistem](#2-arsitektur-sistem)
3. [Pemetaan Pin / GPIO](#3-pemetaan-pin--gpio)
4. [Logika Program — State Machine](#4-logika-program--state-machine)
5. [Cara Menjalankan di Wokwi](#5-cara-menjalankan-di-wokwi) ← **mulai di sini**
6. [Cara Menjalankan Dashboard](#6-cara-menjalankan-dashboard)
7. [Cara Memastikan Dashboard Realtime](#7-cara-memastikan-dashboard-realtime)
8. [Format Data (CSV & MQTT)](#8-format-data-csv--mqtt)
9. [Skenario Demo / Pengujian](#9-skenario-demo--pengujian)
10. [Pengujian Otomatis](#10-pengujian-otomatis)
11. [Catatan Perubahan dari v12](#11-catatan-perubahan-dari-v12)
12. [Troubleshooting](#12-troubleshooting)
13. [Keterbatasan](#13-keterbatasan)

---

## 1. Struktur Repositori

```
project-samsung/
│
├── sketch.ino                     ← FIRMWARE. Ini yang di-paste ke Wokwi.
├── diagram.json                   ← WIRING. Ini yang di-paste ke Wokwi.
├── libraries.txt                  ← DAFTAR LIBRARY. Ini yang di-paste ke Wokwi.
│
├── dashboard/                     ← DASHBOARD WEB REALTIME
│   ├── index.html                 #   UI (tema gelap & terang, grafik, log event)
│   ├── safegas_dashboard.py       #   Backend: serial / mqtt / sim / replay
│   └── bridge_wokwi.py            #   Jembatan Serial Monitor Wokwi → dashboard
│
├── tests/                         ← UJI OTOMATIS (tanpa hardware)
│   ├── run_tests.py               #   35 assertion logika safety (sketch.ino asli)
│   ├── test_main.cpp              #   Isi assertion-nya
│   ├── mock/mock_arduino.h        #   Mock Arduino/ESP32 untuk kompilasi host
│   ├── test_realtime_pipeline.py  #   E2E: Serial → dashboard → SSE
│   ├── test_mqtt_pipeline.py      #   E2E: MQTT → dashboard → SSE
│   └── simulate_serial_output.py  #   Pemalsu output Serial (hanya untuk uji)
│
├── docs/                          ← DOKUMEN PENDUKUNG (PDF)
│   ├── PRD_SafeGas.pdf
│   ├── Arsitektur_Sistem.pdf
│   ├── Komponen_dan_Pinout.pdf
│   ├── Laporan_Perubahan_SafeGas_v12_ke_v13.pdf
│   └── Guideline_Stage2_SIC8.pdf
│
├── legacy/
│   └── sketch_v12_original.ino    # Kode asli tim (arsip, untuk perbandingan)
│
├── requirements.txt               # Dependensi Python (opsional: paho-mqtt)
├── .gitignore
└── README.md
```

**Aturan penting:** hanya **3 file** yang masuk ke Wokwi — `sketch.ino`,
`diagram.json`, dan `libraries.txt`. Folder `dashboard/` jalan di komputer, bukan
di Wokwi.

---

## 2. Arsitektur Sistem

```
   INPUT              PROCESSING                OUTPUT
┌──────────┐      ┌────────────────┐      ┌──────────────┐
│ MQ-2     │─ADC─▶│                │─PWM─▶│ Servo        │─▶ Knop regulator LPG
│ (gas)    │      │                │      └──────────────┘
└──────────┘      │   ESP32        │      ┌──────────────┐
┌──────────┐      │   Dual Core    │─GPIO▶│ Relay        │─▶ Listrik rumah (MCB)
│ Push     │─GPIO▶│                │      └──────────────┘
│ Button   │      │  Core 1: safety│      ┌──────────────┐
└──────────┘      │  Core 0: net   │─GPIO▶│ Buzzer, LED  │
                  │                │      └──────────────┘
                  └───────┬────────┘      ┌──────────────┐
                          │─I2C──────────▶│ OLED SSD1306 │
                          │               └──────────────┘
                          │
                   Serial (CSV)  ──────┐
                   atau MQTT            │
                                        ▼
                          ┌──────────────────────────┐
                          │ bridge_wokwi.py          │
                          │ (tempel Serial Monitor)  │
                          └────────────┬─────────────┘
                                       ▼  serial.log
                          ┌──────────────────────────┐
                          │ safegas_dashboard.py     │
                          │ HTTP + SSE               │
                          └────────────┬─────────────┘
                                       ▼
                          ┌──────────────────────────┐
                          │ Browser                  │
                          │ http://localhost:8080    │
                          └──────────────────────────┘
```

**Pemisahan core** — keputusan arsitektur utama:

- **Core 1**: sampling sensor, state machine safety, aktuator, OLED.
  Tidak pernah menunggu jaringan.
- **Core 0**: WiFi, MQTT, reconnect. Kegagalan jaringan tidak memengaruhi safety.

---

## 3. Pemetaan Pin / GPIO

| Pin ESP32 | Terhubung ke | Tipe Sinyal | Keterangan |
|---|---|---|---|
| D34 | MQ-2 AOUT | Analog (ADC1, 12-bit, 11 dB) | Hanya ADC1 yang aman dipakai saat WiFi aktif |
| D18 | Servo PWM | Digital output (PWM 50 Hz) | 0° = tutup, 90° = buka |
| D19 | Relay IN | Digital output | Memutus listrik rumah |
| D23 | Buzzer | Digital output (tone) | 1500 Hz waspada / 2500 Hz bahaya |
| D25 | Push button | Digital input (pull-up) | Reset manual, active-LOW |
| D13 | LED hijau | Digital output | Indikator NORMAL |
| D32 | LED merah | Digital output | Indikator ADA GAS / BAHAYA |
| D21 | OLED SDA | I2C data | SSD1306 128×64, alamat 0x3C |
| D22 | OLED SCL | I2C clock | — |
| 3V3 | MQ-2 VCC, OLED VCC, relay COM | Power | — |
| VIN | Servo V+, relay VCC | Power (5 V) | Servo butuh 5 V |
| GND | Semua komponen | Ground | — |

---

## 4. Logika Program — State Machine

| State | Kondisi masuk | Katup | Listrik | Alarm | Reset |
|---|---|---|---|---|---|
| `WARMUP` | 5 detik pertama | TUTUP | PUTUS | OFF | diabaikan |
| `NORMAL` | ADC < 1300 | BUKA | NYALA | OFF | — |
| `ADA_GAS` | ADC ≥ 1500 | TUTUP | PUTUS | 1500 Hz lambat | — |
| `MEMBURUK` | tren naik ≥ 50/detik | TUTUP | PUTUS | 1500 Hz | — |
| `BAHAYA` | ADC ≥ 2800 **atau** gas > 15 s **atau** 2× tren naik | TUTUP | PUTUS | 2500 Hz cepat | **manual wajib** |
| `SENSOR_FAULT` | ADC < 30 atau > 4080 | TUTUP | PUTUS | 2500 Hz | setelah sensor pulih |

**Mekanisme penting:**

1. **Moving average buffer 8** — meredam noise ADC (sampling 250 ms).
2. **Hysteresis 1500 / 1300** — ambang masuk ≠ ambang keluar, mencegah
   katup buka-tutup berulang (chattering).
3. **Deteksi tren** — bukan hanya "ada gas", tapi "gas naik cepat".
   Delta ≥ 50 per detik selama 2 jendela berturut-turut → BAHAYA.
4. **Safety interlock** — reset **hanya** berfungsi jika status BAHAYA **dan**
   ADC < 1300 selama 3 detik. Pengguna tidak bisa membungkam alarm saat gas
   masih ada.
5. **Fail-safe boot** — relay OFF dan katup TUTUP sejak `setup()`.
6. **Fail-safe sensor** — sensor putus/rusak tidak dibaca sebagai "udara bersih".
7. **Heartbeat lintas-core** — jika Core 1 hang > 3 detik, Core 0 memaksa
   katup tutup + listrik putus.
8. **Listrik diputus saat `ADA_GAS`** — bukan hanya saat BAHAYA
   (ini perbaikan bug v12; lihat [bagian 11](#11-catatan-perubahan-dari-v12)).

---

## 5. Cara Menjalankan di Wokwi

### 5.1 Yang perlu disiapkan

| Kebutuhan | Keterangan |
|---|---|
| Browser (Chrome/Edge/Firefox) | Wokwi jalan di browser, tidak ada instalasi |
| Akun Wokwi | Gratis, daftar di <https://wokwi.com> |
| 3 file dari repo ini | `sketch.ino`, `diagram.json`, `libraries.txt` |

### 5.2 Membuat proyek Wokwi (5 menit)

1. Buka <https://wokwi.com> → login → **+ New Project** → pilih **ESP32**
   (bukan ESP32-C3 / Arduino).
2. Wokwi membuat proyek dengan dua tab: **sketch.ino** dan **diagram.json**.
3. **Tab `sketch.ino`** — hapus semua isi (Ctrl+A → Delete), lalu tempel
   **seluruh isi file `sketch.ino`** dari repo ini. Pastikan baris terakhir
   `vTaskDelay(1);` dan `}` ikut ter-paste.
4. **Tab `diagram.json`** — klik tab `diagram.json`, hapus semua, tempel
   **seluruh isi `diagram.json`** dari repo ini. Setelah itu kanvas akan
   menampilkan ESP32 + sensor gas + servo + relay + OLED + buzzer + 2 LED +
   tombol.
5. **Tab `Library Manager`** (ikon buku di sidebar kiri) → tombol
   **`+` / Add library** → tempel isi `libraries.txt` satu per satu:
   ```
   Adafruit GFX Library
   Adafruit SSD1306
   ESP32Servo
   PubSubClient
   ```
   > Sebenarnya `diagram.json` sudah memuat blok `dependencies` yang membuat
   > Wokwi meng-install keempat library itu otomatis. Langkah ini hanya
   > jaring pengaman kalau Wokwi gagal resolve dependency.

### 5.3 Menjalankan

1. Klik tombol **▶ (Play)** berwarna hijau di kanan atas.
2. Tunggu kompilasi (30–90 detik pertama; setelah itu cached dan cepat).
3. Buka panel **Serial Monitor** di bagian bawah. Output yang benar:

   ```
   === SafeGas v13 Booting ===
   [NET] Mode Serial saja (MQTT nonaktif). Data dikirim lewat baris CSV di Serial Monitor.
   Warm-up MQ-2...
   [EVENT] Kalibrasi R0=... ohm (Rs udara=...)
   [EVENT] Warm-up selesai
   [EVENT] Katup Gas DIBUKA
   [EVENT] Listrik NYALA
   [LOG] t=6s | ADC: 402 |     5ppm | R0:9820 | NORMAL       | Katup:BUKA  | Listrik:NYALA | Alarm:OFF | Net:OFF
   CSV,safegas01,6,402,405,5,NORMAL,OPEN,ON,0,0,1500,9820
   CSV,safegas01,6,402,404,5,NORMAL,OPEN,ON,0,0,1500,9820
   ```

4. **OLED** akan menampilkan `WARMUP` → `NORMAL`, nilai ADC, status katup/listrik.
5. **LED hijau** menyala saat NORMAL. Servo bergerak ke 90° (katup buka).

> **Penting:** `AKTIFKAN_MQTT` bernilai `0` secara default. Artinya firmware
> berjalan **tanpa WiFi/MQTT** dan mengirim data lewat **baris CSV di Serial
> Monitor**. Ini yang dipakai untuk demo — tidak butuh kredensial apa pun.

### 5.4 Mensimulasikan kebocoran gas

Di kanvas Wokwi, **sensor gas (MQ-2)** punya potensiometer. Klik dan **geser
putarannya ke kanan** untuk menaikkan nilai ADC:

| Putaran | ADC kira-kira | Yang terjadi |
|---|---|---|
| kiri penuh | ~400 | `NORMAL` — katup buka, listrik nyala, LED hijau |
| sedang | ~1600 | `ADA_GAS` — katup tutup, listrik putus, buzzer berbunyi pelan, LED merah |
| kanan penuh | ~2900–4095 | `BAHAYA` — terkunci, buzzer cepat 2500 Hz |
| kembali ke kiri | < 1300 | gas hilang, tapi **tetap terkunci** sampai di-reset |

**Reset:** tekan tombol hijau di kanvas (pin D25) — tapi hanya berhasil kalau
gas sudah benar-benar bersih selama 3 detik. Kalau masih tinggi, Serial Monitor
akan menampilkan `Reset DITOLAK`.

---

## 6. Cara Menjalankan Dashboard

Dashboard punya **4 mode**. Untuk demo Wokwi, pakai **mode `serial`**
(paling aman, tanpa jaringan).

### 6.1 Persiapan (sekali saja)

```bash
# Cek Python — butuh 3.8 atau lebih baru
python --version

# Tidak ada yang wajib di-install untuk mode serial/sim/replay.
# Hanya kalau mau pakai mode MQTT:
pip install -r requirements.txt
```

### 6.2 Mode SERIAL — dashboard membaca Serial Monitor Wokwi (dipakai saat demo)

Butuh **dua terminal**.

**Terminal 1 — jalankan dashboard:**

```bash
python dashboard/safegas_dashboard.py --mode serial --serial-file serial.log
```

Akan muncul:

```
==========================================================
  SAFEGAS REALTIME DASHBOARD
==========================================================
  Mode      : serial
  Serial    : serial.log
  Dashboard : http://localhost:8080
==========================================================
[HTTP] dashboard siap di  http://localhost:8080
[SERIAL] mengikuti D:\project-samsung\dashboard\serial.log
```

**Terminal 2 — jalankan jembatan Wokwi:**

```bash
python dashboard/bridge_wokwi.py --out serial.log
```

Akan muncul panduan interaktif:

```
==============================================================
  JEMBATAN WOKWI -> DASHBOARD
==============================================================
  File tujuan : D:\project-samsung\dashboard\serial.log
  Cara pakai  :
    1. Di Wokwi, buka Serial Monitor
    2. Ctrl+A lalu Ctrl+C untuk menyalin
    3. Klik di sini, Ctrl+V, tekan Enter
    4. Ulangi tiap beberapa detik
  Ketik 'q' lalu Enter untuk keluar.
==============================================================
```

**Alur kerjanya:**

1. Di Wokwi, klik area **Serial Monitor**.
2. **Ctrl+A** (pilih semua) lalu **Ctrl+C** (salin).
3. Pindah ke Terminal 2, **Ctrl+V**, tekan **Enter**.
4. Jembatan menyaring hanya baris `CSV,...` dan `[EVENT] ...`,
   lalu menuliskannya ke `serial.log`.
5. Dashboard membaca baris baru itu **dalam ±0,15 detik** dan langsung
   mendorongnya ke browser lewat SSE.

Ulangi langkah 1–3 tiap beberapa detik. Setiap kali kamu paste, grafik
bertambah. Untuk demo laporan, paste tiap 2–3 detik supaya grafiknya
tampak mengalir.

**Buka browser:** <http://localhost:8080>

> **Tombol Reset** hanya aktif di mode `mqtt` dan `sim` (ada jalur perintah ke
> perangkat). Di mode `serial`, tombol itu nonaktif karena file Serial Monitor
> hanya satu arah — reset tetap bisa dilakukan lewat tombol fisik di Wokwi.

### 6.3 Mode SIMULATOR — dashboard tanpa Wokwi

Untuk latihan presentasi atau kalau Wokwi tidak bisa dibuka:

```bash
python dashboard/safegas_dashboard.py --mode sim
```

Simulator meniru state machine `sketch.ino` (termasuk interlock reset) dengan
siklus 6 detik: NORMAL → ADA_GAS → MEMBURUK → BAHAYA → pulih → reset.
Tombol **Reset** di dashboard ikut berfungsi dan tetap melewati interlock.

### 6.4 Mode MQTT — kalau mau lewat jaringan

```bash
pip install paho-mqtt

# HiveMQ Cloud (wajib TLS 8883)
python dashboard/safegas_dashboard.py --mode mqtt \
  --mqtt-host xxxx.s1.eu.hivemq.cloud --mqtt-port 8883 --mqtt-tls \
  --mqtt-user USER --mqtt-pass PASS

# Broker publik untuk uji cepat (non-TLS)
python dashboard/safegas_dashboard.py --mode mqtt \
  --mqtt-host broker.hivemq.com --mqtt-port 1883
```

Di sisi firmware, ubah di `sketch.ino`:

```c
#define AKTIFKAN_MQTT 1
#define MQTT_HOST "xxxx.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "username-anda"
#define MQTT_PASS "password-anda"
```

> **Catatan Wokwi:** WiFi di Wokwi memakai SSID `Wokwi-GUEST` tanpa password
> (sudah di-set di sketch). Koneksi keluar ke internet lewat gateway Wokwi;
> kalau MQTT gagal connect, cek dulu Serial Monitor — biasanya kredensial
> salah, bukan jaringannya.

### 6.5 Mode REPLAY — memutar ulang rekaman

Setiap sesi dashboard otomatis merekam ke `dashboard/logs/*.jsonl`.

```bash
python dashboard/safegas_dashboard.py --mode replay --log-file logs/safegas_serial_20261030_120000.jsonl
```

Berguna untuk membuat screenshot laporan tanpa harus menjalankan Wokwi lagi.

### 6.6 Pilihan argumen lain

| Argumen | Default | Fungsi |
|---|---|---|
| `--http-port` | `8080` | Ganti kalau port 8080 sudah dipakai |
| `--serial-file` | `serial.log` | Nama file jembatan (relatif ke folder `dashboard/`) |
| `--serial-tail` | mati | Mulai baca dari **akhir** file, bukan dari awal |
| `--device` | `safegas01` | ID perangkat; `+` = terima semua device |
| `--speed` | `1.0` | Kecepatan simulator / replay |
| `--no-record` | mati | Jangan tulis `logs/*.jsonl` |

---

## 7. Cara Memastikan Dashboard Realtime

Ini bagian yang membuktikan dashboard **benar-benar** menampilkan data dari
Wokwi secara realtime, bukan data statis.

### 7.1 Cek visual (paling cepat)

| Yang dilihat | Arti |
|---|---|
| Chip koneksi kanan atas bertuliskan **"terhubung"** dengan titik hijau | SSE aktif |
| Baris **"sumber"** di bawah judul bertuliskan `file · serial.log` | dashboard membaca file jembatan |
| Angka **uptime** bertambah tiap kali kamu paste dari Wokwi | data baru masuk |
| Kartu **ADC** dan **ppm** berubah nilainya | parsing CSV berhasil |
| **Grafik** bertambah satu titik tiap paste | data mengalir ke grafik |
| **Log event** muncul baris baru (`Gas terdeteksi!`, `BAHAYA terkunci`, …) | event `[EVENT]` ikut masuk |

Kalau semua itu bergerak saat kamu memutar potensiometer MQ-2 di Wokwi,
**dashboard sudah realtime**.

### 7.2 Cek lewat endpoint (bukti angka)

Dashboard punya dua endpoint JSON yang bisa dibuka di browser:

```bash
# Ringkasan status + jumlah data yang sudah masuk
curl http://localhost:8080/api/health
```

```json
{"ok": true, "meta": {"mode": "serial", "device": "safegas01",
 "connected": true, "source": "file · serial.log", "live": true}}
```

`"live": true` = dashboard dalam mode mengikuti sumber secara langsung.

```bash
# Semua sampel & event yang tersimpan
curl http://localhost:8080/api/state
```

Jalankan perintah itu **dua kali** dengan jeda 3 detik sambil memutar
potensiometer di Wokwi. Kalau `samples` bertambah dan nilai `adc` berubah,
data memang mengalir realtime.

### 7.3 Uji otomatis pipeline realtime

Tersedia skrip yang membuktikan alur Serial → dashboard → SSE bekerja,
termasuk mengukur latensinya:

```bash
python tests/test_realtime_pipeline.py
```

Hasil yang diharapkan:

```
  PASS  dashboard HTTP hidup
  PASS  SSE mengirim data (bukan hanya REST)  [3 frame]
  PASS  latensi push SSE < 3 detik  [0.24s]
  PASS  semua baris CSV ter-parse  [321/321]
  PASS  semua fase state-machine muncul  [['ADA_GAS','BAHAYA','MEMBURUK','NORMAL','WARMUP']]
  PASS  event [EVENT] masuk dashboard  [8 event]
  PASS  ada event BAHAYA di log
  PASS  field telemetri lengkap
  PASS  meta.live = true (realtime)

=== HASIL E2E: 9 pass, 0 fail ===
```

Dan untuk jalur MQTT:

```bash
pip install paho-mqtt
python tests/test_mqtt_pipeline.py
# === HASIL MQTT: 8 pass, 0 fail ===
```

### 7.4 Kenapa latensinya rendah

Dashboard tidak memakai polling. Alurnya push:

```
bridge_wokwi.py  →  serial.log  →  run_serial() thread
                                        ↓  Store.add_sample() + notify_all()
                                   SSE /stream (wait_delta)
                                        ↓  tulis "data: {...}\n\n"
                                   EventSource di browser
                                        ↓  pushSample() → render() → drawChart()
```

`Store.wait_delta()` memakai `threading.Condition` — begitu ada sampel baru,
thread SSE langsung dibangunkan (bukan menunggu timer). Karena itu latensi
terukur ±0,1–0,3 detik, bukan 1–5 detik seperti polling.

---

## 8. Format Data (CSV & MQTT)

### 8.1 Baris CSV di Serial (dipakai mode `serial`)

Dicetak **2× per detik** (setiap 500 ms):

```
CSV,<device>,<uptime_s>,<adc>,<adc_raw>,<ppm>,<status>,<valve>,<power>,<alarm>,<lock>,<threshold>,<r0>
```

Contoh:

```
CSV,safegas01,42,1750,1780,3200,ADA_GAS,CLOSED,OFF,1,0,1500,9820
```

| Kolom | Arti |
|---|---|
| `device` | ID perangkat (`safegas01`) |
| `uptime_s` | Detik sejak boot |
| `adc` | Nilai ADC hasil moving average (dipakai logika) |
| `adc_raw` | Sampel ADC mentah (untuk melihat noise) |
| `ppm` | Estimasi konsentrasi gas dari kurva MQ-2 |
| `status` | `WARMUP` / `NORMAL` / `ADA_GAS` / `MEMBURUK` / `BAHAYA` / `SENSOR_FAULT` |
| `valve` | `OPEN` / `CLOSED` |
| `power` | `ON` / `OFF` |
| `alarm` | `1` = buzzer berbunyi |
| `lock` | `1` = terkunci, butuh reset manual |
| `threshold` | Ambang `GAS_ON` saat itu (1500) |
| `r0` | Resistansi referensi hasil kalibrasi (opsional) |

Event dicetak dengan prefiks `[EVENT]`:

```
[EVENT] Gas terdeteksi! ADC=1700 (3120 ppm)
[EVENT] BAHAYA terkunci: kadar gas kritis (ADC=2950)
[EVENT] Reset DITOLAK (tombol): gas masih tinggi (ADC=1620)
```

### 8.2 MQTT (opsional)

| Topik | Arah | Payload |
|---|---|---|
| `safegas/safegas01/telemetry` | ESP32 → dashboard | JSON, tiap 1 detik |
| `safegas/safegas01/event` | ESP32 → dashboard | JSON, saat ada kejadian |
| `safegas/safegas01/cmd` | dashboard → ESP32 | `reset` (tetap lewat interlock) |

Contoh telemetri:

```json
{"device":"safegas01","uptime_s":42,"adc":1750,"adc_raw":1780,
 "ppm":3200,"r0":9820,"status":"ADA_GAS","valve":"CLOSED","power":"OFF",
 "alarm":true,"lock":false,"threshold":1500,"rssi":-62}
```

> **Penting:** HiveMQ Cloud **wajib TLS port 8883**. Kode v12 memakai port 1883
> (non-TLS) sehingga tidak akan pernah connect ke HiveMQ Cloud.

---

## 9. Skenario Demo / Pengujian

| # | Test Case | Input / Kondisi | Expected Result | Status |
|---|---|---|---|---|
| 1 | Boot & warm-up | Nyalakan sistem, tunggu 5 detik | OLED `WARMUP` → `NORMAL`; katup buka 90°; listrik nyala; LED hijau | ⬜ |
| 2 | Deteksi gas ringan | Putar potensio hingga ADC ≥ 1500 | Status `ADA_GAS`; katup tutup; listrik putus; buzzer 1500 Hz lambat; LED merah | ⬜ |
| 3 | Deteksi gas kritis | Putar hingga ADC ≥ 2800 | Status `BAHAYA` terkunci; buzzer 2500 Hz cepat | ⬜ |
| 4 | Gas bertahan lama | Tahan ADC > 1300 selama 15 detik | Status `BAHAYA` terkunci (alasan: gas bertahan terlalu lama) | ⬜ |
| 5 | Tren naik cepat | Naikkan ADC ≥ 50/detik dua kali berturut-turut | Status `MEMBURUK` → `BAHAYA` | ⬜ |
| 6 | Reset ditolak | Tekan reset saat ADC masih ≥ 1300 | Log `Reset DITOLAK: gas masih tinggi`; sistem tetap `BAHAYA` | ⬜ |
| 7 | Reset berhasil | Turunkan ADC < 1300, tunggu 3 detik, tekan reset | Log `Reset BERHASIL`; kembali `NORMAL`; katup buka; listrik nyala | ⬜ |
| 8 | Fail-safe sensor | Cabut/lepas sensor (ADC ≈ 0) | Status `SENSOR_FAULT`; katup tutup; listrik putus | ⬜ |
| 9 | Reset dari dashboard | Kirim `reset` ke topik `cmd` saat gas masih tinggi | Reset DITOLAK (interlock tetap berlaku) | ⬜ |
| 10 | Dashboard realtime | Paste Serial Monitor ke jembatan saat status berubah | Grafik & kartu metrik ikut berubah < 1 detik | ⬜ |

Kolom **Status** diisi setelah pengujian manual di Wokwi (butuh screenshot
untuk laporan).

---

## 10. Pengujian Otomatis

Logika safety diuji **tanpa ESP32**: `sketch.ino` **asli** dikompilasi dengan
mock Arduino (bukan disalin ulang), lalu dijalankan di komputer.

```bash
python tests/run_tests.py
# atau jika g++ tidak ada di PATH:
python tests/run_tests.py --gxx "C:/path/to/g++.exe"
```

Output:

```
=== HASIL: 35 pass, 0 fail ===
```

| Perintah | Yang diuji | Hasil |
|---|---|---|
| `python tests/run_tests.py` | 35 assertion logika safety | 35 pass, 0 fail |
| `python tests/test_realtime_pipeline.py` | Serial → dashboard → SSE | 9 pass, 0 fail |
| `python tests/test_mqtt_pipeline.py` | MQTT → dashboard → SSE | 8 pass, 0 fail |
| `arduino-cli compile` (mode serial) | Kompilasi firmware | OK — 31% flash, 9% RAM |
| `arduino-cli compile` (mode MQTT) | Kompilasi firmware | OK — 80% flash, 15% RAM |

Yang diuji: fail-safe boot, warm-up, transisi `NORMAL`→`ADA_GAS`→`BAHAYA`,
hysteresis 1500/1300, pemutusan listrik & katup, interlock reset (ditolak saat
gas tinggi, ditolak tanpa dwell, berhasil setelah dwell), `SENSOR_FAULT`,
monotonisitas konversi ppm, dan format baris CSV.

> Catatan: test ini memverifikasi **logika**, bukan timing hardware. Buffer
> moving-average butuh 8 sampel × 250 ms = 2 detik untuk jenuh, jadi test
> memberi margin waktu yang cukup.

---

## 11. Catatan Perubahan dari v12

Lihat `legacy/sketch_v12_original.ino` untuk versi asli.

| # | Perubahan | Alasan |
|---|---|---|
| 1 | MQTT diaktifkan, TLS port 8883 | HiveMQ Cloud menolak non-TLS; v12 pakai 1883 |
| 2 | Event queue FreeRTOS | v12: Core 1 memanggil `mqtt.publish()` → race condition dengan Core 0 |
| 3 | `vTaskDelay(1)` di `loop()` | v12 tanpa yield → risiko task watchdog starvation |
| 4 | Heartbeat lintas-core | v12: jika loop hang, katup tetap terbuka (fail-dangerous) |
| 5 | Deteksi sensor fault | v12: sensor putus terbaca ADC 0 = "udara bersih" = katup dibuka |
| 6 | Konversi ADC → ppm | v12: threshold hanya angka ADC tanpa satuan fisik |
| 7 | Reset dengan dwell time | v12: satu sampel di bawah ambang sudah cukup untuk reset |
| 8 | Baris CSV di Serial | Dashboard bisa jalan tanpa MQTT |
| 9 | Topik `cmd` | Reset dari dashboard, tetap melewati interlock |
| 10 | **Listrik diputus saat `ADA_GAS`** | **Bug v12:** relay hanya putus saat `BAHAYA`, sehingga saat gas terdeteksi listrik tetap nyala — bertentangan dengan FR-04 PRD ("memutus listrik segera setelah gas terdeteksi") dan skenario demo halaman 2. Sumber percikan api tetap hidup. |

**Yang TIDAK diubah:** pin, wiring, dan seluruh ambang batas logika safety.
`diagram.json` hanya ditambah blok `dependencies` agar library terpasang
otomatis di Wokwi.

---

## 12. Troubleshooting

| Gejala | Penyebab | Solusi |
|---|---|---|
| Wokwi gagal kompilasi: `Adafruit_SSD1306.h: No such file` | Library belum terpasang | Tab Library Manager → tambahkan 4 library dari `libraries.txt`. Atau pastikan `diagram.json` yang di-paste sudah memuat blok `dependencies`. |
| OLED hitam total | Alamat I2C salah / SDA-SCL tertukar | Di Wokwi sudah benar (`0x3C`, D21/D22). Kalau di hardware, jalankan I2C scanner. |
| Serial Monitor kosong | Belum klik ▶ / baud salah | Klik Play, tunggu kompilasi. Baud harus **115200**. |
| Tidak ada baris `CSV,...` | `AKTIFKAN_MQTT` = 1 tapi WiFi/MQTT gagal | Set `#define AKTIFKAN_MQTT 0` untuk mode Serial. |
| Dashboard kosong, chip "terputus" | Dashboard belum jalan / port salah | Pastikan Terminal 1 menampilkan `[HTTP] dashboard siap di ...`. Cek port dengan `--http-port`. |
| Dashboard jalan tapi angka tidak bergerak | Belum paste dari Serial Monitor | Paste ulang isi Serial Monitor ke Terminal 2, tekan Enter. |
| `[SERIAL] menunggu file ...` terus | Nama file beda antara 2 terminal | Pastikan `--serial-file` dan `--out` menunjuk nama sama. Default keduanya `serial.log`. |
| `Address already in use` | Port 8080 dipakai aplikasi lain | `--http-port 8081` |
| Grafik kosong padahal ada data | CDN Plotly diblokir | Buka DevTools → Console. Kalau `Plotly is not defined`, sambungkan internet atau ganti `<script src>` Plotly ke file lokal. |
| `Reset DITOLAK: gas masih tinggi` | Interlock bekerja | Ini **benar**. Turunkan ADC < 1300, tunggu 3 detik, baru reset. |
| `SENSOR FAULT` muncul terus | ADC di luar 30–4080 | Di Wokwi: potensiometer diputar terlalu ekstrem / sensor terlepas. |
| Dashboard mode MQTT: `paho-mqtt belum terpasang` | Dependency belum ada | `pip install -r requirements.txt` |
| MQTT `rc=5` (not authorized) | Username/password salah | Cek tab *Access Management* di console HiveMQ. |

---

## 13. Keterbatasan

- **Simulasi, bukan perangkat fisik.** Sensor MQ-2 disimulasikan dengan
  potensiometer; nilai ADC tidak memiliki satuan ppm yang terkalibrasi.
- **Ambang batas** (1500/1300/2800) adalah hasil tuning di Wokwi, bukan hasil
  kalibrasi gas referensi.
- **Servo bukan komponen safety-rated.** Implementasi nyata sebaiknya memakai
  solenoid valve *normally-closed* (tertutup saat tidak berenergi).
- **Relay 1 channel** tidak layak memutus arus rumah penuh; instalasi nyata
  memerlukan kontaktor dan sertifikasi.
- **MQ-2 tidak selektif** — uap masakan dan asap dapat memicu alarm palsu.
  Untuk produksi, pertimbangkan MQ-6 (lebih selektif terhadap LPG).
- **Jembatan Serial butuh paste manual.** Wokwi (versi gratis) tidak
  menyediakan API untuk membaca Serial Monitor langsung, jadi
  `bridge_wokwi.py` menerima paste dari clipboard. Untuk otomatisasi penuh,
  gunakan mode MQTT.

---

## Pengembangan Lanjutan

- Kalibrasi R₀ dengan gas referensi → threshold dalam satuan ppm
- Sensor fusion: MQ-6 (LPG) + MQ-9 (CO)
- Mode "memasak" (suppress sementara dengan auto-rearm)
- Notifikasi Telegram / WhatsApp
- Dashboard lokal di ESP32 (`AsyncWebServer`) agar tidak bergantung komputer
- Penyimpanan log ke NVS / microSD

---

**Lisensi:** proyek akademik Samsung Innovation Campus Batch 8.
