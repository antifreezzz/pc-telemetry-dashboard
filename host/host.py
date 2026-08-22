#!/usr/bin/env python3
"""ESP32-4848S040C PC dashboard host (protocol v2).

Collects CPU/GPU/RAM/NET/DISK/PROC/LLM metrics and sends them as one batched
frame (header + TLV payload + CRC-8) over UART0 (CH340 on Linux:
/dev/ttyUSB0). The GPU is read by a background IntelGpuMonitor thread
that polls DRM fdinfo (/proc) for VRAM usage and engine-busy deltas
(no root / no intel_gpu_top required). LLM status is read from llmcontrol
REST API by a background LLMMonitor thread.
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
from host.llm_client import LLMMonitor
from host.protocol import (
    SYNC,
    pack_frame,
    parse_serial_command,
    FIELD_CPU,
    FIELD_RAM,
    FIELD_GPU,
    FIELD_NET,
    FIELD_DISK,
    FIELD_HEADER,
    FIELD_PROC,
    FIELD_LLM,
    FIELD_LLM_MODELS,
    PROC_KIND_CPU,
    PROC_KIND_RAM,
    PROC_KIND_GPU,
    PROC_KIND_DISK_RD,
    PROC_KIND_DISK_WR,
    LLM_STATUS_IDLE,
    LLM_STATUS_RUNNING,
    LLM_STATUS_STARTING,
    LLM_STATUS_OFFLINE,
    CMD_STOP_ALL,
    CMD_START_FAVORITE,
    build_cpu,
    build_ram,
    build_gpu,
    build_net,
    build_disk,
    build_header,
    build_proc,
    build_llm,
    build_llm_models,
)

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200
DEFAULT_LLM_URL = "http://127.0.0.1:8666"


def open_serial(port: str, baud: int) -> serial.Serial:
    while True:
        try:
            s = serial.Serial(
                port, baud, timeout=0.1,
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
        (FIELD_PROC, build_proc(PROC_KIND_CPU, snaps["proc_cpu"])),
        (FIELD_PROC, build_proc(PROC_KIND_RAM, snaps["proc_ram"])),
        (FIELD_PROC, build_proc(PROC_KIND_GPU, snaps["proc_gpu"])),
        (FIELD_PROC, build_proc(PROC_KIND_DISK_RD,
                                [(rd, pid, name)
                                 for rd, _wr, pid, name in snaps["proc_disk"]])),
        (FIELD_PROC, build_proc(PROC_KIND_DISK_WR,
                                [(wr, pid, name)
                                 for _rd, wr, pid, name in snaps["proc_disk"]])),
        (FIELD_LLM, build_llm(*snaps["llm"])),
        (FIELD_LLM_MODELS, build_llm_models(snaps["llm_models"])),
    ]
    return pack_frame(fields)


def _execute_cmd(cmd, arg, llm_mon: LLMMonitor) -> None:
    if cmd == CMD_STOP_ALL:
        print("[host] command from ESP32: STOP ALL models", flush=True)
        llm_mon.stop_all()
    elif cmd == CMD_START_FAVORITE:
        print("[host] command from ESP32: START FAVORITE model", flush=True)
        llm_mon.start_favorite()
    elif cmd == "START_MODEL" and arg:
        if ":" in arg:
            model_id, profile = arg.split(":", 1)
            print(f"[host] command from ESP32: START MODEL {model_id} (profile={profile})", flush=True)
            llm_mon.start_model(model_id.strip(), profile=profile.strip())
        else:
            print(f"[host] command from ESP32: START MODEL {arg}", flush=True)
            llm_mon.start_model(arg.strip())


def process_incoming_serial(ser: serial.Serial, rx_buf: bytes, llm_mon: LLMMonitor) -> bytes:
    try:
        n = getattr(ser, "in_waiting", 0)
        if n > 0:
            chunk = ser.read(n)
            if chunk:
                rx_buf += chunk
    except Exception as e:
        print(f"[host] serial read error: {e}", flush=True)
        return rx_buf

    while b"\n" in rx_buf:
        line_bytes, rx_buf = rx_buf.split(b"\n", 1)
        line = line_bytes.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        parsed = parse_serial_command(line)
        if parsed is not None:
            cmd, arg = parsed
            _execute_cmd(cmd, arg, llm_mon)
        elif line.startswith("s:") or line.startswith("FLUSH"):
            pass
        else:
            print(f"[device] {line}", flush=True)

    if len(rx_buf) >= 5 and rx_buf.startswith(bytes([SYNC])):
        parsed = parse_serial_command(rx_buf)
        if parsed is not None:
            cmd, arg = parsed
            _execute_cmd(cmd, arg, llm_mon)
            payload_len = rx_buf[1] | (rx_buf[2] << 8)
            frame_len = 4 + payload_len + 1
            rx_buf = rx_buf[frame_len:]

    return rx_buf


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--interval", type=float, default=0.5,
                    help="seconds between frames")
    ap.add_argument("--llm-url", default=DEFAULT_LLM_URL,
                    help="llmcontrol base url (default: http://127.0.0.1:8666)")
    args = ap.parse_args()

    ser = open_serial(args.port, args.baud)
    gpu_monitor = IntelGpuMonitor()
    llm_monitor = LLMMonitor(base_url=args.llm_url)
    io_tracker = metrics.ProcIoTracker()
    net_prev = psutil.net_io_counters()
    disk_prev = psutil.disk_io_counters()
    psutil.cpu_percent(interval=None)  # prime CPU so the first frame carries a real value
    prev_t = time.monotonic()
    rx_buf = b""

    try:
        while True:
            now_t = time.monotonic()
            elapsed = max(0.001, now_t - prev_t)
            prev_t = now_t

            # Read any commands from ESP32
            rx_buf = process_incoming_serial(ser, rx_buf, llm_monitor)

            net_now = psutil.net_io_counters()
            disk_now = psutil.disk_io_counters() or None

            snaps = {
                "cpu": metrics.cpu_snapshot(),
                "ram": metrics.ram_snapshot(),
                "gpu": metrics.gpu_snapshot(gpu_monitor),
                "net": metrics.net_snapshot(net_prev, net_now, elapsed),
                "disk": metrics.disk_snapshot(disk_prev, disk_now, elapsed),
                "header": metrics.header_snapshot(),
                "proc_cpu": metrics.proc_cpu_snapshot(),
                "proc_ram": metrics.proc_mem_snapshot(),
                "proc_gpu": metrics.proc_gpu_snapshot(gpu_monitor),
                "proc_disk": io_tracker.snapshot(elapsed),
                "llm": metrics.llm_snapshot(llm_monitor),
                "llm_models": metrics.llm_models_snapshot(llm_monitor),
            }
            net_prev = net_now
            disk_prev = disk_now

            ser.write(build_frame(snaps, args.interval))

            cpu = snaps["cpu"][0]
            ram_pct, ram_used, ram_total = snaps["ram"]
            gpu, vram_pct, vram_used, vram_total = snaps["gpu"]
            rx, tx = snaps["net"]
            rd, wr, used_pct = snaps["disk"]
            llm_st, llm_tps, llm_model = snaps["llm"]

            if llm_st == LLM_STATUS_RUNNING:
                llm_log = f"{llm_model} ({llm_tps:.1f} tps)"
            elif llm_st == LLM_STATUS_STARTING:
                llm_log = f"{llm_model} (starting)"
            elif llm_st == LLM_STATUS_IDLE:
                llm_log = "idle"
            else:
                llm_log = "off"

            print(
                f"[host] cpu={cpu:3d}% ram={ram_used:5d}/{ram_total:5d} MB "
                f"gpu={gpu:3d}% vram={vram_used:4d}/{vram_total:4d} MB "
                f"net rx={rx:5d} tx={tx:5d} KiB/s "
                f"disk rd={rd:5d} wr={wr:5d} KiB/s used={used_pct:3d}% "
                f"llm={llm_log}",
                flush=True,
            )
            time.sleep(metrics.sleep_interval(args.interval, time.monotonic() - now_t))
    except KeyboardInterrupt:
        print("\n[host] exiting", flush=True)
    finally:
        gpu_monitor.stop()
        llm_monitor.stop()
        ser.close()


if __name__ == "__main__":
    main()