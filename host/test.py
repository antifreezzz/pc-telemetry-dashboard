#!/usr/bin/env python3
"""Combined integration test: open serial, send protocol v2 batched frames,
read ESP32 debug output, all in one process.

The old firmware can't parse v2 frames, so it may fail to react; that is
fine for this script, which is meant to validate the host send path and the
ESP32 debug echo once the firmware is upgraded. This script is standalone
test code: it does not require the updated firmware to be flashed to run.
"""

import os
import sys
import threading
import time

import psutil
import serial

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from host import metrics
from host.gpu_monitor import IntelGpuMonitor
from host.protocol import (
    pack_frame, build_cpu, build_ram, build_gpu, build_net, build_disk,
    build_header, build_proc, build_llm,
    FIELD_CPU, FIELD_RAM, FIELD_GPU, FIELD_NET, FIELD_DISK, FIELD_HEADER,
    FIELD_PROC, FIELD_LLM,
    PROC_KIND_CPU, PROC_KIND_RAM, PROC_KIND_GPU,
    PROC_KIND_DISK_RD, PROC_KIND_DISK_WR,
)

PORT = "/dev/ttyUSB0"
BAUD = 115200
DURATION = 20


def send_one_frame(ser, snaps):
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
        (FIELD_LLM, build_llm(*snaps.get("llm", (0, 0.0, "")))),
    ]
    ser.write(pack_frame(fields))


def sender(ser, stop, monitor):
    """Build and send one batched v2 frame every 500ms."""
    net_prev = psutil.net_io_counters()
    disk_prev = psutil.disk_io_counters()
    io_tracker = metrics.ProcIoTracker()
    seq = 0
    while not stop.is_set():
        try:
            interval = 0.5
            net_now = psutil.net_io_counters()
            disk_now = psutil.disk_io_counters()
            snaps = {
                "cpu": metrics.cpu_snapshot(),
                "ram": metrics.ram_snapshot(),
                "gpu": metrics.gpu_snapshot(monitor),
                "net": metrics.net_snapshot(net_prev, net_now, interval),
                "disk": metrics.disk_snapshot(disk_prev, disk_now, interval),
                "header": metrics.header_snapshot(),
                "proc_cpu": metrics.proc_cpu_snapshot(),
                "proc_ram": metrics.proc_mem_snapshot(),
                "proc_gpu": metrics.proc_gpu_snapshot(monitor),
                "proc_disk": io_tracker.snapshot(interval),
            }
            send_one_frame(ser, snaps)
            net_prev = net_now
            disk_prev = disk_now

            seq += 1
            cpu = snaps["cpu"][0]
            rx, tx = snaps["net"]
            print(f"[tx] seq={seq} cpu={cpu} rx={rx} tx={tx}", flush=True)
        except Exception as e:
            print(f"[tx] error: {e}", flush=True)
            break
        time.sleep(0.5)


def reader(ser, stop):
    """Read ESP32 output, print non-empty lines."""
    buf = b""
    while not stop.is_set():
        try:
            data = ser.read(200)
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    print(f"[rx] {line.decode('ascii', errors='replace')}", flush=True)
        except Exception as e:
            print(f"[rx] error: {e}", flush=True)
            break


def main():
    s = serial.Serial(PORT, BAUD, timeout=1, rtscts=False, xonxoff=False, dsrdtr=False)
    s.dtr = False
    s.rts = False
    print(f"[main] opened {PORT} @ {BAUD}", flush=True)

    monitor = IntelGpuMonitor()
    stop = threading.Event()
    t_send = threading.Thread(target=sender, args=(s, stop, monitor), daemon=True)
    t_read = threading.Thread(target=reader, args=(s, stop), daemon=True)
    t_send.start()
    t_read.start()

    time.sleep(DURATION)
    stop.set()
    time.sleep(0.5)
    monitor.stop()
    s.close()
    print("[main] done", flush=True)


if __name__ == "__main__":
    main()