#!/usr/bin/env python3
"""
Jalankan test logika safety SafeGas di komputer (tanpa ESP32).

Cara kerja:
  1. Ambil sketch.ino ASLI (bukan salinan manual)
  2. Buang semua #include hardware, ganti dengan mock Arduino
  3. Kompilasi dengan g++ host
  4. Jalankan test_main.cpp

Butuh: g++ (MinGW/MSVC/clang). Di Windows bisa pakai winlibs portable.

Pemakaian:
    python tests/run_tests.py
    python tests/run_tests.py --gxx "C:/path/to/g++.exe"
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKETCH = os.path.join(ROOT, "sketch.ino")
MOCK = os.path.join(HERE, "mock", "mock_arduino.h")
TEST = os.path.join(HERE, "test_main.cpp")


def find_gxx(explicit=None):
    if explicit:
        return explicit
    cands = []
    for name in ("g++", "g++.exe", "clang++", "clang++.exe"):
        p = shutil.which(name)
        if p:
            cands.append(p)
    # winlibs / mingw portable di lokasi umum
    for pat in (
        r"C:/Users/*/AppData/Local/Temp/mingw64/bin/g++.exe",
        r"C:/mingw64/bin/g++.exe",
        r"C:/msys64/mingw64/bin/g++.exe",
        r"C:/laragon/bin/mingw64/bin/g++.exe",
        r"C:/Program Files/LLVM/bin/clang++.exe",
    ):
        cands.extend(glob.glob(pat))
    # xtensa toolchain (bisa compile, tapi binary tidak jalan di host)
    return cands[0] if cands else None


def strip_includes(src):
    src = re.sub(r'#include\s*<[^>]+>\s*\n', '', src)
    src = re.sub(r'#include\s*"[^"]+"\s*\n', '', src)
    return src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gxx", default=None, help="path ke g++ host")
    ap.add_argument("--keep", action="store_true", help="jangan hapus folder build")
    args = ap.parse_args()

    if not os.path.exists(SKETCH):
        print(f"[ERR] sketch.ino tidak ditemukan di {SKETCH}")
        return 2

    gxx = find_gxx(args.gxx)
    if not gxx:
        print("[ERR] g++ tidak ditemukan. Pasang MinGW/MSVC/clang, atau pakai --gxx.")
        return 2
    print(f"[INFO] compiler: {gxx}")

    build = tempfile.mkdtemp(prefix="safegas-test-")
    try:
        # siapkan sketch tanpa include hardware
        src = open(SKETCH, encoding="utf-8", errors="replace").read()
        stripped = strip_includes(src)
        with open(os.path.join(build, "sketch_for_test.ino"), "w", encoding="utf-8") as fh:
            fh.write(stripped)

        shutil.copy(MOCK, os.path.join(build, "mock_arduino.h"))
        shutil.copy(TEST, os.path.join(build, "test_main.cpp"))

        exe = os.path.join(build, "test_bin.exe" if os.name == "nt" else "test_bin")
        cmd = [gxx, "-std=gnu++17", "-I", build, "-o", exe,
               os.path.join(build, "test_main.cpp")]
        print(f"[INFO] kompilasi: {' '.join(cmd)}")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("[ERR] kompilasi gagal:")
            print(r.stderr[:4000])
            return 1

        print("[INFO] menjalankan test...\n")
        r = subprocess.run([exe], capture_output=True, text=True)
        print(r.stdout)
        if r.stderr.strip():
            print("[stderr]", r.stderr[:2000])
        return r.returncode
    finally:
        if args.keep:
            print(f"[INFO] build dipertahankan di {build}")
        else:
            shutil.rmtree(build, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
