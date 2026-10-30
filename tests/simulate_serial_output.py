#!/usr/bin/env python3
"""
ALAT UJI — BUKAN bagian dari submission.

Meniru format output Serial Monitor Wokwi agar pipeline dashboard dapat
diuji tanpa akses ke Wokwi. Skrip ini TIDAK dipakai saat demo/pengumpulan;
yang dijalankan di Wokwi adalah sketch.ino asli.

Alur asli (dipakai saat demo):
    Wokwi asli -> Serial Monitor -> bridge_wokwi.py -> serial.log -> dashboard

Alur uji (hanya untuk verifikasi otomatis):
    simulate_serial_output.py -> serial.log -> dashboard

Pemakaian:
    python tests/simulate_serial_output.py --out serial.log --speed 2 --duration 60
"""

import argparse
import os
import random
import time

GAS_ON, GAS_OFF, GAS_CRITICAL = 1500, 1300, 2800


class Sim:
    def __init__(self):
        self.now = 0.0
        self.phase = 0
        self.phase_t = 0.0
        self.warmup = True
        self.gas = False
        self.worsening = False
        self.lock = False
        self.valve = False
        self.power = False
        self.adc = 420
        self.rise = 0
        self.gas_start = 0.0
        self.trend_t = 0.0
        self.trend_ref = 0
        self.r0 = 9820

    def status(self):
        if self.warmup: return "WARMUP"
        if self.lock: return "BAHAYA"
        if self.worsening: return "MEMBURUK"
        if self.gas: return "ADA_GAS"
        return "NORMAL"

    def step(self, dt):
        self.now += dt
        now = self.now
        self.phase_t += dt
        if   self.phase == 0 and self.phase_t > 6:  self.phase, self.phase_t = 1, 0
        elif self.phase == 1 and self.phase_t > 8:  self.phase, self.phase_t = 2, 0
        elif self.phase == 2 and self.phase_t > 10: self.phase, self.phase_t = 3, 0
        elif self.phase == 3 and self.phase_t > 12: self.phase, self.phase_t = 4, 0
        elif self.phase == 4 and self.phase_t > 8:
            self.phase, self.phase_t = 0, 0
            self.lock = False
            yield "[EVENT] Reset BERHASIL (simulator): sistem normal kembali"
        # Fase 2 = kebocoran membesar (naik), fase 3 = puncak, fase 4 = pulih.
        # Gas HANYA turun setelah puncak tercapai, bukan begitu lock aktif.
        target = {0: 400, 1: 430, 2: 2950, 3: 2950, 4: 400}[self.phase]
        if self.phase == 2:
            target = min(2950, 1500 + self.phase_t * 150)
        if self.phase == 4:
            target = max(400, self.adc - 120)
        self.adc += (target - self.adc) * 0.42
        raw = int(self.adc + random.gauss(0, 16))

        if self.warmup:
            if now >= 5:
                self.warmup = False
                yield "[EVENT] Warm-up selesai"
            yield self.csv(raw)
            return

        ada = (self.adc >= GAS_OFF) if self.gas else (self.adc >= GAS_ON)
        if not ada:
            if self.gas:
                self.gas = False; self.worsening = False; self.rise = 0
                yield f"[EVENT] Gas hilang (ADC={int(self.adc)})"
        else:
            if not self.gas:
                self.gas = True
                self.gas_start = now; self.trend_t = now
                self.trend_ref = int(self.adc); self.rise = 0
                yield f"[EVENT] Gas terdeteksi! ADC={int(self.adc)}"
            if self.adc >= GAS_CRITICAL and not self.lock:
                self.lock = True
                yield f"[EVENT] BAHAYA terkunci: kadar gas kritis (ADC={int(self.adc)})"
            if now - self.trend_t >= 1.0:
                d = int(self.adc) - self.trend_ref
                if d >= 50:
                    self.rise += 1
                    if not self.worsening:
                        self.worsening = True
                        yield (f"[EVENT] Tren memburuk: {self.trend_ref} -> "
                               f"{int(self.adc)} (delta {d:+d})")
                    if self.rise >= 2 and not self.lock:
                        self.lock = True
                        yield "[EVENT] BAHAYA terkunci: tren gas naik terus"
                else:
                    self.rise = 0
                    if d < 0: self.worsening = False
                self.trend_t = now
                self.trend_ref = int(self.adc)

        want_valve = (not self.warmup and not self.lock and not self.gas and not self.worsening)
        if want_valve != self.valve:
            self.valve = want_valve
            yield ("[EVENT] Katup Gas DIBUKA" if self.valve else "[EVENT] Katup Gas DITUTUP")
        want_power = (not self.warmup and not self.lock and not self.gas and not self.worsening)
        if want_power != self.power:
            self.power = want_power
            yield ("[EVENT] Listrik NYALA" if self.power
                   else "[EVENT] Listrik DIPUTUS TOTAL (Darurat)")

        yield self.csv(raw)

    def csv(self, raw):
        vout = max((self.adc / 4095.0) * 3.3, 0.01)
        rs = 5000.0 * (3.3 - vout) / vout
        ppm = 574.25 * max(rs / 10000.0, 0.01) ** -2.222
        return (f"CSV,safegas01,{int(self.now)},{int(self.adc)},{raw},{ppm:.0f},"
                f"{self.status()},{'OPEN' if self.valve else 'CLOSED'},"
                f"{'ON' if self.power else 'OFF'},"
                f"{1 if (self.lock or self.gas) else 0},{1 if self.lock else 0},"
                f"{GAS_ON},{self.r0:.0f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="serial.log")
    ap.add_argument("--speed", type=float, default=2.0, help="sampel per detik")
    ap.add_argument("--duration", type=float, default=0, help="0 = selamanya")
    args = ap.parse_args()

    path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    sim = Sim()
    dt = 1.0 / args.speed
    t0 = time.time()
    print(f"[FAKE-WOKWI] menulis ke {path} @ {args.speed} sampel/s")
    with open(path, "a", encoding="utf-8") as fh:
        while True:
            for line in sim.step(dt):
                fh.write(line + "\n")
            fh.flush()
            time.sleep(dt)
            if args.duration and (time.time() - t0) >= args.duration:
                break
    print("[FAKE-WOKWI] selesai")


if __name__ == "__main__":
    main()
