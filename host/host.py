#!/usr/bin/env python3
"""ESP32-4848S040C PC dashboard host.

Collects CPU/GPU/RAM/NET metrics and sends them as a binary stream
to the ESP32 over UART0 (CH340 on Linux: /dev/ttyUSB0).

Protocol (host -> device):
    [0xAA] [len_lo] [len_hi] [type] [payload...]

Types:
    0x01 CPU pct          payload: u8
    0x02 RAM              payload: u8 pct, u32 used_mb, u32 total_mb
    0x03 GPU pct          payload: u8
    0x04 NET              payload: u32 rx_kbps, u32 tx_kbps
"""

import argparse
import json
import socket
import struct
import subprocess
import sys
import time

import psutil
import serial

SYNC = 0xAA
DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200


def get_cpu_pct() -> int:
    return int(psutil.cpu_percent(interval=None))


def get_ram() -> tuple[int, int, int]:
    mem = psutil.virtual_memory()
    return (
        int(mem.percent),
        mem.used // (1024 * 1024),
        mem.total // (1024 * 1024),
    )


def get_gpu_pct() -> int:
    """Read Intel GPU utilization via intel_gpu_top JSON mode.

    Requires CAP_PERFMON; install with:
        sudo setcap cap_perfmon,cap_sys_admin+ep /usr/bin/intel_gpu_top
    """
    try:
        out = subprocess.check_output(
            ["intel_gpu_top", "-J", "-n", "1", "-s", "500"],
            stderr=subprocess.DEVNULL,
            timeout=2,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, subprocess.CalledProcessError):
        return -1
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return -1
    try:
        engines = data["devices"][0]["engine-classes"]
        for eng in engines:
            if "Render" in eng.get("name", "") or "3D" in eng.get("name", ""):
                return int(eng["busy"])
        if engines:
            return int(engines[0]["busy"])
        return 0
    except (KeyError, IndexError, TypeError):
        return -1


def get_net_kbps(sample_interval: float = 0.5) -> tuple[int, int]:
    n1 = psutil.net_io_counters()
    time.sleep(sample_interval)
    n2 = psutil.net_io_counters()
    rx_bps = (n2.bytes_recv - n1.bytes_recv) / sample_interval
    tx_bps = (n2.bytes_sent - n1.bytes_sent) / sample_interval
    return int(rx_bps / 1024), int(tx_bps / 1024)


def send_packet(ser: serial.Serial, pkt_type: int, payload: bytes) -> None:
    length = len(payload)
    header = bytes([SYNC, length & 0xFF, (length >> 8) & 0xFF, pkt_type])
    ser.write(header + payload)


def open_serial(port: str, baud: int) -> serial.Serial:
    while True:
        try:
            s = serial.Serial(
                port, baud, timeout=1,
                rtscts=False, xonxoff=False, dsrdtr=False,
            )
            # CH340 quirk: open sets DTR/RTS HIGH which holds ESP32 in reset.
            # Release immediately so ESP32 boots app.
            s.dtr = False
            s.rts = False
            print(f"[host] connected to {port} @ {baud}", flush=True)
            return s
        except serial.SerialException as e:
            print(f"[host] serial not ready ({e}), retrying...", flush=True)
            time.sleep(2)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--interval", type=float, default=0.5,
                    help="seconds between packets")
    args = ap.parse_args()

    ser = open_serial(args.port, args.baud)
    try:
        while True:
            cpu = get_cpu_pct()
            ram_pct, ram_used, ram_total = get_ram()
            gpu = get_gpu_pct()
            rx_kbps, tx_kbps = get_net_kbps(sample_interval=args.interval)

            send_packet(ser, 0x01, bytes([cpu]))
            send_packet(ser, 0x02, struct.pack("<BII", ram_pct, ram_used, ram_total))
            if gpu >= 0:
                send_packet(ser, 0x03, bytes([gpu]))
            send_packet(ser, 0x04, struct.pack("<II", rx_kbps, tx_kbps))

            print(
                f"[host] cpu={cpu:3d}% ram={ram_used:5d}/{ram_total:5d} MB "
                f"gpu={gpu:3d}% net rx={rx_kbps:5d} tx={tx_kbps:5d} kbps",
                flush=True,
            )
    except KeyboardInterrupt:
        print("\n[host] exiting", flush=True)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
