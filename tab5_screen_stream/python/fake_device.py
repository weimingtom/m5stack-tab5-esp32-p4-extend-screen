#!/usr/bin/env python3
"""Offline stand-in for the Tab5 firmware.

It parses the exact same byte stream the firmware accepts, so the sender can be
validated without hardware:

    # 1. record a stream to a file ...
    python screen_stream.py --dry-run frames.bin --count 20
    python fake_device.py --file frames.bin

    # 2. ... or serve TCP 8266 and point the sender at it (--tcp 127.0.0.1)
    python fake_device.py --listen 8266
    python screen_stream.py --tcp 127.0.0.1:8266 --count 20

Frames are JPEG in the default configuration, so ``--save`` decodes and writes
the first one as a PNG for a quick visual check of scaling and rotation.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time

import frame_proto as proto


def consume(chunks, *, save: str | None, quiet: bool) -> int:
    frames = 0
    total = 0
    first_payload = None
    first_size = (0, 0)
    started = time.perf_counter()

    for frame in proto.iter_frames(chunks):
        frames += 1
        total += len(frame.payload)
        if first_payload is None:
            first_payload = frame.payload
            first_size = (frame.width, frame.height)
            if not quiet:
                kind = "jpeg" if frame.is_jpeg else "rgb565"
                print(
                    f"first frame: seq={frame.seq} {frame.width}x{frame.height} {kind} "
                    f"{len(frame.payload)} bytes crc={frame.crc32:08x}"
                )
        if not quiet and frames % 20 == 0:
            elapsed = time.perf_counter() - started
            print(
                f"frames={frames} avg={frames / elapsed:.1f} fps "
                f"{total / elapsed / 1000:.0f} kB/s"
            )

    elapsed = max(time.perf_counter() - started, 1e-9)
    print(f"done: {frames} frames, {total} bytes, {frames / elapsed:.1f} fps average")

    if save and first_payload is not None:
        width, height = first_size
        if first_payload[:2] == b"\xff\xd8":
            from PIL import Image

            import io

            image = Image.open(io.BytesIO(first_payload))
            image.save(save)
            print(f"wrote first frame ({image.width}x{image.height}) to {save}")
        else:
            print("first frame is raw RGB565; --save only supports JPEG frames", file=sys.stderr)
    return 0 if frames else 1


def read_file(path: str):
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(65536)
            if not chunk:
                return
            yield chunk


def serve(port: int, *, save: str | None, quiet: bool) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", port))
        server.listen(1)
        print(f"waiting for a sender on port {port} ...", flush=True)
        conn, addr = server.accept()
        print(f"connected: {addr[0]}:{addr[1]}", flush=True)
        with conn:
            conn.settimeout(5.0)

            def chunks():
                while True:
                    try:
                        data = conn.recv(65536)
                    except socket.timeout:
                        return
                    if not data:
                        return
                    yield data

            return consume(chunks(), save=save, quiet=quiet)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", help="parse a recorded byte stream")
    parser.add_argument("--listen", type=int, help="listen on this TCP port instead")
    parser.add_argument("--save", help="write the first frame as PNG")
    parser.add_argument("--quiet", action="store_true", help="only print the final summary")
    args = parser.parse_args(argv)

    if args.file:
        return consume(read_file(args.file), save=args.save, quiet=args.quiet)
    if args.listen:
        return serve(args.listen, save=args.save, quiet=args.quiet)
    parser.error("choose --file PATH or --listen PORT")


if __name__ == "__main__":
    sys.exit(main())
