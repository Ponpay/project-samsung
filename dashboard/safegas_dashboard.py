#!/usr/bin/env python3
"""
SafeGas Realtime Dashboard — Backend
=====================================
Menerima telemetri & event dari ESP32, menyimpannya, lalu menyajikannya
ke browser secara realtime lewat Server-Sent Events (SSE).

Empat mode input:

  serial  -> baca baris CSV dari Serial Monitor yang disimpan ke file
             (atau dari stdin). TIDAK butuh MQTT, TIDAK butuh akun.
             Cocok untuk demo kalau jaringan bermasalah.
  mqtt    -> subscribe ke broker MQTT (HiveMQ Cloud TLS 8883 / publik 1883)
  sim     -> simulator internal, meniru state machine sketch.ino
  replay  -> putar ulang file log .jsonl hasil rekaman

Contoh:
  python safegas_dashboard.py --mode sim
  python safegas_dashboard.py --mode mqtt --mqtt-host xxx.s1.eu.hivemq.cloud \
      --mqtt-port 8883 --mqtt-tls --mqtt-user USER --mqtt-pass PASS
  python safegas_dashboard.py --mode serial --serial-file serial.log
  python safegas_dashboard.py --mode replay --log-file logs/x.jsonl

Buka http://localhost:8080

Dependensi: mode sim/replay/serial = stdlib saja.
            mode mqtt butuh: pip install paho-mqtt
"""

import argparse
import json
import os
import random
import re
import signal
import sys
import threading
import time
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(BASE_DIR, "logs")
INDEX_FILE = os.path.join(BASE_DIR, "index.html")

# Ambang batas — HARUS sama dengan sketch.ino
GAS_ON = 1500
GAS_OFF = 1300
GAS_CRITICAL = 2800
PERSIST_LOCK_MS = 15000
TREND_RISE_MIN = 50

# Format baris CSV dari sketch.ino (printCsv):
# CSV,<device>,<uptime_s>,<adc>,<adc_raw>,<ppm>,<status>,<valve>,<power>,<alarm>,<lock>,<threshold>[,<r0>]
CSV_RE = re.compile(
    r"^CSV,([^,]+),(\d+),(-?\d+),(-?\d+),(-?[\d.]+),([A-Z_]+),"
    r"([A-Z]+),([A-Z]+),(\d),(\d),(\d+)(?:,(-?[\d.]+))?")


# ============================================================
#  PENYIMPANAN
# ============================================================
class Store:
    def __init__(self, max_samples=5000):
        self.cond = threading.Condition()
        self.samples = deque(maxlen=max_samples)
        self.events = deque(maxlen=2000)
        self.s_seq = 0
        self.e_seq = 0
        self.meta = {"mode": "-", "device": "-", "connected": False,
                     "source": "-", "started": time.time(), "live": False,
                     "cmd": False}   # cmd=True hanya bila perintah reset didukung
        self.cmd_sink = None   # callable(str) untuk kirim perintah ke device

    def add_sample(self, d):
        with self.cond:
            self.s_seq += 1
            d = dict(d); d["_seq"] = self.s_seq; d["_recv"] = time.time()
            self.samples.append(d)
            self.cond.notify_all()
            return d

    def add_event(self, d):
        with self.cond:
            self.e_seq += 1
            d = dict(d); d["_seq"] = self.e_seq; d["_recv"] = time.time()
            self.events.append(d)
            self.cond.notify_all()
            return d

    def set_meta(self, **kw):
        with self.cond:
            self.meta.update(kw)
            self.cond.notify_all()

    def snapshot(self):
        with self.cond:
            return {"samples": list(self.samples), "events": list(self.events),
                    "meta": dict(self.meta), "s_seq": self.s_seq, "e_seq": self.e_seq}

    def wait_delta(self, since_s, since_e, timeout=20.0):
        with self.cond:
            if self.s_seq <= since_s and self.e_seq <= since_e:
                self.cond.wait(timeout)
            new_s = [s for s in self.samples if s["_seq"] > since_s]
            new_e = [e for e in self.events if e["_seq"] > since_e]
            return new_s, new_e, dict(self.meta), self.s_seq, self.e_seq


