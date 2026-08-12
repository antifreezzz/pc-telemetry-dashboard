#!/usr/bin/env python3
"""Pure metric collection (no serial dependency)."""

import os
import time

import psutil


def _mb(value: int) -> int:
    return int(value // (1024 * 1024))


def sleep_interval(interval: float, elapsed: float) -> float:
    """Seconds to sleep to keep a fixed `interval` cadence.

    `elapsed` is the time already spent collecting this frame's metrics. The
    returned value is never negative and never exceeds `interval`, so the loop
    cannot drift into progressively longer sleeps.
    """
    if interval <= 0:
        return 0.0
    return max(0.0, interval - elapsed)


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


def proc_cpu_snapshot(n: int = 10) -> list:
    """Top-n processes by recent CPU%, sorted by memory as a fallback."""
    procs = list(psutil.process_iter(["pid", "name"]))
    for p in procs:
        try:
            p.cpu_percent(interval=None)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    time.sleep(0.05)
    entries = []
    for p in procs:
        try:
            cpu = int(p.cpu_percent(interval=None))
            mem = p.memory_percent() or 0.0
            pid = p.pid or 0
            name = str(p.info.get("name") or "?")
            entries.append((cpu, mem, pid, name))
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue
    entries.sort(key=lambda t: (t[0], t[1]), reverse=True)
    out = []
    for cpu, _mem, pid, name in entries[:n]:
        out.append((cpu, pid, name))
    return out


def _comm(pid: int) -> str:
    """Short process name from /proc/<pid>/comm (independent of psutil)."""
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()[:15] or "?"
    except OSError:
        return "?"


def proc_mem_snapshot(n: int = 10) -> list:
    """Top-n processes by resident memory (RSS), value in MB, descending."""
    procs = list(psutil.process_iter(["pid", "name"]))
    entries = []
    for p in procs:
        try:
            mem = p.memory_info().rss
            pid = p.pid or 0
            name = str(p.info.get("name") or "?")
            entries.append((mem, pid, name))
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue
    entries.sort(key=lambda t: t[0], reverse=True)
    return [(mem // (1024 * 1024), pid, name) for mem, pid, name in entries[:n]]


def proc_gpu_snapshot(monitor, n: int = 10) -> list:
    """Top-n DRM clients by resident VRAM, value in MB, descending."""
    if monitor is None:
        return []
    try:
        clients = monitor.top_clients(n)
    except Exception:
        return []
    return [(res // (1024 * 1024), pid, _comm(pid)) for pid, res in clients]


class ProcIoTracker:
    """Per-pid cumulative io counters turned into rd/wr KiB/s deltas.

    First call only caches the counters (rates need a baseline); every
    subsequent call reports KiB/s for processes present in the previous frame.
    """

    def __init__(self) -> None:
        self._prev: dict = {}

    def snapshot(self, interval: float, n: int = 10) -> list:
        """[(rd_kibs, wr_kibs, pid, name)] sorted by rd+wr descending."""
        now: dict = {}
        raw = []
        procs = list(psutil.process_iter(["pid", "name"]))
        for p in procs:
            try:
                io = p.io_counters()
                pid = p.pid or 0
                now[pid] = (io.read_bytes, io.write_bytes)
                prev = self._prev.get(pid)
                if prev is not None and interval > 0:
                    rd = max(0, (io.read_bytes - prev[0]) / interval / 1024)
                    wr = max(0, (io.write_bytes - prev[1]) / interval / 1024)
                    name = str(p.info.get("name") or "?")
                    raw.append((rd + wr, int(rd), int(wr), pid, name))
            except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
                continue
        self._prev = now
        raw.sort(key=lambda t: t[0], reverse=True)
        return [(rd, wr, pid, name) for _tot, rd, wr, pid, name in raw[:n]]
