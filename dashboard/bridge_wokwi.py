#!/usr/bin/env python3
"""
Jembatan Wokwi -> Dashboard (mode Serial)
==========================================
Menyalin output Serial Monitor Wokwi ke file log yang dibaca dashboard,
secara realtime. Tidak butuh MQTT, akun, atau API apa pun.

Dua cara pakai:

  A. Copy-paste dari Wokwi (paling mudah, tanpa alat tambahan)
     -------------------------------------------------------
     1. Jalankan skrip ini:   python dashboard/bridge_wokwi.py --out serial.log
     2. Di Wokwi, klik Serial Monitor -> Ctrl+A -> Ctrl+C setiap beberapa detik
        lalu Ctrl+V di terminal skrip ini, tekan Enter.
     3. Skrip menulis baris CSV ke serial.log; dashboard langsung menampilkannya.

  B. Tempel sekaligus (batch)
     ------------------------
     python dashboard/bridge_wokwi.py --out serial.log --paste
     Lalu tempel seluruh isi Serial Monitor, akhiri dengan baris berisi END.

  C. Dari file yang sudah ada
     ------------------------
     python dashboard/bridge_wokwi.py --from-file hasil_copy.txt --out serial.log

Dashboard membaca file ini secara realtime (mode --mode serial).
"""

import argparse
import os
import re
import sys
import time

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

CSV_RE = re.compile(r"^CSV,[^,]+,\d+,")
EVENT_RE = re.compile(r"\[EVENT\]")


def is_useful(line: str) -> bool:
    """Hanya teruskan baris CSV dan [EVENT] — buang noise log lain."""
    s = line.strip()
    if not s:
        return False
    if CSV_RE.match(s):
        return True
    if EVENT_RE.search(s):
        return True
    return False


def emit(lines, out, quiet=False):
    n = 0
    for raw in lines:
        # Wokwi kadang menambahkan prefix waktu / nomor baris
        line = raw
        m = re.search(r"(CSV,[^,]+,\d+,.*)$", line)
        if m:
            line = m.group(1)
        if not is_useful(line):
            continue
        out.write(line.rstrip("\r\n") + "\n")
        out.flush()
        n += 1
        if not quiet:
            print(f"  -> {line.rstrip()[:110]}")
    return n


def main():
    ap = argparse.ArgumentParser(description="Jembatan Serial Wokwi -> dashboard")
    ap.add_argument("--out", default="serial.log", help="file tujuan (dibaca dashboard)")
    ap.add_argument("--from-file", help="impor dari file hasil copy Serial Monitor")
    ap.add_argument("--paste", action="store_true",
                    help="mode tempel: baca stdin sampai baris 'END'")
    ap.add_argument("--reset", action="store_true", help="kosongkan file tujuan dulu")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out_path = os.path.abspath(args.out)
    # Path relatif diartikan relatif terhadap folder dashboard/ (sama seperti
    # safegas_dashboard.py), supaya keduanya menunjuk file yang sama walau
    # dijalankan dari folder berbeda.
    if not os.path.isabs(args.out):
        out_path = os.path.abspath(os.path.join(BASE_DIR, args.out))
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    mode = "a"
    if args.reset and os.path.exists(out_path):
        mode = "w"

    with open(out_path, mode, encoding="utf-8") as out:
        # --- C: dari file ---
        if args.from_file:
            if not os.path.exists(args.from_file):
                print(f"[ERR] file tidak ada: {args.from_file}")
                return 2
            with open(args.from_file, encoding="utf-8", errors="replace") as fh:
                n = emit(fh, out, args.quiet)
            print(f"\n[SELESAI] {n} baris ditulis ke {out_path}")
            return 0

        # --- B: tempel sekaligus ---
        if args.paste:
            print("Tempel isi Serial Monitor, lalu ketik END di baris baru:")
            lines = []
            for line in sys.stdin:
                if line.strip() == "END":
                    break
                lines.append(line)
            n = emit(lines, out, args.quiet)
            print(f"\n[SELESAI] {n} baris ditulis ke {out_path}")
            return 0

        # --- A: interaktif ---
        print("=" * 62)
        print("  JEMBATAN WOKWI -> DASHBOARD")
        print("=" * 62)
        print(f"  File tujuan : {out_path}")
        print("  Cara pakai  :")
        print("    1. Di Wokwi, buka Serial Monitor")
        print("    2. Ctrl+A lalu Ctrl+C untuk menyalin")
        print("    3. Klik di sini, Ctrl+V, tekan Enter")
        print("    4. Ulangi tiap beberapa detik")
        print("  Ketik 'q' lalu Enter untuk keluar.")
        print("=" * 62)

        total = 0
        while True:
            try:
                line = input()
            except (EOFError, KeyboardInterrupt):
                break
            if line.strip().lower() in ("q", "quit", "exit"):
                break
            # bisa berisi banyak baris sekaligus hasil paste
            n = emit(line.splitlines() or [line], out, args.quiet)
            total += n
            if n:
                print(f"  [{total} baris total]")

    print(f"\n[SELESAI] {total} baris -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
