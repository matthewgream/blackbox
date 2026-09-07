#!/usr/bin/env python3

# ------------------------------------------------------------------------------------------------
# blackbox_esp_flash.py - decode a raw dump of an ESP_FLASH blackbox partition into its CSV records.
#
# The device-side backend (BLACKBOX_PERSIST_ESP_FLASH, see include/blackbox.h) stores records as a
# CIRCULAR log of sectors, so a raw partition image is not readable text: the records are framed, the
# sectors are in ring order rather than chronological order, and unwritten space is erased 0xFF. This
# turns such an image back into the original newline-separated CSV lines, oldest first.
#
# ON-FLASH FORMAT (the authoritative description is in DESIGN.md; keep them in step)
#
#   partition = N x 4096-byte sectors, used as a ring
#   sector    = [ magic u32 | seq u32 ] then records packed forward, no record spanning a sector
#   magic     = 0x42584F42 ('BXOB' little-endian); a sector without it was never written
#   seq       = monotonic lap counter, so sorting sectors by seq gives oldest -> newest regardless
#               of physical position (this is what makes the ring readable after it wraps)
#   record    = [ len u16 ][ payload ]   payload = one CSV line, WITHOUT its trailing newline
#   sector end= len == 0xFFFF (erased cell) or len == 0, or a length that would overrun the sector
#
#   All integers little-endian (the esp32 writes them with memcpy).
#
# USAGE
#   Pull the partition with esptool (no ESP-IDF toolchain needed) and decode:
#     python -m esptool --port /dev/ttyACM0 read-flash 0x320000 0xe0000 diag.bin
#     ./blackbox_esp_flash.py diag.bin > diag.csv
#   Then, to get JSON, feed the CSV to the csv2json tool with the project's record header:
#     ./blackbox_esp_flash.py diag.bin | node ../js/csv2json.js --definitions=records.h
#
#   As a module:
#     from blackbox_esp_flash import decode_image, decode_file
#     for line in decode_image(open('diag.bin','rb').read()): ...
#
# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# ------------------------------------------------------------------------------------------------

import argparse
import struct
import sys

SECTOR_SIZE = 4096
SECTOR_MAGIC = 0x42584F42  # 'BXOB'
SECTOR_HDR = 8  # magic(4) + seq(4)
LEN_ERASED = 0xFFFF


def decode_sector(data, off, sector_size=SECTOR_SIZE):
    """Yield the payloads held in one sector. `off` is the sector's start in `data`."""
    end = off + sector_size
    pos = off + SECTOR_HDR
    while pos + 2 <= end:
        (length,) = struct.unpack_from("<H", data, pos)
        if length == LEN_ERASED or length == 0 or pos + 2 + length > end:
            break  # erased cell / terminator / would overrun: nothing more in this sector
        yield data[pos + 2 : pos + 2 + length]
        pos += 2 + length


def scan_sectors(data, sector_size=SECTOR_SIZE):
    """Return [(seq, offset)] for the written sectors, ordered oldest -> newest."""
    found = []
    for off in range(0, (len(data) // sector_size) * sector_size, sector_size):
        if off + SECTOR_HDR > len(data):
            break
        magic, seq = struct.unpack_from("<II", data, off)
        if magic == SECTOR_MAGIC:
            found.append((seq, off))
    found.sort()  # by seq: the lap counter restores chronological order across the ring wrap
    return found


def decode_image(data, sector_size=SECTOR_SIZE, errors="replace"):
    """Yield the CSV lines held in a raw partition image, oldest record first."""
    for _seq, off in scan_sectors(data, sector_size):
        for payload in decode_sector(data, off, sector_size):
            yield payload.decode("utf-8", errors)


def decode_file(path, sector_size=SECTOR_SIZE):
    with open(path, "rb") as f:
        return list(decode_image(f.read(), sector_size))


def main():
    ap = argparse.ArgumentParser(
        description="Decode a raw ESP_FLASH blackbox partition image into its CSV records (oldest first).",
        epilog="e.g.  python -m esptool -p /dev/ttyACM0 read-flash 0x320000 0xe0000 diag.bin && %(prog)s diag.bin",
    )
    ap.add_argument("image", help="raw partition dump (from esptool read-flash / parttool read_partition)")
    ap.add_argument("--sector-size", type=int, default=SECTOR_SIZE, help=f"sector size (default {SECTOR_SIZE})")
    ap.add_argument("--stats", action="store_true", help="report sector/record counts on stderr")
    args = ap.parse_args()

    try:
        with open(args.image, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    sectors = scan_sectors(data, args.sector_size)
    count = 0
    for line in decode_image(data, args.sector_size):
        print(line)
        count += 1
    if args.stats:
        total = len(data) // args.sector_size
        print(
            f"blackbox: {count} record(s) from {len(sectors)}/{total} written sector(s)"
            + (f", seq {sectors[0][0]}..{sectors[-1][0]}" if sectors else ""),
            file=sys.stderr,
        )
    return 0 if count or not sectors else 1


if __name__ == "__main__":
    sys.exit(main())
