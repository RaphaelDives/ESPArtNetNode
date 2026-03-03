#!/usr/bin/env python3
import argparse
import math
import socket
import sys
import time
from typing import Iterable

ARTNET_PORT = 6454
OP_OUTPUT = 0x5000
PROTOCOL_VERSION = 14


def build_artnet_packet(universe: int, sequence: int, dmx_data: bytes, physical: int = 0) -> bytes:
    if not (0 <= universe <= 0x7FFF):
        raise ValueError("Universe must be 0..32767")

    dmx_len = len(dmx_data)
    if dmx_len < 2 or dmx_len > 512:
        raise ValueError("DMX payload must be 2..512 bytes")

    subuni = universe & 0xFF
    net = (universe >> 8) & 0x7F

    header = bytearray()
    header.extend(b"Art-Net\x00")
    header.extend((OP_OUTPUT & 0xFF, (OP_OUTPUT >> 8) & 0xFF))
    header.extend((0x00, PROTOCOL_VERSION))
    header.extend((sequence & 0xFF, physical & 0xFF))
    header.extend((subuni, net))
    header.extend(((dmx_len >> 8) & 0xFF, dmx_len & 0xFF))

    return bytes(header) + dmx_data


def solid_frame(channels: int, r: int, g: int, b: int) -> bytes:
    buf = bytearray(channels)
    for i in range(0, channels, 3):
        if i + 2 >= channels:
            break
        buf[i + 0] = r
        buf[i + 1] = g
        buf[i + 2] = b
    return bytes(buf)


def rainbow_frame(channels: int, phase: float) -> bytes:
    pixels = channels // 3
    buf = bytearray(channels)
    for p in range(pixels):
        t = (p / max(1, pixels)) + phase
        r = int((math.sin((t + 0.0) * 2 * math.pi) * 0.5 + 0.5) * 255)
        g = int((math.sin((t + 0.3333) * 2 * math.pi) * 0.5 + 0.5) * 255)
        b = int((math.sin((t + 0.6666) * 2 * math.pi) * 0.5 + 0.5) * 255)
        i = p * 3
        buf[i + 0] = r
        buf[i + 1] = g
        buf[i + 2] = b
    return bytes(buf)


def chase_frame(channels: int, step: int, width: int = 3) -> bytes:
    pixels = channels // 3
    buf = bytearray(channels)
    for p in range(pixels):
        d = (p - step) % max(1, pixels)
        if d < width:
            v = int(255 * (1 - d / max(1, width)))
            i = p * 3
            buf[i + 0] = v
            buf[i + 1] = int(v * 0.4)
            buf[i + 2] = int(v * 0.1)
    return bytes(buf)


def pulse_frame(channels: int, phase: float, r: int, g: int, b: int) -> bytes:
    level = int((math.sin(phase * 2 * math.pi) * 0.5 + 0.5) * 255)
    return solid_frame(channels, (r * level) // 255, (g * level) // 255, (b * level) // 255)


def send_loop(
    target: str,
    port: int,
    universe: int,
    fps: float,
    pattern: str,
    channels: int,
    color: tuple[int, int, int],
    count: int,
    broadcast: bool,
    source_ip: str | None,
) -> None:
    interval = 1.0 / fps
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if source_ip:
        sock.bind((source_ip, 0))
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)

    # discover the actual local source IP chosen by the OS routing
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        if source_ip:
            probe.bind((source_ip, 0))
        probe.connect((target, port))
        actual_source_ip, _ = probe.getsockname()
    finally:
        probe.close()

    sequence = 1
    sent = 0
    start = time.time()

    print(
        f"Sending Art-Net to {target}:{port} from {actual_source_ip} "
        f"universe={universe} pattern={pattern} channels={channels} fps={fps}"
    )
    try:
        while count <= 0 or sent < count:
            t0 = time.time()

            if pattern == "solid":
                frame = solid_frame(channels, *color)
            elif pattern == "rainbow":
                frame = rainbow_frame(channels, phase=sent * 0.01)
            elif pattern == "chase":
                frame = chase_frame(channels, step=sent % max(1, channels // 3), width=4)
            elif pattern == "pulse":
                frame = pulse_frame(channels, phase=sent * 0.02, r=color[0], g=color[1], b=color[2])
            else:
                raise ValueError(f"Unknown pattern: {pattern}")

            packet = build_artnet_packet(universe=universe, sequence=sequence, dmx_data=frame)
            sock.sendto(packet, (target, port))

            sequence = (sequence + 1) & 0xFF
            if sequence == 0:
                sequence = 1

            sent += 1
            if sent % int(max(1, fps)) == 0:
                elapsed = max(0.001, time.time() - start)
                print(f"sent={sent} avg_fps={sent / elapsed:.1f}")

            dt = time.time() - t0
            sleep_time = interval - dt
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print("\nStopped by user")
    finally:
        sock.close()


def parse_rgb(text: str) -> tuple[int, int, int]:
    parts = text.split(",")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("--rgb must be R,G,B")
    try:
        vals = tuple(int(x) for x in parts)
    except ValueError as ex:
        raise argparse.ArgumentTypeError("--rgb values must be integers") from ex
    if any(v < 0 or v > 255 for v in vals):
        raise argparse.ArgumentTypeError("--rgb values must be 0..255")
    return vals  # type: ignore[return-value]


def main(argv: Iterable[str]) -> int:
    parser = argparse.ArgumentParser(description="Simple Art-Net sender for ESPArtNetNode testing")
    parser.add_argument("--target", default="192.168.4.1", help="Target IP (default: 192.168.4.1)")
    parser.add_argument("--source-ip", default=None, help="Optional local source IPv4 to bind (e.g. 192.168.4.2)")
    parser.add_argument("--port", type=int, default=ARTNET_PORT, help="UDP port (default: 6454)")
    parser.add_argument("--universe", type=int, default=0, help="Art-Net universe 0..32767 (default: 0)")
    parser.add_argument("--fps", type=float, default=40.0, help="Frames per second (default: 40)")
    parser.add_argument("--channels", type=int, default=36, help="DMX channels (2..512, default: 36)")
    parser.add_argument("--pattern", choices=["solid", "rainbow", "chase", "pulse"], default="rainbow")
    parser.add_argument("--rgb", type=parse_rgb, default=(255, 80, 20), help="RGB for solid/pulse, e.g. 255,0,0")
    parser.add_argument("--count", type=int, default=0, help="Number of frames (0 = infinite)")
    parser.add_argument("--broadcast", action="store_true", help="Enable UDP broadcast")

    args = parser.parse_args(list(argv))

    if args.channels < 2 or args.channels > 512:
        parser.error("--channels must be 2..512")
    if args.fps <= 0:
        parser.error("--fps must be > 0")

    send_loop(
        target=args.target,
        port=args.port,
        universe=args.universe,
        fps=args.fps,
        pattern=args.pattern,
        channels=args.channels,
        color=args.rgb,
        count=args.count,
        broadcast=args.broadcast,
        source_ip=args.source_ip,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
