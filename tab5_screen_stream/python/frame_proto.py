"""Frame protocol shared by the PC sender and the offline test receiver.

Wire format (all integers little-endian)::

    offset  size  field
    ------  ----  -------------------------------------------------
    0       4     magic  b"SCRF"
    4       1     version (1)
    5       1     flags   bit0: 1 = JPEG payload, 0 = raw RGB565
    6       4     seq     frame counter, wraps at 2**32
    10      2     width   visible width  of the payload in pixels
    12      2     height  visible height of the payload in pixels
    14      4     length  payload length in bytes
    18      4     crc32   zlib/IEEE CRC of the payload
    22      ...   payload

The firmware parses exactly this layout (see ``firmware/main/scr_frame.h``);
the CRC must stay bit-for-bit compatible with ``zlib.crc32``.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAGIC = b"SCRF"
VERSION = 1

FLAG_JPEG = 0x01
FLAG_RAW_RGB565 = 0x00

HEADER = struct.Struct("<4sBBiHHII")
HEADER_SIZE = HEADER.size
assert HEADER_SIZE == 22

#: The Tab5 MIPI-DSI panel is mounted natively as 720x1280 (portrait).
TAB5_PANEL_WIDTH = 720
TAB5_PANEL_HEIGHT = 1280

#: Hard limit matching the firmware's receive buffers.
MAX_PAYLOAD = 4 * 1024 * 1024


@dataclass
class Frame:
    """One decoded frame as seen by a receiver."""

    seq: int
    width: int
    height: int
    flags: int
    crc32: int
    payload: bytes

    @property
    def is_jpeg(self) -> bool:
        return bool(self.flags & FLAG_JPEG)


def pack_frame(seq: int, width: int, height: int, payload: bytes, *, jpeg: bool = True) -> bytes:
    """Serialize one frame into the on-wire representation."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload of {len(payload)} bytes exceeds the {MAX_PAYLOAD} byte limit")
    flags = FLAG_JPEG if jpeg else FLAG_RAW_RGB565
    header = HEADER.pack(
        MAGIC,
        VERSION,
        flags,
        seq & 0xFFFFFFFF,
        width,
        height,
        len(payload),
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    return header + payload


def iter_frames(chunks):
    """Parse frames out of an iterable of byte chunks.

    Yields :class:`Frame` objects.  Resynchronises on the magic word after a
    corrupt length, so a single dropped byte only costs one frame.
    """
    buf = bytearray()
    for chunk in chunks:
        if chunk:
            buf += chunk
        while True:
            start = buf.find(MAGIC)
            if start < 0:
                # keep a tail that could still contain the beginning of a magic
                if len(buf) > len(MAGIC) - 1:
                    del buf[: len(buf) - (len(MAGIC) - 1)]
                break
            if start:
                del buf[:start]
            if len(buf) < HEADER_SIZE:
                break
            magic, version, flags, seq, width, height, length, crc = HEADER.unpack_from(buf, 0)
            if version != VERSION or length > MAX_PAYLOAD:
                del buf[: len(MAGIC)]
                continue
            total = HEADER_SIZE + length
            if len(buf) < total:
                break
            payload = bytes(buf[HEADER_SIZE:total])
            del buf[:total]
            if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
                # corrupt payload: drop this frame, keep scanning from the next
                # byte so that a following frame is still found quickly
                continue
            yield Frame(seq=seq, width=width, height=height, flags=flags, crc32=crc, payload=payload)


# --------------------------------------------------------------------------
# device -> PC status lines
# --------------------------------------------------------------------------


@dataclass
class DeviceStatus:
    """Parsed ``STAT`` line sent by the firmware."""

    seq: int = 0
    width: int = 0
    height: int = 0
    nbytes: int = 0
    decode_us: int = 0
    draw_us: int = 0
    fps: float = 0.0
    kbps: float = 0.0
    drop: int = 0
    bad_crc: int = 0
    free_kb: int = 0
    min_free_kb: int = 0


def parse_device_line(line: str):
    """Turn one newline-terminated device line into ("log"|"stat", value).

    Returns ``None`` for lines that are neither (e.g. stray boot output).
    """
    line = line.strip()
    if line.startswith("LOG "):
        return "log", line[4:]
    if line.startswith("STAT "):
        status = DeviceStatus()
        for item in line[5:].split():
            key, _, value = item.partition("=")
            try:
                if key in ("fps", "kbps"):
                    setattr(status, key, float(value))
                elif key in ("w", "h"):
                    setattr(status, "width" if key == "w" else "height", int(value))
                elif key == "bytes":
                    status.nbytes = int(value)
                elif key == "dec_us":
                    status.decode_us = int(value)
                elif key == "draw_us":
                    status.draw_us = int(value)
                elif key == "badcrc":
                    status.bad_crc = int(value)
                elif key == "free_kb":
                    status.free_kb = int(value)
                elif key == "min_free_kb":
                    status.min_free_kb = int(value)
                elif key in ("seq", "drop"):
                    setattr(status, key, int(value))
            except ValueError:
                continue
        return "stat", status
    return None
