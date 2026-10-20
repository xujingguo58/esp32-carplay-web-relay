#!/usr/bin/env python3
"""Validate the embedded M2 stream framing and its Annex-B H.264 access units."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def nal_types(access_unit: bytes) -> set[int]:
    found: set[int] = set()
    offset = 0
    while offset < len(access_unit):
        if access_unit[offset : offset + 4] == b"\x00\x00\x00\x01":
            offset += 4
        elif access_unit[offset : offset + 3] == b"\x00\x00\x01":
            offset += 3
        else:
            raise ValueError(f"missing Annex-B start code at offset {offset}")
        if offset >= len(access_unit):
            raise ValueError("start code has no NAL unit")
        nal_type = access_unit[offset] & 0x1F
        found.add(nal_type)
        offset += 1
        while offset < len(access_unit) and access_unit[offset : offset + 3] not in (
            b"\x00\x00\x01",
            b"\x00\x00\x00",
        ):
            offset += 1
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "stream",
        nargs="?",
        type=Path,
        default=Path("components/media_bridge/test_stream.bin"),
    )
    args = parser.parse_args()
    data = args.stream.read_bytes()
    offset = 0
    count = 0
    idr_count = 0
    max_frame = 0
    while offset < len(data):
        if len(data) - offset < 4:
            raise SystemExit(f"truncated frame header at offset {offset}")
        (size,) = struct.unpack_from(">I", data, offset)
        offset += 4
        if size == 0 or size > len(data) - offset:
            raise SystemExit(f"invalid frame size {size} at frame {count}")
        access_unit = data[offset : offset + size]
        types = nal_types(access_unit)
        if 7 not in types or 8 not in types:
            raise SystemExit(f"frame {count} is missing SPS/PPS NAL units")
        idr_count += 5 in types
        max_frame = max(max_frame, size)
        count += 1
        offset += size
    if count != 40 or idr_count < 4:
        raise SystemExit(f"unexpected stream: frames={count} IDR={idr_count}")
    print(
        f"valid Annex-B stream: {count} access units, {idr_count} IDR frames, "
        f"max access unit {max_frame} bytes, total {len(data)} bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