# ============================================================
#  LOG
# ============================================================
class LogWriter:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.fh = None
        if path:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            self.fh = open(path, "a", encoding="utf-8")
            print(f"[LOG] Merekam ke {path}")

    def write(self, kind, payload):
        if not self.fh:
            return
        rec = {"ts": datetime.now().isoformat(timespec="milliseconds"),
               "kind": kind, "data": payload}
        with self.lock:
            self.fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
            self.fh.flush()

    def close(self):
        with self.lock:
            if self.fh:
                try: self.fh.close()
                except Exception: pass


# ============================================================
#  MODE 1: SERIAL (tanpa MQTT)
# ============================================================
def run_serial(store, writer, args):
    """Ikuti file log Serial Monitor; parse baris CSV & [EVENT]."""
    # Path relatif diartikan relatif terhadap folder dashboard/ agar perintah
    # bisa dijalankan dari mana saja (tidak bergantung cwd).
    path = args.serial_file
    if not os.path.isabs(path):
        path = os.path.join(BASE_DIR, path)
    path = os.path.abspath(path)
    print(f"[SERIAL] mengikuti {path}")
    # Dashboard selalu mengikuti file ini secara live (baris baru langsung
    # terbaca dan di-push lewat SSE). --serial-tail hanya mengatur titik mulai
    # baca (akhir file vs awal file), bukan status live.
    store.set_meta(connected=True,
                   source=f"file · {os.path.basename(path)}",
                   live=True)

    # tunggu file ada
    waited = 0
    while not os.path.exists(path):
        if waited % 10 == 0:
            print(f"[SERIAL] menunggu file {path} ...")
        time.sleep(1); waited += 1
        if args.serial_wait and waited > args.serial_wait:
            print("[SERIAL] timeout menunggu file")
            store.set_meta(connected=False)
            return

    last_uptime = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        # Default: baca dari AWAL agar grafik & log sinkron sejak detik 0.
        # --serial-tail: lompat ke akhir (untuk mengikuti proses yang sedang jalan).
        if args.serial_tail:
            fh.seek(0, os.SEEK_END)
        buf = ""
        last_size = 0
        while True:
            chunk = fh.readline()
            if not chunk:
                # File dikosongkan/di-rotate (mis. bridge_wokwi.py --reset)?
                # Kembali ke awal supaya data baru tetap terbaca.
                try:
                    size = os.path.getsize(path)
                except OSError:
                    size = last_size
                if size < last_size:
                    fh.seek(0)
                    buf = ""
                last_size = size
                time.sleep(0.15)
                continue
            last_size = max(last_size, fh.tell())

            # Baris bisa terpotong saat file sedang ditulis: simpan sisa
            # yang belum berakhiran newline dan gabungkan dengan chunk berikut.
            if not chunk.endswith("\n"):
                buf += chunk
                continue
            line = (buf + chunk).strip()
            buf = ""
            if not line:
                continue

            m = CSV_RE.match(line)
            if m:
                g = m.groups()
                dev, up, adc, raw, ppm, status, valve, power, alarm, lock, thr = g[:11]
                r0 = g[11] if len(g) > 11 else None
                s = {"device": dev, "uptime_s": int(up), "adc": int(adc),
                     "adc_raw": int(raw), "ppm": float(ppm), "status": status,
                     "valve": valve, "power": power, "alarm": int(alarm) == 1,
                     "lock": int(lock) == 1, "threshold": int(thr)}
                if r0 is not None:
                    try:
                        s["r0"] = float(r0)
                    except ValueError:
                        pass
                cur_up = int(up)
                # Deteksi sesi baru: uptime mundur jauh -> reset penanda
                if cur_up + 5 < last_uptime:
                    last_uptime = cur_up
                else:
                    last_uptime = cur_up
                store.add_sample(s)
                writer.write("telemetry", s)
                store.set_meta(device=dev, connected=True)
                continue

            if "[EVENT]" in line:
                msg = line.split("[EVENT]", 1)[1].strip()
                lvl = 2 if ("BAHAYA" in msg or "FAULT" in msg
                            or "DIPUTUS" in msg) else \
                      (1 if ("Gas" in msg or "Tren" in msg or "DITOLAK" in msg
                             or "DITUTUP" in msg) else 0)
                e = {"device": store.meta.get("device", "-"),
                     "uptime_s": last_uptime, "msg": msg, "level": lvl}
                store.add_event(e)
                writer.write("event", e)
                continue


