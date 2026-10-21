#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only verification after booting the candidate m1n1, on either kernel."""
from pathlib import Path
import re
import struct

BASE = Path('/sys/firmware/devicetree/base')
TARGET = BASE / 'soc/dcp@31ac00000'


def cells(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise RuntimeError(f'Malformed property: {path}')
    return struct.unpack('>' + 'I' * (len(data) // 4), data)


def main():
    if cells(TARGET / 'm3gpu,firmware-reservation-version') != (1,):
        raise RuntimeError('Candidate bootloader reservation marker missing')
    ph = cells(TARGET / 'phandle')[0]
    refs = cells(TARGET / 'memory-region')
    if len(refs) != 4:
        raise RuntimeError(f'Unexpected firmware segment count: {len(refs)}')
    ram = []
    for line in Path('/proc/iomem').read_text().splitlines():
        match = re.match(r'\s*([0-9a-f]+)-([0-9a-f]+) : System RAM$', line)
        if match:
            start, end = (int(x, 16) for x in match.groups())
            if end:
                ram.append((start, end + 1))
    if not ram:
        raise RuntimeError('RAM addresses are hidden; run this read-only check as root')
    by_phandle = {cells(p / 'phandle')[0]: p for p in (BASE / 'reserved-memory').iterdir()
                  if p.is_dir() and (p / 'phandle').exists()}
    for ref in refs:
        p = by_phandle[ref]
        if not (p / 'no-map').exists():
            raise RuntimeError(f'Missing no-map: {p}')
        raw = cells(p / 'reg')
        if len(raw) != 4:
            raise RuntimeError(f'Unexpected register layout: {p}')
        phys, size = (raw[0] << 32) | raw[1], (raw[2] << 32) | raw[3]
        if not size or (phys | size) & 0x3fff:
            raise RuntimeError(f'Invalid reservation alignment or size: {p}')
        if any(phys < end and start < phys + size for start, end in ram):
            raise RuntimeError(f'Firmware still intersects System RAM: {p}')
        maps = cells(p / 'iommu-addresses')
        if not maps or len(maps) % 5:
            raise RuntimeError(f'Malformed IOMMU mappings: {p}')
        matches = [maps[i:i + 5] for i in range(0, len(maps), 5) if maps[i] == ph]
        if len(matches) != 1 or ((matches[0][3] << 32) | matches[0][4]) != size:
            raise RuntimeError(f'Missing or invalid external-DCP mapping: {p}')
        print(f'{p.name}: {phys:#x} + {size:#x}, reserved outside System RAM')
    print('PASS: all four live firmware segments reserved. No hardware state changed.')


if __name__ == '__main__':
    main()
