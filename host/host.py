#!/usr/bin/env python3
"""ESP32-4848S040C PC dashboard host (protocol v2).

Collects CPU/GPU/RAM/NET/DISK/PROC metrics and sends them as one batched
frame (header + TLV payload + CRC-8) over UART0 (CH340 on Linux:
/dev/ttyUSB0). The GPU is read by a background IntelGpuMonitor thread
that polls DRM fdinfo (/proc) for VRAM usage and engine-busy deltas
(no root / no intel_gpu_top required).
"""

import argparse
import os
import sys
import time

import psutil
import serial

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from host import metrics
from host.gpu_monitor import IntelGpuMonitor
from host.protocol import (
    pack_frame,
    FIELD_CPU,
    FIELD_RAM,
    FIELD_GPU,
    FIELD_NET,
    FIELD_DISK,
    FIELD_HEADER,
    FIELD_PROC,
    build_cpu,
    build_ram,
    build_gpu,
    build_net,
    build_disk,
    build_header,
    build_proc,
)

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200


def open_serial(port: str, baud: int) -> serial.Serial:
    while True:
        try:
            s = serial.Serial(
                port, baud, timeout=1,
                rtscts=False, xonxoff=False, dsrdtr=False,
            )
            # CH340 quirk: open sets DTR/RTS HIGH which holds ESP32 in reset.
            s.dtr = False
            s.rts = False
            print(f"[host] connected to {port} @ {baud}", flush=True)
            return s
        except serial.SerialException as e:
            print(f"[host] serial not ready ({e}), retrying...", flush=True)
            time.sleep(2)


def build_frame(snaps: dict, interval: float) -> bytes:
    fields = [
        (FIELD_CPU, build_cpu(snaps["cpu"][0], snaps["cpu"][1])),
        (FIELD_RAM, build_ram(*snaps["ram"])),
        (FIELD_GPU, build_gpu(*snaps["gpu"])),
        (FIELD_NET, build_net(*snaps["net"])),
        (FIELD_DISK, build_disk(*snaps["disk"])),
        (FIELD_HEADER, build_header(*snaps["header"])),
        (FIELD_PROC, build_proc(snaps["proc"])),
    ]
    return pack_frame(fields)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--interval", type=float, default=0.5,
                    help="seconds between frames")
    args = ap.parse_args()

    ser = open_serial(args.port, args.baud)
    monitor = IntelGpuMonitor()
    net_prev = psutil.net_io_counters()
    disk_prev = psutil.disk_io_counters()
    prev_t = time.monotonic()

    try:
        while True:
            now_t = time.monotonic()
            interval = max(0.001, now_t - prev_t)
            prev_t = now_t

            net_now = psutil.net_io_counters()
            disk_now = psutil.disk_io_counters() or None

            snaps = {
                "cpu": metrics.cpu_snapshot(),
                "ram": metrics.ram_snapshot(),
                "gpu": metrics.gpu_snapshot(monitor),
                "net": metrics.net_snapshot(net_prev, net_now, interval),
                "disk": metrics.disk_snapshot(disk_prev, disk_now, interval),
                "header": metrics.header_snapshot(),
                "proc": metrics.proc_snapshot(),
            }
            net_prev = net_now
            disk_prev = disk_now

            ser.write(build_frame(snaps, interval))

            cpu = snaps["cpu"][0]
            ram_pct, ram_used, ram_total = snaps["ram"]
            gpu, vram_pct, vram_used, vram_total = snaps["gpu"]
            rx, tx = snaps["net"]
            rd, wr, used_pct = snaps["disk"]

            print(
                f"[host] cpu={cpu:3d}% ram={ram_used:5d}/{ram_total:5d} MB "
                f"gpu={gpu:3d}% vram={vram_used:4d}/{vram_total:4d} MB "
                f"net rx={rx:5d} tx={tx:5d} KiB/s "
                f"disk rd={rd:5d} wr={wr:5d} KiB/s used={used_pct:3d}%",
                flush=True,
            )
            time.sleep(interval)
    except KeyboardInterrupt:
        print("\n[host] exiting", flush=True)
    finally:
        monitor.stop()
        ser.close()


if __name__ == "__main__":
    main()