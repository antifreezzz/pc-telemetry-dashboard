#!/usr/bin/env python3
"""Durable intel_gpu_top collector.

Runs one persistent `intel_gpu_top -J -s 500` subprocess in a background
thread and continuously parses its JSON stream, exposing the latest
Render/3D busy% and dedicated (local) VRAM usage. intel_gpu_top needs
CAP_PERFMON; without it the subprocess exits immediately and the monitor
exposes "no data" (-1, 0, 0, 0) and auto-restarts with a backoff.

intel_gpu_top streams one JSON object per sample line, roughly:

    {"busy": 12.5, "engine-classes": [{"name": "Render/3D", "busy": 43.7}],
     "memory": {"region": [{"name": "local", "used": N, "total": M}]}}

Some builds nest engines under "engines"/{"class":[...]}, and the device
list may be printed at startup. We parse defensively and keep the last
good values on any failure.
"""

import json
import subprocess
import threading


def _clamp(v: int) -> int:
    if v < 0:
        return 0
    return min(100, v)


def _is_render(name: str) -> bool:
    lowered = name.lower()
    return "render" in lowered or "3d" in lowered


def _engines(sample: dict) -> list:
    for key in ("engine-classes", "engines"):
        val = sample.get(key)
        if isinstance(val, dict) and isinstance(val.get("class"), list):
            return [e for e in val["class"] if isinstance(e, dict)]
        if isinstance(val, list):
            return [e for e in val if isinstance(e, dict)]
    return []


def _busy_of(sample: dict) -> int:
    """Best busy pct for the sample: prefer Render/3D, else first engine,
    else the top-level 'busy'. -1 when nothing is readable."""
    engines = _engines(sample)
    for eng in engines:
        busy = eng.get("busy")
        if isinstance(busy, (int, float)) and _is_render(str(eng.get("name") or "")):
            return _clamp(int(busy))
    for eng in engines:
        busy = eng.get("busy")
        if isinstance(busy, (int, float)):
            return _clamp(int(busy))
    top_busy = sample.get("busy")
    if isinstance(top_busy, (int, float)):
        return _clamp(int(top_busy))
    return -1


def _parse_sample(sample: dict, prev_pct: int, prev_used: int, prev_total: int) -> tuple:
    """Return (pct, vram_used_mb, vram_total_mb); fall back to previous
    values on any parse failure."""
    busy = _busy_of(sample)
    if busy < 0:
        pct = prev_pct
    else:
        pct = busy
    used = prev_used
    total = prev_total
    memory = sample.get("memory")
    if isinstance(memory, dict) and isinstance(memory.get("region"), list):
        regions = [r for r in memory["region"] if isinstance(r, dict)]
        chosen = next((r for r in regions if str(r.get("name") or "").lower() == "local"), None)
        if chosen is None and regions:
            chosen = regions[0]
        if chosen is not None:
            used = int(chosen.get("used") or 0) // (1024 * 1024)
            total = int(chosen.get("total") or 0) // (1024 * 1024)
    return pct, used, total


class IntelGpuMonitor:
    """Background thread running one persistent intel_gpu_top subprocess."""

    def __init__(self, device: int = 0) -> None:
        self._device = device
        self._lock = threading.Lock()
        self._pct = -1
        self._vram_used = 0
        self._vram_total = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _set(self, pct: int, used: int, total: int) -> None:
        with self._lock:
            self._pct = pct
            self._vram_used = used
            self._vram_total = total

    def snapshot(self) -> tuple:
        """Return (pct, vram_pct, vram_used_mb, vram_total_mb), or
        (-1, 0, 0, 0) when no sample is available yet or the process died."""
        with self._lock:
            pct = self._pct
            used = self._vram_used
            total = self._vram_total
        if pct < 0 or total <= 0:
            return pct, 0, used, total
        vram_pct = _clamp(round(used / total * 100))
        return _clamp(pct), vram_pct, used, total

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)

    def _run(self) -> None:
        backoff = 2.0
        while not self._stop.is_set():
            cmd = ["intel_gpu_top", "-J", "-s", "500"]
            if self._device:
                cmd += ["-d", self._device]
            try:
                proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
                )
            except (FileNotFoundError, OSError):
                self._stop.wait(backoff)
                backoff = min(30.0, backoff * 2.5)
                continue
            try:
                for raw in proc.stdout:
                    if self._stop.is_set():
                        break
                    line = raw.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    try:
                        sample = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(sample, dict):
                        continue
                    pct, used, total = _parse_sample(
                        sample, self._pct, self._vram_used, self._vram_total
                    )
                    self._set(pct, used, total)
            finally:
                try:
                    proc.kill()
                except OSError:
                    pass
                try:
                    proc.wait()
                except subprocess.SubprocessError:
                    pass
            self._stop.wait(backoff)
            backoff = min(30.0, backoff * 2.5)