# ============================================================
#  MODE 2: MQTT
# ============================================================
def run_mqtt(store, writer, args):
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[ERR] paho-mqtt belum terpasang: pip install paho-mqtt")
        store.set_meta(connected=False, source="mqtt (library hilang)")
        return

    topic_tel = f"safegas/{args.device}/telemetry"
    topic_evt = f"safegas/{args.device}/event"
    topic_cmd = f"safegas/{args.device}/cmd"
    if args.device == "+":
        topic_tel, topic_evt = "safegas/+/telemetry", "safegas/+/event"

    holder = {}

    def on_connect(client, userdata, flags, rc, properties=None):
        code = getattr(rc, "value", rc)
        ok = (code == 0)
        print(f"[MQTT] on_connect rc={rc}")
        store.set_meta(connected=ok, source=f"MQTT {args.mqtt_host}:{args.mqtt_port}",
                       live=True)
        if ok:
            client.subscribe([(topic_tel, 0), (topic_evt, 0)])
            print(f"[MQTT] subscribe {topic_tel}, {topic_evt}")

    def on_disconnect(client, userdata, *a):
        print("[MQTT] terputus")
        store.set_meta(connected=False)

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            print(f"[MQTT] bukan JSON di {msg.topic}: {msg.payload[:120]}")
            return
        if msg.topic.endswith("/telemetry"):
            store.add_sample(payload)
            writer.write("telemetry", payload)
            store.set_meta(device=payload.get("device", "-"))
        elif msg.topic.endswith("/event"):
            store.add_event(payload)
            writer.write("event", payload)

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=f"dashboard-{random.randint(1000, 9999)}")
    except Exception:
        client = mqtt.Client(client_id=f"dashboard-{random.randint(1000, 9999)}")

    if args.mqtt_user:
        client.username_pw_set(args.mqtt_user, args.mqtt_pass or "")
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    if args.mqtt_tls:
        import ssl
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
        client.tls_insecure_set(True)

    while True:
        try:
            print(f"[MQTT] connect ke {args.mqtt_host}:{args.mqtt_port} ...")
            client.connect(args.mqtt_host, args.mqtt_port, 60)
            break
        except Exception as e:
            print(f"[MQTT] gagal: {e} — coba lagi 5 dtk")
            store.set_meta(connected=False)
            time.sleep(5)

    holder["client"] = client
    holder["topic"] = topic_cmd

    def send_cmd(cmd):
        try:
            client.publish(topic_cmd, cmd)
            print(f"[MQTT] cmd terkirim: {cmd}")
            return True
        except Exception as e:
            print(f"[MQTT] gagal kirim cmd: {e}")
            return False

    store.cmd_sink = send_cmd
    store.set_meta(cmd=True)
    client.loop_forever()


