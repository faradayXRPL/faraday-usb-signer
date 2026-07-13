#!/usr/bin/env python3
"""Collect cold-boot entropy diagnostic samples over USB serial.

Build a non-production firmware with cfg::ENTROPY_DIAGNOSTICS=true, power-cycle
the ESP32-S3 before each sample, then run this script to append one sample per
boot. Never enable the diagnostic command in production firmware.
"""

import argparse
import csv
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
    raise


def read_sample(port: str, baud: int, timeout: float) -> str:
    with serial.Serial(port, baudrate=baud, timeout=timeout) as ser:
        time.sleep(0.4)
        ser.reset_input_buffer()
        ser.write(b"ENTROPY_DIAG\n")
        ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = ser.readline().decode("ascii", errors="replace").strip()
            if line.startswith("ENTROPY_DIAG "):
                sample = line.split(" ", 1)[1]
                if len(sample) == 64 and all(c in "0123456789ABCDEF" for c in sample):
                    return sample
                raise RuntimeError(f"malformed sample: {line}")
            if line.startswith("ERROR "):
                raise RuntimeError(line)
    raise TimeoutError("no ENTROPY_DIAG response")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="Serial port, e.g. COM7 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--out", default="entropy-cold-boot.csv")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    out = Path(args.out)
    seen = set()
    if out.exists():
        with out.open(newline="") as f:
            for row in csv.DictReader(f):
                seen.add(row["sample"])

    first_write = not out.exists()
    with out.open("a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["index", "unix_time", "sample", "duplicate"])
        if first_write:
            writer.writeheader()
        for i in range(args.count):
            input(f"Power-cycle the device, wait for READY, then press Enter [{i + 1}/{args.count}]")
            sample = read_sample(args.port, args.baud, args.timeout)
            duplicate = sample in seen
            seen.add(sample)
            writer.writerow(
                {
                    "index": i,
                    "unix_time": f"{time.time():.3f}",
                    "sample": sample,
                    "duplicate": int(duplicate),
                }
            )
            f.flush()
            status = "DUPLICATE" if duplicate else "ok"
            print(f"{i + 1}: {sample} {status}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
