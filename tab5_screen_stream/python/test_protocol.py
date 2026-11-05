#!/usr/bin/env python3
"""Self-check for the frame protocol (no board required).

Verifies that:
  * the CRC-32 used by the firmware (nibble table, see scr_frame.c) produces
    exactly the same values as Python's zlib.crc32, so `--dry-run` output and
    the on-device check agree;
  * a sender -> receiver round trip survives garbage bytes and truncated frames;
  * the header layout matches what scr_frame.h parses.

    python test_protocol.py
"""

from __future__ import annotations

import os
import struct
import sys
import zlib

import frame_proto as proto

# Literal copy of s_crc32_nibble[] from firmware/main/scr_frame.c
CRC32_NIBBLE = [
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
]


def firmware_crc32(seed: int, data: bytes) -> int:
    """The exact loop from scr_crc32() in the firmware."""
    crc = (~seed) & 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0F]
        crc = (crc >> 4) ^ CRC32_NIBBLE[crc & 0x0F]
    return (~crc) & 0xFFFFFFFF


def check_crc() -> None:
    assert firmware_crc32(0, b"") == zlib.crc32(b"") & 0xFFFFFFFF
    assert firmware_crc32(0, b"123456789") == zlib.crc32(b"123456789") & 0xFFFFFFFF
    for size in (1, 2, 3, 7, 22, 64, 255, 1000, 4096):
        data = os.urandom(size)
        assert firmware_crc32(0, data) == zlib.crc32(data) & 0xFFFFFFFF, f"crc mismatch at {size} bytes"
    print("crc32: firmware algorithm == zlib.crc32  (empty, ASCII vector, random sizes)")


def check_header() -> None:
    payload = b"\xff\xd8jpeg-ish bytes\xff\xd9"
    packet = proto.pack_frame(seq=7, width=720, height=1280, payload=payload, jpeg=True)
    assert len(packet) == proto.HEADER_SIZE + len(payload)
    magic, version, flags, seq, width, height, length, crc = proto.HEADER.unpack_from(packet, 0)
    assert magic == b"SCRF" and version == 1 and flags == proto.FLAG_JPEG
    assert (seq, width, height, length) == (7, 720, 1280, len(payload))
    assert crc == zlib.crc32(payload) & 0xFFFFFFFF
    print(f"header: {proto.HEADER_SIZE} bytes, magic/version/flags/length/crc round-trip")


def check_resync() -> None:
    frames = [
        proto.pack_frame(i, 4, 4, bytes([i]) * 32, jpeg=True) for i in range(5)
    ]
    stream = b"\x00garbageSCR" + frames[0][:-5] + frames[1] + b"noise" + frames[2] + frames[3] + frames[4]
    seen = [f.seq for f in proto.iter_frames([stream])]
    # A truncated frame costs that frame *and* whatever follows inside the
    # number of bytes its header claimed (frame 1 here); after that the parser
    # resynchronises on the magic and recovers everything else.  This is exactly
    # what the firmware does, so one bad length never poisons the stream.
    assert seen == [2, 3, 4], seen
    print(f"resync: recovered {seen} after garbage, a truncated frame and noise")


def check_corrupt_payload() -> None:
    good = proto.pack_frame(11, 2, 2, b"abcdefgh", jpeg=False)
    bad = bytearray(proto.pack_frame(12, 2, 2, b"abcdefgh", jpeg=False))
    bad[-1] ^= 0xFF  # flip a payload byte -> crc must reject the frame
    seen = [f.seq for f in proto.iter_frames([bytes(good) + bytes(bad)])]
    assert seen == [11], seen
    print("corrupt payload: rejected by crc, following frames still parsed")


def check_device_lines() -> None:
    kind, log = proto.parse_device_line("LOG waiting for frames on tcp/8266")
    assert kind == "log" and log.startswith("waiting")
    kind, status = proto.parse_device_line(
        "STAT seq=41 w=720 h=1280 bytes=135160 dec_us=9821 draw_us=6104 fps=8.4 "
        "kbps=9200 drop=0 badcrc=0 free_kb=28410 ok=0"
    )
    assert kind == "stat"
    assert (status.seq, status.width, status.height, status.nbytes) == (41, 720, 1280, 135160)
    assert (status.decode_us, status.draw_us, status.free_kb) == (9821, 6104, 28410)
    assert abs(status.fps - 8.4) < 1e-9
    assert proto.parse_device_line("random boot noise") is None
    print("device lines: STAT parsed, LOG parsed, noise ignored")


if __name__ == "__main__":
    try:
        check_crc()
        check_header()
        check_resync()
        check_corrupt_payload()
        check_device_lines()
    except AssertionError as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
    print("all protocol checks passed")