# ============================================================
#  MODE 3: SIMULATOR
# ============================================================
class Simulator:
    """Meniru state machine sketch.ino dengan clock logis (detik)."""

    def __init__(self, store, writer, speed=1.0, device="safegas01"):
        self.store, self.writer = store, writer
        self.speed, self.device = speed, device
        self.now = 0.0
        self.phase = 0
        self.phase_t = 0.0
        self.reset_req = False
        self.clear_since = None

        self.warmup = True
        self.gas = False
        self.worsening = False
        self.lock = False
        self.valve = False
        self.power = False
        self.alarm = False
        self.rise = 0
        self.gas_start = 0.0
        self.trend_t = 0.0
        self.trend_ref = 0
        self.adc = 420
        self.raw = 420

    def log(self, msg, up, level=None):
        if level is None:
            level = 2 if ("BAHAYA" in msg) else (1 if ("Gas" in msg or "Tren" in msg or "DITOLAK" in msg) else 0)
        ev = {"device": self.device, "uptime_s": int(up), "msg": msg, "level": level}
        self.store.add_event(ev)
        self.writer.write("event", ev)

    def status(self):
        if self.warmup: return "WARMUP"
        if self.lock:   return "BAHAYA"
        if self.worsening: return "MEMBURUK"
        if self.gas:    return "ADA_GAS"
        return "NORMAL"

    # ---- interlock reset (meniru tryReset() di sketch.ino) ----
    def request_reset(self, cmd="reset"):
        """Dipanggil dari tombol dashboard (POST /api/cmd)."""
        if cmd != "reset":
            return False
        self.reset_req = True
        return True

    def try_reset(self, up):
        if self.warmup:
            self.log("Reset diabaikan (dashboard): masih warm-up", up, 0)
            return
        if not self.lock:
            self.log("Reset diabaikan (dashboard): sistem sedang normal", up, 0)
            return
        if self.adc >= GAS_OFF:
            self.log(f"Reset DITOLAK (dashboard): gas masih tinggi (ADC={int(self.adc)})", up, 1)
            return
        if self.clear_since is None:
            self.clear_since = self.now
        clear_for = self.now - self.clear_since
        if clear_for < 3.0:
            self.log(f"Reset DITOLAK (dashboard): tunggu {int((3.0 - clear_for) * 1000)} ms lagi", up, 1)
            return
        self.lock = False
        self.gas = False
        self.worsening = False
        self.rise = 0
        self.clear_since = None
        # lanjut ke fase pemulihan agar ADC turun bertahap
        self.phase, self.phase_t = 4, 0.0
        self.log("Reset BERHASIL (dashboard): sistem normal kembali", up, 0)

    def step(self, dt=1.0):
        self.now += dt
        now = up = self.now

        if self.reset_req:
            self.reset_req = False
            self.try_reset(up)

        self.phase_t += dt
        if   self.phase == 0 and self.phase_t > 6:  self.phase, self.phase_t = 1, 0
        elif self.phase == 1 and self.phase_t > 10: self.phase, self.phase_t = 2, 0
        elif self.phase == 2 and self.phase_t > 12: self.phase, self.phase_t = 3, 0
        elif self.phase == 3 and self.phase_t > 14: self.phase, self.phase_t = 4, 0
        elif self.phase == 4 and self.phase_t > 8:
            self.phase, self.phase_t = 0, 0
            self.lock = False
            self.log("Reset BERHASIL (simulator): sistem normal kembali", up, 0)

        target = {0: 400, 1: 430, 2: 1650, 3: 2950, 4: 900}[self.phase]
        if self.phase == 2:
            target = 1500 + self.phase_t * 60
        self.adc += (target - self.adc) * 0.45
        self.raw = int(self.adc + random.gauss(0, 18))

        if self.warmup:
            if now >= 5:
                self.warmup = False
                self.log("Warm-up selesai", up, 0)
            self.emit(up)
            return

        ada = (self.adc >= GAS_OFF) if self.gas else (self.adc >= GAS_ON)
        if not ada:
            if self.gas:
                self.log(f"Gas hilang (ADC={int(self.adc)})", up, 0)
                self.gas = False; self.worsening = False; self.rise = 0
        else:
            if not self.gas:
                self.gas = True
                self.gas_start = now; self.trend_t = now
                self.trend_ref = int(self.adc); self.rise = 0
                self.log(f"Gas terdeteksi! ADC={int(self.adc)}", up, 1)

            if self.adc >= GAS_CRITICAL and not self.lock:
                self.lock = True
                self.log(f"BAHAYA terkunci: kadar gas kritis (ADC={int(self.adc)})", up, 2)
            if not self.lock and (now - self.gas_start) * 1000 >= PERSIST_LOCK_MS:
                self.lock = True
                self.log("BAHAYA terkunci: gas bertahan terlalu lama", up, 2)

            if now - self.trend_t >= 1.0:
                delta = int(self.adc) - self.trend_ref
                if delta >= TREND_RISE_MIN:
                    self.rise += 1
                    if not self.worsening:
                        self.worsening = True
                        self.log(f"Tren memburuk: {self.trend_ref} -> {int(self.adc)} (delta {delta:+d})", up, 1)
                    if self.rise >= 2 and not self.lock:
                        self.lock = True
                        self.log("BAHAYA terkunci: tren gas naik terus", up, 2)
                else:
                    self.rise = 0
                    if delta < 0: self.worsening = False
                self.trend_t = now
                self.trend_ref = int(self.adc)

        want_valve = (not self.warmup and not self.lock and not self.gas and not self.worsening)
        if want_valve != self.valve:
            self.valve = want_valve
            self.log("Katup Gas DIBUKA" if self.valve else "Katup Gas DITUTUP", up, 0)
        # Samakan dengan sketch.ino: listrik diputus SEGERA saat gas terdeteksi
        want_power = (not self.warmup and not self.lock and not self.gas
                      and not self.worsening)
        if want_power != self.power:
            self.power = want_power
            self.log("Listrik NYALA" if self.power else "Listrik DIPUTUS TOTAL (Darurat)", up, 0)
        self.alarm = self.lock or self.gas

        self.emit(up)

    def emit(self, up):
        adc = max(self.adc, 1.0)
        vout = max((adc / 4095.0) * 3.3, 0.01)
        rs = 5000.0 * (3.3 - vout) / vout
        ppm = 574.25 * max(rs / 10000.0, 0.01) ** -2.222
        sample = {"device": self.device, "uptime_s": int(up), "adc": int(self.adc),
                  "adc_raw": self.raw, "ppm": round(ppm, 0), "status": self.status(),
                  "valve": "OPEN" if self.valve else "CLOSED",
                  "power": "ON" if self.power else "OFF",
                  "alarm": self.alarm, "lock": self.lock, "threshold": GAS_ON}
        self.store.add_sample(sample)
        self.writer.write("telemetry", sample)

    def run(self):
        dt = 1.0 / max(self.speed, 0.1)
        while True:
            self.step(dt)
            time.sleep(dt)


