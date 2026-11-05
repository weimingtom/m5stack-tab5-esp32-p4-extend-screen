#!/usr/bin/env python3
"""Stream the PC screen to an M5Stack Tab5 (ESP32-P4) over USB or Wi-Fi.

The script grabs the desktop, fits it into the Tab5 panel (720x1280 native,
letterboxed), optionally rotates it 90 degrees for the landscape bezel,
JPEG-encodes it and pushes length-prefixed frames to the board.  The firmware
decodes each frame with the ESP32-P4 hardware JPEG decoder and paints it on
the MIPI-DSI panel, keeping the raw payload in PSRAM.

Examples
--------
    # USB: same USB-C cable you flash with, no extra setup
    python screen_stream.py --serial COM7

    # Wi-Fi: the board listens on TCP 8266 once it has joined your AP
    python screen_stream.py --tcp 192.168.1.50

    # 30 fps, lower quality (smaller frames = higher frame rate on USB)
    python screen_stream.py --serial COM7 --fps 30 --quality 55

    # See which screens are available, and check the stream offline
    python screen_stream.py --list-monitors
    python screen_stream.py --dry-run frames.bin --count 10
"""

from __future__ import annotations

import argparse
import io
import socket
import sys
import threading
import time

import frame_proto as proto

try:
    from PIL import Image
except ImportError:  # pragma: no cover - handled with a friendly message
    Image = None


# --------------------------------------------------------------------------
# capture
# --------------------------------------------------------------------------


def _new_mss():
    """mss 10 renamed the factory from ``mss.mss`` to ``mss.MSS``."""
    import mss

    factory = getattr(mss, "MSS", None) or mss.mss
    return factory()


def list_monitors() -> None:
    try:
        import mss  # noqa: F401  (probe the import only)
    except ImportError:
        print("mss is not installed; run: pip install -r requirements.txt")
        return
    with _new_mss() as sct:
        for index, monitor in enumerate(sct.monitors):
            kind = "virtual desktop (all screens)" if index == 0 else f"screen {index}"
            print(
                f"  monitor {index}: {kind} "
                f"{monitor['width']}x{monitor['height']} at ({monitor['left']}, {monitor['top']})"
            )


class Grabber:
    """Screenshot source: mss when available, PIL.ImageGrab otherwise."""

    def __init__(self, monitor: int):
        self.monitor = monitor
        self._sct = None
        try:
            self._sct = _new_mss()
            if monitor >= len(self._sct.monitors):
                raise SystemExit(
                    f"monitor {monitor} does not exist (1..{len(self._sct.monitors) - 1})"
                )
        except ImportError:
            if monitor != 0:
                raise SystemExit("installing mss is required to capture a specific monitor")

    def grab(self) -> "Image.Image":
        if self._sct is not None:
            shot = self._sct.grab(self._sct.monitors[self.monitor])
            return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
        return ImageGrabFallback()

    def close(self) -> None:
        if self._sct is not None:
            self._sct.close()


def ImageGrabFallback() -> "Image.Image":  # noqa: N802 - reads like a constructor
    from PIL import ImageGrab

    return ImageGrab.grab(all_screens=False).convert("RGB")


