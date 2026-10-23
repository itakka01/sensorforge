#!/usr/bin/env python3
"""Check a SensorForge build against the real partition layout.

The Arduino IDE checks the firmware size against the *menu* partition scheme
(default_8MB: 3,342,336 bytes), while the sketch-folder partitions.csv that is
actually flashed has 3,932,160-byte OTA slots. This script checks the image
against partitions.csv and verifies the generated partitions.bin matches it.

Usage: tools/check_firmware_size.py <build-path> [<build-path> ...]
Exit 1 if an image does not fit or the flashed table differs from the CSV.
Read-only; it never writes, copies or flashes anything.
"""
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARN_HEADROOM = 0.10   # warn below 10 % free in the OTA slot


def parse_int(text):
    text = text.strip()
    return int(text, 0) if text else None


def read_csv(path):
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            rows.append((cols[0], cols[1], cols[2], parse_int(cols[3]), parse_int(cols[4])))
    return rows


def read_bin(path):
    data = open(path, "rb").read()
    rows = []
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            break
        offset, size = struct.unpack("<II", entry[4:12])
        rows.append((entry[12:28].rstrip(b"\0").decode(), offset, size))
    return rows


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    csv_rows = read_csv(os.path.join(REPO, "partitions.csv"))
    apps = [r for r in csv_rows if r[1] == "app"]
    slot = min(r[4] for r in apps)
    expected = [(r[0], r[3], r[4]) for r in csv_rows]
    failed = False
    print("partitions.csv: %d app slots of %d bytes" % (len(apps), slot))
    for build in argv[1:]:
        name = os.path.basename(os.path.normpath(build))
        image = os.path.join(build, "sensorforge.ino.bin")
        table = os.path.join(build, "sensorforge.ino.partitions.bin")
        if not os.path.exists(image):
            print("%s: no sensorforge.ino.bin" % name)
            failed = True
            continue
        size = os.path.getsize(image)
        free = slot - size
        status = "OK"
        if free < 0:
            status = "DOES NOT FIT"
            failed = True
        elif free < slot * WARN_HEADROOM:
            status = "WARNING: less than %d %% free" % int(WARN_HEADROOM * 100)
        print("%s: image %d bytes = %.1f %% of slot, %d bytes free -> %s"
              % (name, size, 100.0 * size / slot, free, status))
        if os.path.exists(table):
            if read_bin(table) != expected:
                print("%s: generated partitions.bin differs from partitions.csv!" % name)
                failed = True
            else:
                print("%s: generated partition table matches partitions.csv" % name)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