# ============================================================
#  MODE 4: REPLAY
# ============================================================
def run_replay(store, args):
    path = args.log_file
    if not path or not os.path.exists(path):
        print(f"[ERR] file log tidak ada: {path}")
        store.set_meta(connected=False, source="replay (file tidak ada)")
        return
    store.set_meta(connected=True, source=f"replay {os.path.basename(path)}")
    print(f"[REPLAY] memutar {path} @ {args.speed}x")
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                continue
            kind, data = rec.get("kind"), rec.get("data", {})
            if kind == "telemetry":
                store.add_sample(data)
            elif kind == "event":
                store.add_event(data)
            time.sleep(1.0 / max(args.speed, 0.1))
    print("[REPLAY] selesai")
    store.set_meta(connected=False, source="replay (selesai)")


# ============================================================
#  HTTP + SSE
# ============================================================
class Handler(BaseHTTPRequestHandler):
    store = None
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def handle_one_request(self):
        """Browser menutup koneksi SSE/HTTP itu normal — jangan cetak traceback."""
        try:
            super().handle_one_request()
        except (ConnectionAbortedError, ConnectionResetError, BrokenPipeError):
            self.close_connection = True

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path in ("/", "/index.html"):
            try:
                with open(INDEX_FILE, "rb") as fh:
                    self._send(200, fh.read(), "text/html; charset=utf-8")
            except FileNotFoundError:
                self._send(500, "index.html tidak ditemukan", "text/plain")
            return
        if u.path == "/api/state":
            self._send(200, json.dumps(self.store.snapshot()))
            return
        if u.path == "/api/health":
            self._send(200, json.dumps({"ok": True, "meta": self.store.meta}))
            return
        if u.path == "/stream":
            self.stream(u)
            return
        self._send(404, json.dumps({"error": "not found"}))

    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/api/cmd":
            try:
                n = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(n) or b"{}")
            except Exception:
                body = {}
            cmd = body.get("cmd", "reset")
            sink = self.store.cmd_sink
            if sink is None:
                self._send(503, json.dumps({"ok": False,
                                            "error": "mode ini tidak mendukung perintah"}))
                return
            try:
                ok = bool(sink(cmd))
            except Exception as e:            # jangan sampai request menggantung
                self._send(500, json.dumps({"ok": False, "cmd": cmd, "error": str(e)}))
                return
            self._send(200 if ok else 500, json.dumps({"ok": ok, "cmd": cmd}))
            return
        self._send(404, json.dumps({"error": "not found"}))

    def stream(self, u):
        q = parse_qs(u.query)
        since_s = int(q.get("ss", ["0"])[0])
        since_e = int(q.get("es", ["0"])[0])

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        try:
            self.wfile.write(b"retry: 2000\n\n")
            self.wfile.flush()
            while True:
                new_s, new_e, meta, s_seq, e_seq = self.store.wait_delta(
                    since_s, since_e, timeout=15.0)
                payload = {"samples": new_s, "events": new_e, "meta": meta,
                           "s_seq": s_seq, "e_seq": e_seq}
                self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode("utf-8"))
                self.wfile.flush()
                since_s, since_e = s_seq, e_seq
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            # Browser menutup tab / koneksi: normal, bukan error.
            return
        except Exception as e:              # jangan cetak traceback ke konsol
            print(f"[SSE] stream berhenti: {e}")
            return


