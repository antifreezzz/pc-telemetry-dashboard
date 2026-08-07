#!/usr/bin/env python3
"""Intel dGPU (i915) VRAM + busy collector via DRM fdinfo — no root needed.

On a discrete Intel GPU every DRM client writes per-fd accounting into
/proc/<pid>/fdinfo/<fd>:

    drm-pdev:          0000:03:00.0
    drm-client-id:     35
    drm-total-local0:  35652 KiB
    drm-resident-local0: 35652 KiB        <- resident device-local (VRAM) bytes
    drm-engine-render: 573979900 ns       <- cumulative engine busy time

The same client reports identical numbers on all its fds, so we dedup by
(pid, drm-client-id) taking the max. VRAM used = sum of resident-local0 over
unique clients; VRAM total = the device's prefetchable PCI MEM BAR. GPU busy
= wall-clock normalized delta of summed drm-engine-render + drm-engine-compute
(the Render/3D class, matching intel_gpu_top's Render/3D).

This works without CAP_PERFMON (unlike intel_gpu_top) on kernels that expose
the engine timestamps in fdinfo. No data -> (-1, 0, 0, 0).
"""

import os
import threading
import time

_ENGINES_BUSY = ("render", "compute")
_DEAD_BAND = 5.0  # show 0% below this busy to hide desktop-compositor idle churn
_UNITS = {"B": 1, "KiB": 1024, "MiB": 1024 ** 2, "GiB": 1024 ** 3, "TiB": 1024 ** 4}


def parse_fdinfo(text: str) -> dict:
    d = {}
    for line in text.splitlines():
        k, sep, v = line.partition(":")
        if sep:
            d[k.strip()] = v.strip()
    return d


def to_bytes(val: str) -> int:
    if not val:
        return 0
    parts = val.split()
    try:
        n = float(parts[0])
    except ValueError:
        return 0
    unit = parts[1] if len(parts) > 1 else "B"
    return int(n * _UNITS.get(unit, 1))


def to_ns(val: str) -> int:
    """Cumulative engine counter like '573979900 ns' -> ns int."""
    if not val:
        return 0
    parts = val.split()
    try:
        return int(float(parts[0]))
    except ValueError:
        return 0


def _parse_resource_text(text: str) -> int:
    """Prefetchable (IORESOURCE_MEM 0x200) PCI MEM BAR size in bytes."""
    best = 0
    for line in text.splitlines():
        cols = line.split()
        if len(cols) < 3:
            continue
        start = int(cols[0], 16)
        end = int(cols[1], 16)
        flags = int(cols[2], 16)
        if flags & 0x200 and end > start:  # IORESOURCE_MEM
            best = max(best, end - start + 1)
    return best


def _pci_mem_total(pdev: str) -> int:
    """Prefetchable PCI MEM BAR size = device VRAM capacity (bytes)."""
    try:
        with open(f"/sys/bus/pci/devices/{pdev}/resource") as f:
            return _parse_resource_text(f.read())
    except OSError:
        return 0


def scan_drm() -> tuple:
    """Return (clients, pdev).

    clients: dict (pid, drm-client-id) -> {res: int bytes, eng: {engine: ns}}
    pdev: first seen drm-pdev string or None.
    """
    clients: dict = {}
    pdev = None
    for pid_name in os.listdir("/proc"):
        if not pid_name.isdigit():
            continue
        fddir = f"/proc/{pid_name}/fdinfo"
        try:
            fds = os.listdir(fddir)
        except OSError:
            continue
        for fd in fds:
            try:
                target = os.readlink(f"/proc/{pid_name}/fd/{fd}")
            except OSError:
                continue
            if not target.startswith("/dev/dri/"):
                continue
            try:
                with open(f"{fddir}/{fd}") as f:
                    info = parse_fdinfo(f.read())
            except OSError:
                continue
            cid = info.get("drm-client-id")
            if cid is None:
                continue
            pdev = info.get("drm-pdev") or pdev
            key = (int(pid_name), cid)
            prev = clients.get(key)
            res = to_bytes(info.get("drm-resident-local0", "0"))
            eng = {
                name: to_ns(info.get(f"drm-engine-{name}", "0"))
                for name in _ENGINES_BUSY
            }
            if prev is None:
                clients[key] = {"res": res, "eng": eng}
            else:
                prev["res"] = max(prev["res"], res)
                for name in _ENGINES_BUSY:
                    prev["eng"][name] = max(prev["eng"][name], eng[name])
    return clients, pdev


class IntelGpuMonitor:
    """Background thread sampling DRM fdinfo every ~0.5s."""

    def __init__(self, interval: float = 0.5) -> None:
        self._interval = interval
        self._lock = threading.Lock()
        self._pct = -1
        self._vram_used = 0
        self._vram_total = 0
        self._prev_eng = None
        self._prev_wall = None
        self._total_cached = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _clamp(self, v: float) -> int:
        if v < _DEAD_BAND:
            return 0  # hide compositor idle -> a removed load snaps straight to 0
        if v > 100:
            return 100
        return int(v)

    def _set(self, pct: int, used: int, total: int) -> None:
        with self._lock:
            self._pct = pct
            self._vram_used = used
            self._vram_total = total

    def snapshot(self) -> tuple:
        """(busy_pct, vram_pct, vram_used_mb, vram_total_mb); (-1,0,0,0) = no GPU."""
        with self._lock:
            pct = self._pct
            used = self._vram_used
            total = self._vram_total
        if pct < 0 or total <= 0:
            return pct, 0, used, total
        return pct, self._clamp(round(used / total * 100)), used, total

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)

    def _run(self) -> None:
        while not self._stop.is_set():
            now = time.monotonic()
            clients, pdev = scan_drm()
            total = self._total_cached
            if total <= 0 and pdev:
                total = _pci_mem_total(pdev)
                if total > 0:
                    self._total_cached = total

            used_mb = sum(c["res"] for c in clients.values()) // (1024 * 1024)

            eng_sum = sum(c["eng"][e] for c in clients.values() for e in _ENGINES_BUSY)
            pct = self._pct
            if self._prev_eng is not None:
                wall = now - self._prev_wall
                if wall > 0:
                    d_ns = max(0, eng_sum - self._prev_eng)
                    busy = d_ns / (wall * 1e9) * 100.0
                    pct = self._clamp(busy)
            self._prev_eng = eng_sum
            self._prev_wall = now

            self._set(pct, used_mb, total // (1024 * 1024))
            self._stop.wait(self._interval)
