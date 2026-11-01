#!/usr/bin/env python3
"""Compare MUHD raw span dumps: handoff_diff.py <a> <b> (largest ranges first)."""
import argparse
from pathlib import Path
import struct
import sys

MAX_RAW = 128 * 1024 * 1024


def read_dump(path):
    # Cap actual reads, rather than trusting stat() for files that can grow concurrently.
    with Path(path).open('rb') as stream:
        data = stream.read(MAX_RAW + 20 + 65536 * 12 + 1)
    if len(data) < 20:
        raise ValueError('truncated dump header')
    magic, version, count, raw = struct.unpack_from('<4sIIQ', data)
    if magic != b'MUHD' or version != 1:
        raise ValueError('wrong dump magic/version')
    if not 0 < count <= 65536 or not 0 < raw <= MAX_RAW:
        raise ValueError('invalid dump size/count')
    expected = 20 + 12 * count + raw
    if len(data) != expected:
        raise ValueError('truncated dump' if len(data) < expected else 'oversized dump')
    table = [struct.unpack_from('<QI', data, 20 + i * 12) for i in range(count)]
    end = 0
    for address, size in table:
        if not address or not size or address < end or address + size > 2**64 - 1:
            raise ValueError('invalid span table')
        end = address + size
    if sum(size for _, size in table) != raw:
        raise ValueError('raw span size mismatch')
    return table, data[20 + 12 * count:]


def differences(table, a, b):
    """Merge address gaps strictly less than 64 bytes, even across span boundaries."""
    ranges = []
    offset = 0
    first = last = None
    changed = 0
    for address, size in table:
        for i in range(size):
            if a[offset + i] == b[offset + i]:
                continue
            here = address + i
            if last is not None and here - last - 1 >= 64:
                ranges.append((first, last - first + 1, changed))
                first = None
                changed = 0
            if first is None:
                first = here
            last = here
            changed += 1
        offset += size
    if first is not None:
        ranges.append((first, last - first + 1, changed))
    return sorted(ranges, key=lambda item: (-item[1], item[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('a')
    parser.add_argument('b')
    args = parser.parse_args()
    try:
        table, a = read_dump(args.a)
        other, b = read_dump(args.b)
        if table != other:
            raise ValueError('different span tables')
        ranges = differences(table, a, b)
    except (OSError, ValueError) as exc:
        print(f'handoff_diff: {exc}', file=sys.stderr)
        return 2
    for address, size, changed in ranges:
        print(f'0x{address:016x}-0x{address + size:016x} {size} bytes ({changed} differing)')
    print(f'{len(ranges)} ranges, {sum(item[2] for item in ranges)} differing bytes')
    return 1 if ranges else 0


if __name__ == '__main__':
    sys.exit(main())