def run_http(store, port):
    Handler.store = store
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.daemon_threads = True
    print(f"[HTTP] dashboard siap di  http://localhost:{port}")
    srv.serve_forever()


# ============================================================
#  MAIN
# ============================================================
def main():
    ap = argparse.ArgumentParser(description="SafeGas Realtime Dashboard")
    ap.add_argument("--mode", choices=["serial", "mqtt", "sim", "replay"], default="sim")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--device", default="safegas01",
                    help="device id; pakai + untuk semua device")
    ap.add_argument("--serial-file", default="serial.log",
                    help="mode serial: file berisi output Serial Monitor")
    ap.add_argument("--serial-wait", type=int, default=60,
                    help="mode serial: detik menunggu file muncul (0=selamanya)")
    ap.add_argument("--serial-tail", action="store_true",
                    help="mode serial: hanya baca baris BARU (ikuti proses berjalan)")
    ap.add_argument("--mqtt-host", default="broker.hivemq.com")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--mqtt-user", default="")
    ap.add_argument("--mqtt-pass", default="")
    ap.add_argument("--mqtt-tls", action="store_true")
    ap.add_argument("--log-file", default="")
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--no-record", action="store_true")
    args = ap.parse_args()

    store = Store()
    store.set_meta(mode=args.mode, device=args.device)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = None if args.no_record else os.path.join(LOG_DIR, f"safegas_{args.mode}_{stamp}.jsonl")
    writer = LogWriter(log_path)

    print("=" * 58)
    print("  SAFEGAS REALTIME DASHBOARD")
    print("=" * 58)
    print(f"  Mode      : {args.mode}")
    if args.mode == "mqtt":
        print(f"  Broker    : {args.mqtt_host}:{args.mqtt_port} (TLS={'ya' if args.mqtt_tls else 'tidak'})")
        print(f"  Topic     : safegas/{args.device}/telemetry")
    if args.mode == "serial":
        print(f"  Serial    : {args.serial_file}")
    print(f"  Dashboard : http://localhost:{args.http_port}")
    print("=" * 58)

    if args.mode == "mqtt":
        threading.Thread(target=run_mqtt, args=(store, writer, args), daemon=True).start()
    elif args.mode == "sim":
        store.set_meta(connected=True, source="simulator internal", live=True)
        simulator = Simulator(store, writer, args.speed, args.device)
        # Tombol reset di dashboard juga berfungsi di mode simulator
        # (melewati interlock yang sama dengan firmware asli).
        store.cmd_sink = simulator.request_reset
        store.set_meta(cmd=True)
        threading.Thread(target=simulator.run, daemon=True).start()
    elif args.mode == "serial":
        threading.Thread(target=run_serial, args=(store, writer, args), daemon=True).start()
    else:
        threading.Thread(target=run_replay, args=(store, args), daemon=True).start()

    def shutdown(*_):
        print("\n[EXIT] menutup...")
        writer.close()
        os._exit(0)

    signal.signal(signal.SIGINT, shutdown)
    try:
        run_http(store, args.http_port)
    except KeyboardInterrupt:
        shutdown()


if __name__ == "__main__":
    main()