def render(img: "Image.Image", panel: tuple[int, int], rotate: int) -> "Image.Image":
    """Letterbox ``img`` into the panel and rotate it for the bezel."""
    want_w, want_h = panel
    if rotate % 180 == 90:
        # the frame is rotated after fitting, so fit into the swapped box
        want_w, want_h = want_h, want_w

    scale = min(want_w / img.width, want_h / img.height)
    new_w = max(1, min(want_w, round(img.width * scale)))
    new_h = max(1, min(want_h, round(img.height * scale)))
    resample = Image.LANCZOS if scale < 1.0 else Image.BILINEAR
    scaled = img.resize((new_w, new_h), resample)

    canvas = Image.new("RGB", (want_w, want_h), (0, 0, 0))
    canvas.paste(scaled, ((want_w - new_w) // 2, (want_h - new_h) // 2))

    rotate %= 360
    if rotate == 90:
        canvas = canvas.transpose(Image.ROTATE_270)  # clockwise
    elif rotate == 180:
        canvas = canvas.transpose(Image.ROTATE_180)
    elif rotate == 270:
        canvas = canvas.transpose(Image.ROTATE_90)  # counter-clockwise
    return canvas


def encode(img: "Image.Image", quality: int, jpeg: bool) -> bytes:
    if jpeg:
        buffer = io.BytesIO()
        # subsampling=2 -> 4:2:0, which the P4 hardware decoder handles; it also
        # keeps the payload small enough for the USB-Serial-JTAG link.
        img.save(buffer, format="JPEG", quality=quality, subsampling=2, optimize=False)
        return buffer.getvalue()

    raw = img.tobytes()
    try:
        import numpy as np
    except ImportError:
        # pure-python fallback: slow, but keeps --codec rgb565 usable
        out = bytearray(len(raw) // 3 * 2)
        o = 0
        for r, g, b in zip(raw[0::3], raw[1::3], raw[2::3]):
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[o] = value & 0xFF
            out[o + 1] = value >> 8
            o += 2
        return bytes(out)

    pixels = np.asarray(img, dtype=np.uint16)
    packed = (((pixels[:, :, 0] & 0xF8) << 8) | ((pixels[:, :, 1] & 0xFC) << 3) | (pixels[:, :, 2] >> 3))
    return packed.astype("<u2").tobytes()


# --------------------------------------------------------------------------
# transports
# --------------------------------------------------------------------------


class SerialTransport:
    """USB-Serial/JTAG CDC-ACM link - the port used for flashing too."""

    def __init__(self, port: str, baud: int):
        import serial  # pyserial

        self.name = port
        self._ser = serial.Serial(
            port,
            baudrate=baud,
            timeout=0.05,
            write_timeout=5.0,
            rtscts=False,
            dsrdtr=False,
        )
        # esptool-style reset lines: leave both deasserted so the board keeps
        # running instead of rebooting into the ROM bootloader.
        try:
            self._ser.dtr = False
            self._ser.rts = False
        except Exception:  # pragma: no cover - not all backends support it
            pass
        self._ser.reset_input_buffer()

    def send(self, data: bytes) -> None:
        self._ser.write(data)

    def read_line(self) -> str | None:
        try:
            line = self._ser.readline()
        except Exception:
            return None
        return line.decode("utf-8", "replace") if line else None

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass


class TcpTransport:
    """Plain TCP: the firmware listens on the panel once Wi-Fi is up."""

    def __init__(self, host: str, port: int, timeout: float = 10.0):
        self.name = f"{host}:{port}"
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(5.0)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._pending = b""

    def send(self, data: bytes) -> None:
        self._sock.sendall(data)

    def read_line(self) -> str | None:
        while b"\n" not in self._pending:
            try:
                chunk = self._sock.recv(4096)
            except socket.timeout:
                return None
            except OSError:
                return None
            if not chunk:
                return None
            self._pending += chunk
        line, _, self._pending = self._pending.partition(b"\n")
        return line.decode("utf-8", "replace")

    def close(self) -> None:
        try:
            self._sock.close()
        except Exception:
            pass


class FileTransport:
    """Writes the raw byte stream to a file (for offline verification)."""

    def __init__(self, path: str):
        self.name = path
        self._file = open(path, "wb")

    def send(self, data: bytes) -> None:
        self._file.write(data)

    def read_line(self) -> str | None:
        return None

    def close(self) -> None:
        self._file.close()


# --------------------------------------------------------------------------
# device feedback
# --------------------------------------------------------------------------


class DeviceListener(threading.Thread):
    """Consumes ``STAT``/``LOG`` lines coming back from the board."""

    def __init__(self, transport):
        super().__init__(daemon=True, name="device-listener")
        self._transport = transport
        self._stop = threading.Event()
        self.last_status: proto.DeviceStatus | None = None
        self.logs: list[str] = []

    def run(self) -> None:
        while not self._stop.is_set():
            line = self._transport.read_line()
            if line is None:
                time.sleep(0.01)
                continue
            parsed = proto.parse_device_line(line)
            if parsed is None:
                continue
            kind, value = parsed
            if kind == "log":
                self.logs.append(value)
                print(f"[tab5] {value}", flush=True)
            else:
                self.last_status = value

    def stop(self) -> None:
        self._stop.set()


# --------------------------------------------------------------------------
# main loop
# --------------------------------------------------------------------------


def parse_panel(text: str) -> tuple[int, int]:
    try:
        width, height = text.lower().split("x")
        return int(width), int(height)
    except ValueError:
        raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT, e.g. 720x1280")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Stream the PC screen to an M5Stack Tab5 (ESP32-P4).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    link = parser.add_argument_group("link")
    link.add_argument("--serial", metavar="PORT", help="USB-Serial/JTAG port, e.g. COM7 or /dev/ttyACM0")
    link.add_argument("--baud", type=int, default=115200, help="ignored by the CDC-ACM link, kept for pyserial")
    link.add_argument("--tcp", metavar="HOST[:PORT]", help="connect to the board's TCP server (Wi-Fi mode)")
    link.add_argument("--dry-run", metavar="FILE", help="write the byte stream to FILE instead of a device")

    shot = parser.add_argument_group("capture")
    shot.add_argument("--monitor", type=int, default=1, help="mss monitor index (see --list-monitors)")
    shot.add_argument("--list-monitors", action="store_true", help="list screens and exit")
    shot.add_argument("--panel", type=parse_panel, default=(proto.TAB5_PANEL_WIDTH, proto.TAB5_PANEL_HEIGHT),
                      help="panel resolution in the board's native orientation")
    shot.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=90,
                      help="clockwise rotation applied after fitting; 90 or 270 for the landscape bezel")
    shot.add_argument("--fps", type=float, default=10.0, help="frame rate cap, 0 = unlimited")
    shot.add_argument("--quality", type=int, default=70, help="JPEG quality")
    shot.add_argument("--codec", choices=("jpeg", "rgb565"), default="jpeg",
                      help="jpeg uses the P4 hardware decoder; rgb565 is raw and needs ~%.1f MB per frame"
                           % (proto.TAB5_PANEL_WIDTH * proto.TAB5_PANEL_HEIGHT * 2 / 1e6))
    shot.add_argument("--count", type=int, default=0, help="stop after N frames, 0 = until Ctrl+C")
    shot.add_argument("--duration", type=float, default=0.0, help="stop after N seconds, 0 = until Ctrl+C")
    shot.add_argument("--max-frame-kb", type=int, default=512,
                      help="largest payload the firmware slot accepts (CONFIG_APP_PAYLOAD_BUFFER_KB)")
    shot.add_argument("--stats", type=float, default=2.0, help="seconds between local stats lines, 0 = quiet")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if Image is None:
        print("Pillow is required: pip install -r requirements.txt", file=sys.stderr)
        return 2

    if args.list_monitors:
        list_monitors()
        return 0

    if not (args.serial or args.tcp or args.dry_run):
        parser.error("choose a link: --serial PORT, --tcp HOST[:PORT] or --dry-run FILE")

    transport = None
    if args.serial:
        transport = SerialTransport(args.serial, args.baud)
    elif args.tcp:
        host, _, port = args.tcp.partition(":")
        transport = TcpTransport(host, int(port or 8266))
    else:
        transport = FileTransport(args.dry_run)

    grabber = Grabber(args.monitor)
    listener = DeviceListener(transport)
    listener.start()

    jpeg = args.codec == "jpeg"
    max_payload = args.max_frame_kb * 1024
    if not jpeg:
        raw_size = args.panel[0] * args.panel[1] * 2
        if raw_size > max_payload:
            raise SystemExit(
                f"--codec rgb565 needs {raw_size / 1024:.0f} KB per frame but the firmware slot is "
                f"{args.max_frame_kb} KB: raise CONFIG_APP_PAYLOAD_BUFFER_KB and --max-frame-kb"
            )
    interval = 1.0 / args.fps if args.fps > 0 else 0.0

    print(
        f"streaming {args.monitor} -> {transport.name} "
        f"{args.panel[0]}x{args.panel[1]} rotate={args.rotate} codec={args.codec} "
        f"quality={args.quality} fps<={args.fps:g}  (Ctrl+C to stop)",
        flush=True,
    )

    frames = 0
    sent_bytes = 0
    dropped = 0
    started = time.perf_counter()
    next_deadline = started
    next_stats = started + args.stats if args.stats > 0 else None
    seq = 0
    exit_code = 0

    try:
        while True:
            now = time.perf_counter()
            if args.duration and now - started >= args.duration:
                break
            if args.count and frames >= args.count:
                break

            if interval:
                wait = next_deadline - now
                if wait > 0:
                    time.sleep(min(wait, interval))
                next_deadline = max(next_deadline + interval, time.perf_counter())

            image = render(grabber.grab(), args.panel, args.rotate)
            payload = encode(image, args.quality, jpeg)
            if len(payload) > max_payload:
                dropped += 1
                print(
                    f"frame {seq} is {len(payload)} bytes, above the {max_payload} byte limit: "
                    f"lower --quality or --fps",
                    file=sys.stderr,
                )
                seq += 1
                continue

            packet = proto.pack_frame(seq, image.width, image.height, payload, jpeg=jpeg)
            transport.send(packet)
            frames += 1
            sent_bytes += len(packet)
            seq += 1

            if next_stats is not None and time.perf_counter() >= next_stats:
                elapsed = time.perf_counter() - started
                status = listener.last_status
                extra = ""
                if status is not None:
                    extra = (
                        f" | tab5: seq={status.seq} dec={status.decode_us / 1000:.1f}ms "
                        f"draw={status.draw_us / 1000:.1f}ms fps={status.fps:.1f} "
                        f"drop={status.drop} badcrc={status.bad_crc} free={status.free_kb}KB"
                    )
                print(
                    f"[pc] frames={frames} dropped={dropped} "
                    f"fps={frames / elapsed:.1f} link={sent_bytes * 8 / elapsed / 1000:.0f}kbps "
                    f"frame={len(payload) / 1024:.0f}KB{extra}",
                    flush=True,
                )
                next_stats += args.stats
    except KeyboardInterrupt:
        print("\nstopping", flush=True)
    except (BrokenPipeError, ConnectionResetError) as exc:
        print(f"link closed: {exc}", file=sys.stderr)
        exit_code = 1
    finally:
        listener.stop()
        transport.close()
        grabber.close()

    elapsed = time.perf_counter() - started
    print(
        f"sent {frames} frames, {sent_bytes / 1e6:.1f} MB in {elapsed:.1f}s "
        f"({frames / elapsed:.1f} fps average)"
        if elapsed > 0
        else "nothing sent"
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
