#!/usr/bin/env python3
"""
ALAT UJI — BUKAN bagian dari submission.

Verifikasi mode MQTT dashboard memakai broker publik (broker.hivemq.com:1883).
Tidak butuh akun HiveMQ Cloud; yang diuji adalah jalur kode yang sama
(subscribe telemetry + event, push ke browser lewat SSE).

Butuh: pip install paho-mqtt

Pemakaian:
    python tests/test_mqtt_pipeline.py
"""

import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DASH = os.path.join(ROOT, "dashboard", "safegas_dashboard.py")
PY = sys.executable

PORT = 8098
HOST, BROKER_PORT = "broker.hivemq.com", 1883
DEVICE = "safegas01"
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail else ""))


def wait_http(url, timeout=25):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                return r.read()
        except Exception:
            time.sleep(0.2)
    return None


def main():
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[SKIP] paho-mqtt belum terpasang (pip install paho-mqtt)")
        return 0

    print("=" * 62)
    print("  E2E: MQTT broker -> Dashboard -> SSE")
    print("=" * 62)

    dash = subprocess.Popen(
        [PY, DASH, "--mode", "mqtt", "--mqtt-host", HOST, "--mqtt-port", str(BROKER_PORT),
         "--device", DEVICE, "--http-port", str(PORT), "--no-record"],
        cwd=os.path.join(ROOT, "dashboard"),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )

    try:
        if wait_http(f"http://127.0.0.1:{PORT}/api/health") is None:
            print("[ERR] dashboard gagal start")
            return 1
        check("dashboard HTTP hidup", True)

        # ---- SSE dibuka sebelum publish ----
        sse = {"lines": [], "data": 0, "t_first": None}

        def reader():
            try:
                with urllib.request.urlopen(
                        f"http://127.0.0.1:{PORT}/stream?ss=0&es=0", timeout=40) as r:
                    for raw in r:
                        line = raw.decode("utf-8", "replace").strip()
                        if line.startswith("data: "):
                            frame = json.loads(line[6:])
                            sse["lines"].append(frame)
                            # frame pertama bisa hanya berisi meta (belum ada data)
                            if frame.get("samples") or frame.get("events"):
                                sse["data"] += 1
                                if sse["t_first"] is None:
                                    sse["t_first"] = time.time()
                                return
            except Exception as e:
                sse["err"] = str(e)

        th = threading.Thread(target=reader, daemon=True)
        th.start()

        # ---- publisher (mensimulasikan ESP32) ----
        # Broker publik tidak menjamin urutan: dashboard bisa saja belum selesai
        # subscribe saat publish pertama. Karena itu pesan dikirim berulang
        # sampai dashboard benar-benar menerima (atau batas waktu habis).
        pub = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                          client_id=f"test-pub-{int(time.time())}")
        pub.connect(HOST, BROKER_PORT, 30)
        pub.loop_start()
        time.sleep(3.0)   # beri waktu dashboard subscribe

        t_pub = time.time()
        i = 0
        while time.time() - t_pub < 25 and sse["data"] == 0:
            tel = {"device": DEVICE, "uptime_s": 100 + i, "adc": 1750, "adc_raw": 1780,
                   "ppm": 3200, "r0": 9820, "status": "ADA_GAS", "valve": "CLOSED",
                   "power": "OFF", "alarm": True, "lock": False, "threshold": 1500,
                   "rssi": -60}
            pub.publish(f"safegas/{DEVICE}/telemetry", json.dumps(tel))
            pub.publish(f"safegas/{DEVICE}/event",
                        json.dumps({"device": DEVICE, "uptime_s": 100 + i,
                                    "level": 2, "msg": "BAHAYA uji MQTT"}))
            i += 1
            time.sleep(0.6)
        th.join(timeout=20)
        pub.loop_stop()
        pub.disconnect()

        # Broker publik kadang lambat; tunggu sampai dashboard benar-benar
        # menyimpan data (maks 15 detik) sebelum menilai.
        state = {}
        t0 = time.time()
        while time.time() - t0 < 15:
            state = json.loads(wait_http(f"http://127.0.0.1:{PORT}/api/state") or b"{}")
            if state.get("samples") and state.get("events"):
                break
            time.sleep(1.0)
        check("SSE menerima data via MQTT", len(sse["lines"]) > 0,
              f"{len(sse['lines'])} frame")
        if sse["t_first"]:
            check("latensi MQTT -> SSE < 8 detik", (sse["t_first"] - t_pub) < 8.0,
                  f"{sse['t_first'] - t_pub:.2f}s")
        check("telemetri tersimpan di store", len(state.get("samples", [])) > 0,
              f"{len(state.get('samples', []))} sampel")
        check("event tersimpan di store", len(state.get("events", [])) > 0,
              f"{len(state.get('events', []))} event")
        check("meta.connected = true", state.get("meta", {}).get("connected") is True)
        check("device id terdeteksi", state.get("meta", {}).get("device") == DEVICE)
        if state.get("samples"):
            s = state["samples"][-1]
            check("payload MQTT utuh", s.get("status") == "ADA_GAS"
                  and s.get("valve") == "CLOSED", str(s.get("status")))

    finally:
        dash.terminate()
        try:
            dash.wait(timeout=5)
        except subprocess.TimeoutExpired:
            dash.kill()

    ok = sum(1 for _, o, _ in results if o)
    bad = len(results) - ok
    print()
    print(f"=== HASIL MQTT: {ok} pass, {bad} fail ===")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
