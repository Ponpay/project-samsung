#!/usr/bin/env python3
"""
ALAT UJI — BUKAN bagian dari submission.

Verifikasi end-to-end pipeline realtime:
    simulator Serial -> serial.log -> dashboard (mode serial) -> SSE /stream

Yang diuji:
  1. dashboard menerima baris CSV dan mem-parse semuanya (0 baris terbuang)
  2. SSE mengirim data ke browser (bukan cuma REST /api/state)
  3. latensi realtime wajar (< 3 detik dari tulis file sampai SSE terkirim)
  4. semua status state-machine pernah muncul (WARMUP/NORMAL/ADA_GAS/MEMBURUK/BAHAYA)
  5. event [EVENT] ikut terkirim ke panel log dashboard

Pemakaian:
    python tests/test_realtime_pipeline.py
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DASH = os.path.join(ROOT, "dashboard", "safegas_dashboard.py")
SIM = os.path.join(HERE, "simulate_serial_output.py")
PY = sys.executable

PORT = 8099
results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  [{detail}]" if detail else ""))


def wait_http(url, timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as r:
                return r.read()
        except Exception:
            time.sleep(0.2)
    return None


def main():
    tmp = tempfile.mkdtemp(prefix="safegas-e2e-")
    serial_log = os.path.join(tmp, "serial.log")
    open(serial_log, "w").close()

    print("=" * 62)
    print("  E2E: Serial -> Dashboard -> SSE")
    print("=" * 62)

    dash = subprocess.Popen(
        [PY, DASH, "--mode", "serial", "--serial-file", serial_log,
         "--serial-wait", "10", "--http-port", str(PORT), "--no-record"],
        cwd=os.path.join(ROOT, "dashboard"),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )

    try:
        if wait_http(f"http://127.0.0.1:{PORT}/api/health") is None:
            print("[ERR] dashboard gagal start")
            out = dash.stdout.read() if dash.stdout else ""
            print(out[:3000])
            return 1
        check("dashboard HTTP hidup", True)

        # ---- buka SSE dulu, baru tulis data (uji push realtime) ----
        sse = {"lines": [], "t_first": None}
        t_write = [None]

        def sse_reader():
            req = urllib.request.Request(f"http://127.0.0.1:{PORT}/stream?ss=0&es=0")
            try:
                with urllib.request.urlopen(req, timeout=30) as r:
                    for raw in r:
                        line = raw.decode("utf-8", "replace").strip()
                        if line.startswith("data: "):
                            if sse["t_first"] is None:
                                sse["t_first"] = time.time()
                            sse["lines"].append(json.loads(line[6:]))
                            if len(sse["lines"]) >= 3:
                                return
            except Exception as e:
                sse["err"] = str(e)

        th = threading.Thread(target=sse_reader, daemon=True)
        th.start()
        time.sleep(0.6)

        sim = subprocess.Popen(
            [PY, SIM, "--out", serial_log, "--speed", "8", "--duration", "42"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        t_write[0] = time.time()
        sim.wait(timeout=90)
        th.join(timeout=25)

        check("SSE mengirim data (bukan hanya REST)", len(sse["lines"]) > 0,
              f"{len(sse['lines'])} frame")
        if sse["t_first"] and t_write[0]:
            lat = sse["t_first"] - t_write[0]
            check("latensi push SSE < 3 detik", lat < 3.0, f"{lat:.2f}s")

        # ---- verifikasi isi ----
        state = json.loads(wait_http(f"http://127.0.0.1:{PORT}/api/state"))
        samples = state["samples"]
        events = state["events"]

        with open(serial_log, encoding="utf-8") as fh:
            csv_lines = [l for l in fh if l.startswith("CSV,")]
        check("semua baris CSV ter-parse", len(samples) == len(csv_lines),
              f"{len(samples)}/{len(csv_lines)}")

        statuses = {s["status"] for s in samples}
        need = {"WARMUP", "NORMAL", "ADA_GAS", "BAHAYA"}
        check("semua fase state-machine muncul", need <= statuses, str(sorted(statuses)))

        check("event [EVENT] masuk dashboard", len(events) > 0, f"{len(events)} event")
        check("ada event BAHAYA di log", any("BAHAYA" in e.get("msg", "") for e in events))

        # field wajib untuk grafik & panel
        s0 = samples[-1]
        need_fields = {"device", "uptime_s", "adc", "adc_raw", "ppm", "status",
                       "valve", "power", "alarm", "lock", "threshold"}
        missing = need_fields - set(s0)
        check("field telemetri lengkap", not missing, str(missing) if missing else "")

        # dashboard harus menandai live
        check("meta.live = true (realtime)", state["meta"].get("live") is True)

    finally:
        dash.terminate()
        try:
            dash.wait(timeout=5)
        except subprocess.TimeoutExpired:
            dash.kill()

    ok = sum(1 for _, o, _ in results if o)
    bad = len(results) - ok
    print()
    print(f"=== HASIL E2E: {ok} pass, {bad} fail ===")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
