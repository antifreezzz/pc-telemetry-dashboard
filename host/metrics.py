#!/usr/bin/env python3
"""Pure metric collection (no serial dependency)."""

import os
import time

import psutil


def _mb(value: int) -> int:
    return int(value // (1024 * 1024))


def cpu_snapshot() -> tuple:
    """Return (overall pct, per-core list)."""
    overall = int(psutil.cpu_percent(interval=None))
    cores = [int(c) for c in psutil.cpu_percent(interval=None, percpu=True)]
    return overall, cores


def ram_snapshot() -> tuple:
    mem = psutil.virtual_memory()
    return int(mem.percent), _mb(mem.used), _mb(mem.total)


def gpu_snapshot(monitor) -> tuple:
    """Return GPU pct/vram data from the background monitor object."""
    if monitor is None:
        return -1, 0, 0, 0
    try:
        return monitor.snapshot()
    except Exception:
        return -1, 0, 0, 0


def net_snapshot(prev, now, interval: float) -> tuple:
    """KiB/s rates from two psutil.net_io_counters snapshots."""
    return (
        int(max(0, (now.bytes_recv - prev.bytes_recv) / interval / 1024)),
        int(max(0, (now.bytes_sent - prev.bytes_sent) / interval / 1024)),
    )


def disk_snapshot(prev, now, interval: float) -> tuple:
    """rd/wr KiB/s (aggregate) and root filesystem used pct."""
    rd = 0
    wr = 0
    if prev is not None and now is not None and now.read_bytes >= 0:
        rd = int(max(0, (now.read_bytes - prev.read_bytes) / interval / 1024))
        wr = int(max(0, (now.write_bytes - prev.write_bytes) / interval / 1024))
    used_pct = int(psutil.disk_usage("/").percent)
    return rd, wr, used_pct


def header_snapshot() -> tuple:
    uptime_s = int(time.time() - psutil.boot_time())
    epoch_s = int(time.time())
    hostname = os.uname().nodename
    return uptime_s, epoch_s, hostname


def proc_snapshot(n: int = 6) -> list:
    """Top-n processes by recent CPU%, sorted by memory as a fallback."""
    procs = []
    for p in psutil.process_iter(["cpu_percent", "memory_percent", "pid", "name"]):
        try:
            p.cpu_percent(interval=None)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    time.sleep(0.05)
    for p in psutil.process_iter(["cpu_percent", "memory_percent", "pid", "name"]):
        try:
            info = p.info
            pid = info.get("pid") or 0
            name = str(info.get("name") or "?")
            cpu = int(info.get("cpu_percent") or 0)
            mem = info.get("memory_percent") or 0
            procs.append((cpu, mem, pid, name))
        except (psutil.NoSuchProcess, psutil.AccessDenied, TypeError):
            continue
    procs.sort(key=lambda t: (t[0], t[1]), reverse=True)
    out = []
    for cpu, _mem, pid, name in procs[:n]:
        out.append((cpu, pid, name))
    return out