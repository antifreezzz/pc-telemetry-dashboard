#!/usr/bin/env python3
"""Unit tests for host protocol v2.

Run: python3 host/test_unit.py
"""

import os
import struct
import sys
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from host import metrics
from host.metrics import sleep_interval
from host.gpu_monitor import (
    parse_fdinfo,
    to_bytes,
    to_ns,
    scan_drm,
    _parse_resource_text,
    IntelGpuMonitor,
)
from host.protocol import net_kbps, pack_net_kbps


class TestGpuFdinfo(unittest.TestCase):
    SAMPLE = (
        "pos:\t0\n"
        "flags:\t0100002\n"
        "drm-pdev:\t0000:03:00.0\n"
        "drm-client-id:\t35\n"
        "drm-total-local0:\t35652 KiB\n"
        "drm-resident-local0:\t35652 KiB\n"
        "drm-active-local0:\t0 B\n"
        "drm-engine-render:\t573979900 ns\n"
        "drm-engine-compute:\t10047118432 ns\n"
    )

    def test_parse_fdinfo(self):
        d = parse_fdinfo(self.SAMPLE)
        self.assertEqual(d["drm-pdev"], "0000:03:00.0")
        self.assertEqual(d["drm-client-id"], "35")
        self.assertEqual(d["drm-engine-render"], "573979900 ns")

    def test_to_bytes(self):
        self.assertEqual(to_bytes("0 B"), 0)
        self.assertEqual(to_bytes("1024 B"), 1024)
        self.assertEqual(to_bytes("35652 KiB"), 35652 * 1024)
        self.assertEqual(to_bytes("1.5 MiB"), int(1.5 * 1024 * 1024))
        self.assertEqual(to_bytes("2 GiB"), 2 * 1024 ** 3)

    def test_to_ns(self):
        self.assertEqual(to_ns("573979900 ns"), 573979900)
        self.assertEqual(to_ns("0"), 0)
        self.assertEqual(to_ns(""), 0)

    def test_scan_is_thread_safe_smoke(self):
        mon = IntelGpuMonitor(interval=0.1)
        try:
            snap = mon.snapshot()
            self.assertEqual(len(snap), 4)
        finally:
            mon.stop()

    def test_pci_mem_total_parses(self):
        # 16 GiB (0x400000000) prefetchable IORESOURCE_MEM |0|v mostly
        txt = ("0000000000000000 0000000000000000 0x200 00000000 00000000\n"
               "0000000000000000 00000003ffffffff 0x121c2200 00000000 00000000\n")
        self.assertEqual(_parse_resource_text(txt), 16 * 1024 ** 3)
from host.protocol import (
    SYNC,
    FRAME_TYPE,
    FRAME_TYPE_CMD,
    FIELD_CPU,
    FIELD_RAM,
    FIELD_GPU,
    FIELD_NET,
    FIELD_DISK,
    FIELD_HEADER,
    FIELD_PROC,
    FIELD_LLM,
    PROC_KIND_CPU,
    PROC_KIND_RAM,
    PROC_KIND_GPU,
    PROC_KIND_DISK_RD,
    PROC_KIND_DISK_WR,
    LLM_STATUS_IDLE,
    LLM_STATUS_RUNNING,
    LLM_STATUS_STARTING,
    LLM_STATUS_PROMPT_EVAL,
    LLM_STATUS_GENERATING,
    LLM_STATUS_OFFLINE,
    CMD_STOP_ALL,
    CMD_START_FAVORITE,
    crc8,
    pack_frame,
    pack_kiBs,
    pack_pct,
    build_cpu,
    build_ram,
    build_gpu,
    build_net,
    build_disk,
    build_header,
    build_proc,
    build_llm,
    build_cmd_frame,
    parse_serial_command,
)
from host.llm_client import LLMControlClient, LLMMonitor, _status_str_to_code

PROC_ENTRY_BYTES = 22


class TestNetKbps(unittest.TestCase):
    def test_normal(self):
        self.assertEqual(net_kbps(1024 * 1024, 1.0), 1024)

    def test_zero_delta(self):
        self.assertEqual(net_kbps(0, 0.5), 0)

    def test_negative_delta_clamps_to_zero(self):
        self.assertEqual(net_kbps(-512, 0.5), 0)

    def test_counter_reset_clamps(self):
        self.assertEqual(net_kbps(-12345678, 0.5), 0)

    def test_fraction_truncates(self):
        self.assertEqual(net_kbps(512, 1.0), 0)
        self.assertEqual(net_kbps(1536, 1.0), 1)

    def test_pack_never_crashes_struct(self):
        for delta in (-100, 0, 100):
            rx, tx = pack_net_kbps(delta, 0.5, delta, 0.5)
            pkt = struct.pack("<II", rx, tx)
            self.assertEqual(len(pkt), 8)


class TestCrc8(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(crc8(b""), 0)

    def test_zero_byte(self):
        self.assertEqual(crc8(b"\x00"), 0)

    def test_check_value(self):
        self.assertEqual(crc8(b"123456789"), 0xF4)

    def test_type_only(self):
        self.assertEqual(crc8(bytes([FRAME_TYPE])), 0xD9)


class TestPackKiBs(unittest.TestCase):
    def test_rate(self):
        self.assertEqual(pack_kiBs(1024 * 1024, 1.0), 1024)

    def test_negative_clamps(self):
        self.assertEqual(pack_kiBs(-500, 1.0), 0)

    def test_zero_interval(self):
        self.assertEqual(pack_kiBs(1024, 0.0), 0)


class TestPackPct(unittest.TestCase):
    def test_clamps(self):
        self.assertEqual(pack_pct(-5), 255)
        self.assertEqual(pack_pct(0), 0)
        self.assertEqual(pack_pct(50), 50)
        self.assertEqual(pack_pct(150), 100)

    def test_custom_na(self):
        self.assertEqual(pack_pct(-1, n_na=0), 0)


class TestBuildHelpers(unittest.TestCase):
    def test_cpu_length(self):
        data = build_cpu(42, [10, 20, 30, 40])
        self.assertEqual(len(data), 1 + 4)
        self.assertEqual(data[0], 42)
        self.assertEqual(list(data[1:]), [10, 20, 30, 40])

    def test_cpu_clamps(self):
        data = build_cpu(-1, [200, -7])
        self.assertEqual(data[0], 255)
        self.assertEqual(list(data[1:]), [100, 0])

    def test_ram_length_and_layout(self):
        data = build_ram(77, 2048, 8192)
        self.assertEqual(len(data), 9)
        self.assertEqual(struct.unpack("<BII", data), (77, 2048, 8192))

    def test_ram_clamps(self):
        data = build_ram(150, -5, 1 << 40)
        self.assertEqual(struct.unpack("<BII", data), (100, 0, 0xFFFFFFFF))

    def test_gpu_length(self):
        data = build_gpu(60, 50, 4096, 8192)
        self.assertEqual(len(data), 10)
        self.assertEqual(struct.unpack("<BBII", data), (60, 50, 4096, 8192))

    def test_gpu_na(self):
        data = build_gpu(-1, -1, -1, -1)
        self.assertEqual(len(data), 10)
        self.assertEqual(struct.unpack("<BBII", data), (255, 0, 0, 0))

    def test_net_length(self):
        data = build_net(1024, 512)
        self.assertEqual(len(data), 8)
        self.assertEqual(struct.unpack("<II", data), (1024, 512))

    def test_disk_length(self):
        data = build_disk(10, 20, 55)
        self.assertEqual(len(data), 9)
        self.assertEqual(struct.unpack("<IIB", data), (10, 20, 55))

    def test_header_layout(self):
        data = build_header(123, 456, "mybox")
        self.assertEqual(len(data), 32)
        up, ep = struct.unpack_from("<II", data, 0)
        self.assertEqual((up, ep), (123, 456))
        self.assertEqual(data[8:].rstrip(b"\x00"), b"mybox")
        self.assertEqual(data[8:], b"mybox" + b"\x00" * 19)

    def test_header_long_hostname_truncated(self):
        data = build_header(0, 0, "x" * 100)
        name = data[8:].rstrip(b"\x00")
        self.assertLessEqual(len(name), 23)

    def test_proc_layout(self):
        entries = [(10, 1234, "foo"), (20, 5678, "bar")]
        data = build_proc(PROC_KIND_RAM, entries)
        self.assertEqual(len(data), 2 + 2 * PROC_ENTRY_BYTES)
        self.assertEqual(data[0], PROC_KIND_RAM)
        self.assertEqual(data[1], 2)
        off = 2
        for value, pid, name in entries:
            v, p, pname = struct.unpack_from("<I H 16s", data, off)
            self.assertEqual(v, value)
            self.assertEqual(p, pid)
            self.assertEqual(pname.rstrip(b"\x00").decode(), name)
            off += PROC_ENTRY_BYTES

    def test_proc_kinds_encoded(self):
        for kind in (PROC_KIND_CPU, PROC_KIND_RAM, PROC_KIND_GPU,
                     PROC_KIND_DISK_RD, PROC_KIND_DISK_WR):
            data = build_proc(kind, [(7, 42, "x")])
            self.assertEqual(data[0], kind)

    def test_proc_long_name(self):
        data = build_proc(PROC_KIND_CPU, [(5, 1, "y" * 40)])
        self.assertEqual(len(data[2:]), PROC_ENTRY_BYTES)
        v, p, name = struct.unpack_from("<I H 16s", data, 2)
        self.assertLessEqual(len(name.rstrip(b"\x00").decode()), 15)

    def test_proc_value_and_pid_clamp(self):
        data = build_proc(PROC_KIND_CPU, [(1 << 40, 1 << 20, "p")])
        v, p, _ = struct.unpack_from("<I H 16s", data, 2)
        self.assertEqual(v, 0xFFFFFFFF)
        self.assertEqual(p, 0xFFFF)


def decode_frame(frame: bytes) -> list:
    """Minimal TLV decoder mirroring the spec, used for round-trip checks."""
    assert frame[0] == SYNC
    length = frame[1] | (frame[2] << 8)
    assert frame[3] == FRAME_TYPE
    assert frame[4 + length] == crc8(frame[3 : 4 + length])
    payload = frame[4 : 4 + length]
    fields = []
    pos = 0
    while pos < len(payload):
        field_id = payload[pos]
        field_len = payload[pos + 1]
        fields.append((field_id, payload[pos + 2 : pos + 2 + field_len]))
        pos += 2 + field_len
    return fields


def field_data(decoded: list, field_id: int) -> list:
    """All data blobs for a given field id (a frame may repeat a field id)."""
    return [data for fid, data in decoded if fid == field_id]


class TestPackFrame(unittest.TestCase):
    def test_frame_header(self):
        frame = pack_frame([(FIELD_CPU, bytes([50]))])
        self.assertEqual(frame[0], 0xAA)
        self.assertEqual(frame[1], 3)
        self.assertEqual(frame[2], 0)
        self.assertEqual(frame[3], 0xF1)

    def test_frame_crc_last_byte(self):
        fields = [
            (FIELD_CPU, build_cpu(30, [10, 20])),
            (FIELD_RAM, build_ram(60, 1024, 8192)),
        ]
        frame = pack_frame(fields)
        length = frame[1] | (frame[2] << 8)
        self.assertEqual(frame[-1], crc8(frame[3:-1]))
        self.assertEqual(len(frame), 4 + length + 1)

    def test_full_frame_round_trip(self):
        fields = [
            (FIELD_CPU, build_cpu(44, [10, 20, 30, 40, 50, 60, 70, 80])),
            (FIELD_RAM, build_ram(61, 2048, 8192)),
            (FIELD_GPU, build_gpu(70, 33, 2048, 8192)),
            (FIELD_NET, build_net(1234, 4321)),
            (FIELD_DISK, build_disk(11, 22, 55)),
            (FIELD_HEADER, build_header(99, 1000, "host-test")),
            (FIELD_PROC, build_proc(PROC_KIND_CPU, [(9, 1, "a")])),
            (FIELD_PROC, build_proc(PROC_KIND_RAM, [(8, 2, "bb")])),
            (FIELD_PROC, build_proc(PROC_KIND_GPU, [(7, 3, "ccc")])),
            (FIELD_PROC, build_proc(PROC_KIND_DISK_RD, [(6, 4, "dd")])),
            (FIELD_PROC, build_proc(PROC_KIND_DISK_WR, [(5, 5, "e")])),
        ]
        frame = pack_frame(fields)
        decoded = decode_frame(frame)
        self.assertEqual(len(decoded), len(fields))
        proc_blobs = field_data(decoded, FIELD_PROC)
        self.assertEqual(len(proc_blobs), 5)
        self.assertEqual([b[0] for b in proc_blobs],
                         [PROC_KIND_CPU, PROC_KIND_RAM, PROC_KIND_GPU,
                          PROC_KIND_DISK_RD, PROC_KIND_DISK_WR])
        self.assertEqual([len(b) for b in proc_blobs],
                         [2 + PROC_ENTRY_BYTES] * 5)
        self.assertEqual(field_data(decoded, FIELD_RAM),
                         [struct.pack("<BII", 61, 2048, 8192)])
        self.assertEqual(field_data(decoded, FIELD_GPU),
                         [struct.pack("<BBII", 70, 33, 2048, 8192)])
        self.assertEqual(len(field_data(decoded, FIELD_HEADER)[0]), 32)

    def test_unknown_field_skippable(self):
        frame = pack_frame([(0x7F, b"\x01\x02\x03"), (FIELD_CPU, bytes([5]))])
        decoded = decode_frame(frame)
        self.assertEqual(len(decoded), 2)
        self.assertEqual(decoded[1], (FIELD_CPU, b"\x05"))


class TestSleepInterval(unittest.TestCase):
    def test_full_elapsed_returns_zero(self):
        self.assertEqual(sleep_interval(0.5, 0.5), 0.0)

    def test_no_elapsed_returns_full_interval(self):
        self.assertEqual(sleep_interval(0.5, 0.0), 0.5)

    def test_elapsed_exceeding_interval_clamps_to_zero(self):
        self.assertEqual(sleep_interval(0.5, 2.0), 0.0)

    def test_never_exceeds_interval(self):
        for elapsed in (0.0, 0.001, 0.2, 0.5, 1.0, 100.0):
            self.assertLessEqual(sleep_interval(0.5, elapsed), 0.5)

    def test_fixed_cadence_stays_linear(self):
        # Simulate a fixed-cadence loop: N frames must take ~N*interval, not
        # quadratic like the old bug (sleeping for the whole previous interval).
        interval = 0.5
        work = 0.15
        total = 0.0
        n = 100
        for _ in range(n):
            total += work
            total += sleep_interval(interval, work)
        self.assertAlmostEqual(total, n * interval, delta=n * 0.05)


class _FakeProc:
    """Mimics psutil.Process.cpu_percent: 0.0 on the priming call, real value after."""

    def __init__(self, pid, name, cpu_second):
        self.pid = pid
        self.info = {"pid": pid, "name": name}
        self._cpu_second = cpu_second
        self._primed = False

    def cpu_percent(self, interval=None):
        if not self._primed:
            self._primed = True
            return 0.0
        return float(self._cpu_second)

    def memory_percent(self):
        return 1.0


class TestProcSnapshot(unittest.TestCase):
    def _fake_iter(self, attrs=None):
        return [_FakeProc(100 + i, f"proc{i}", (i + 1) * 10) for i in range(8)]

    def test_returns_primed_cpu_not_zero(self):
        with mock.patch.object(metrics.psutil, "process_iter", side_effect=self._fake_iter), \
             mock.patch.object(metrics.time, "sleep"):
            out = metrics.proc_cpu_snapshot(3)
        # second call on the SAME objects yields 80/70/60, sorted descending
        self.assertEqual(out, [(80, 107, "proc7"), (70, 106, "proc6"), (60, 105, "proc5")])


class _FakeMemInfo:
    def __init__(self, rss):
        self.rss = rss


class _FakeMemProc:
    def __init__(self, pid, name, rss_bytes):
        self.pid = pid
        self.info = {"pid": pid, "name": name}
        self._rss = rss_bytes

    def memory_info(self):
        return _FakeMemInfo(self._rss)


class TestProcMemSnapshot(unittest.TestCase):
    def _fake_iter(self, attrs=None):
        return [_FakeMemProc(100 + i, f"p{i}", (i + 1) * (4 * 1024 * 1024))
                for i in range(8)]

    def test_sorted_by_rss_desc_in_mb(self):
        with mock.patch.object(metrics.psutil, "process_iter", side_effect=self._fake_iter):
            out = metrics.proc_mem_snapshot(3)
        self.assertEqual(out, [(32, 107, "p7"), (28, 106, "p6"), (24, 105, "p5")])


class _FakeIo:
    def __init__(self, read_bytes, write_bytes):
        self.read_bytes = read_bytes
        self.write_bytes = write_bytes


class _FakeIoProc:
    def __init__(self, pid, name, read_bytes, write_bytes):
        self.pid = pid
        self.info = {"pid": pid, "name": name}
        self._rd = read_bytes
        self._wr = write_bytes

    def io_counters(self):
        return _FakeIo(self._rd, self._wr)


class TestProcDiskSnapshot(unittest.TestCase):
    def _run(self, calls):
        tracker = metrics.ProcIoTracker()
        procs_by_call = [
            [_FakeIoProc(100 + i, f"p{i}", c[0], c[1])
             for i, c in enumerate(counters)]
            for counters in calls
        ]

        def fake_iter(attrs=None):
            return procs_by_call.pop(0)

        results = []
        with mock.patch.object(metrics.psutil, "process_iter", side_effect=fake_iter):
            for _ in range(len(calls)):
                results.append(tracker.snapshot(0.5))
        return results

    def test_first_call_empty_then_rates_sorted_by_total(self):
        calls = [
            [(1000, 2000), (4000, 3000), (0, 0)],
            [(1000 + 2048, 2000 + 1024), (4000 + 1024 * 512, 3000), (0, 0)],
        ]
        first, second = self._run(calls)
        self.assertEqual(first, [])
        # p1: rd 1024 KiB/s, wr 0 -> total 1024; p0: rd 4, wr 2 -> total 6
        self.assertEqual(second[0], (1024, 0, 101, "p1"))
        self.assertEqual(second[1], (4, 2, 100, "p0"))
        self.assertEqual(second[2], (0, 0, 102, "p2"))


class _FakeGpuMonitor:
    def __init__(self, clients):
        self._clients = clients

    def top_clients(self, n):
        return sorted(self._clients.items(), key=lambda kv: kv[1], reverse=True)[:n]


class TestProcGpuSnapshot(unittest.TestCase):
    def test_none_monitor_returns_empty(self):
        self.assertEqual(metrics.proc_gpu_snapshot(None, 10), [])

    def test_returns_mb_pid_name(self):
        mon = _FakeGpuMonitor({100: 2 * 1024 * 1024, 101: 6 * 1024 * 1024})
        with mock.patch("host.metrics._comm", return_value="gproc"):
            out = metrics.proc_gpu_snapshot(mon, 10)
        self.assertEqual(out, [(6, 101, "gproc"), (2, 100, "gproc")])


class TestGpuMonitorPriming(unittest.TestCase):
    _CLIENTS = {(123, "1"): {"res": 100 * 1024 * 1024,
                             "eng": {"render": 1000, "compute": 0}}}

    def test_first_snapshot_is_idle_zero_not_na(self):
        with mock.patch("host.gpu_monitor.scan_drm", return_value=(dict(self._CLIENTS), "0000:03:00.0")), \
             mock.patch("host.gpu_monitor._pci_mem_total", return_value=16 * 1024 ** 3):
            mon = IntelGpuMonitor(interval=0.1)
            try:
                pct, _vram_pct, _used, total = mon.snapshot()
            finally:
                mon.stop()
        self.assertEqual(pct, 0)
        self.assertEqual(total, 16 * 1024)

    def test_no_gpu_stays_na(self):
        with mock.patch("host.gpu_monitor.scan_drm", return_value=({}, None)), \
             mock.patch("host.gpu_monitor._pci_mem_total", return_value=0):
            mon = IntelGpuMonitor(interval=0.1)
            try:
                pct, _vram_pct, _used, total = mon.snapshot()
            finally:
                mon.stop()
        self.assertEqual(pct, -1)
        self.assertEqual(total, 0)


class TestLlmProtocol(unittest.TestCase):
    def test_build_llm_running(self):
        data = build_llm(LLM_STATUS_RUNNING, 40.3, "gemma4", cache_hit_pct=95, prompt_tokens=8000, has_alert=False)
        self.assertEqual(len(data), 1 + 2 + 1 + 2 + 1 + 24)
        status, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", data, 0)
        self.assertEqual(status, LLM_STATUS_RUNNING)
        self.assertEqual(tps_x10, 403)
        self.assertEqual(cache_hit, 95)
        self.assertEqual(prompt_k, 8)
        self.assertEqual(flags, 0)
        self.assertEqual(data[7:].rstrip(b"\x00"), b"gemma4")

    def test_build_llm_prompt_eval_alert(self):
        data = build_llm(LLM_STATUS_PROMPT_EVAL, 0.0, "ornith", cache_hit_pct=11, prompt_tokens=73000, has_alert=True)
        self.assertEqual(len(data), 31)
        status, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", data, 0)
        self.assertEqual(status, LLM_STATUS_PROMPT_EVAL)
        self.assertEqual(cache_hit, 11)
        self.assertEqual(prompt_k, 73)
        self.assertEqual(flags, 1)
        self.assertEqual(data[7:].rstrip(b"\x00"), b"ornith")

    def test_build_llm_idle(self):
        data = build_llm(LLM_STATUS_IDLE, 0.0, "")
        self.assertEqual(len(data), 31)
        status, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", data, 0)
        self.assertEqual(status, LLM_STATUS_IDLE)
        self.assertEqual(tps_x10, 0)
        self.assertEqual(data[7:], b"\x00" * 24)

    def test_build_llm_offline(self):
        data = build_llm(LLM_STATUS_OFFLINE, 0.0, "")
        self.assertEqual(len(data), 31)
        status, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", data, 0)
        self.assertEqual(status, 255)
        self.assertEqual(tps_x10, 0)

    def test_build_llm_negative_clamps(self):
        data = build_llm(-1, -5.0, "test")
        status, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", data, 0)
        self.assertEqual(status, 255)
        self.assertEqual(tps_x10, 0)

    def test_build_llm_long_model_name_truncated(self):
        long_name = "a" * 100
        data = build_llm(LLM_STATUS_RUNNING, 10.0, long_name)
        self.assertEqual(len(data), 31)
        name = data[7:].rstrip(b"\x00")
        self.assertLessEqual(len(name), 23)

    def test_frame_round_trip_with_llm(self):
        fields = [
            (FIELD_CPU, build_cpu(30, [10, 20])),
            (FIELD_LLM, build_llm(LLM_STATUS_RUNNING, 25.5, "qwen2.5-coder")),
        ]
        frame = pack_frame(fields)
        decoded = decode_frame(frame)
        self.assertEqual(len(decoded), 2)
        llm_blobs = field_data(decoded, FIELD_LLM)
        self.assertEqual(len(llm_blobs), 1)
        blob = llm_blobs[0]
        self.assertEqual(len(blob), 31)
        st, tps_x10, cache_hit, prompt_k, flags = struct.unpack_from("<BHBHB", blob, 0)
        self.assertEqual(st, LLM_STATUS_RUNNING)
        self.assertEqual(tps_x10, 255)
        self.assertEqual(blob[7:].rstrip(b"\x00"), b"qwen2.5-coder")


class TestSerialCommands(unittest.TestCase):
    def test_parse_text_stop_all(self):
        for s in ("CMD:STOP_ALL", "CMD:stop_all", "STOP_ALL", "CMD:LLM_STOP_ALL", "CMD:STOP"):
            res = parse_serial_command(s)
            self.assertEqual(res, (CMD_STOP_ALL, None))

    def test_parse_text_start_favorite(self):
        for s in ("CMD:START_FAVORITE", "START_FAVORITE", "CMD:LLM_START_FAVORITE", "CMD:START_FAV"):
            res = parse_serial_command(s)
            self.assertEqual(res, (CMD_START_FAVORITE, None))

    def test_parse_text_start_model(self):
        res = parse_serial_command("CMD:START_MODEL:gemma4")
        self.assertEqual(res, ("START_MODEL", "gemma4"))
        res = parse_serial_command("START:llama3")
        self.assertEqual(res, ("START_MODEL", "llama3"))

    def test_parse_binary_cmd_frame(self):
        frame = build_cmd_frame(CMD_STOP_ALL)
        res = parse_serial_command(frame)
        self.assertEqual(res, (CMD_STOP_ALL, None))

        frame_fav = build_cmd_frame(CMD_START_FAVORITE)
        res_fav = parse_serial_command(frame_fav)
        self.assertEqual(res_fav, (CMD_START_FAVORITE, None))

        frame_arg = build_cmd_frame(0x05, "custom_model")
        res_arg = parse_serial_command(frame_arg)
        self.assertEqual(res_arg, (0x05, "custom_model"))

    def test_parse_invalid_or_empty(self):
        self.assertIsNone(parse_serial_command(""))
        self.assertIsNone(parse_serial_command("   "))
        self.assertIsNone(parse_serial_command("SOME_RANDOM_LOG_LINE"))
        self.assertIsNone(parse_serial_command(b""))
        self.assertIsNone(parse_serial_command(b"\xaa\x01\x00\xf2\x01\x00"))  # bad crc


class TestLlmClient(unittest.TestCase):
    def test_status_str_to_code(self):
        self.assertEqual(_status_str_to_code("running"), LLM_STATUS_RUNNING)
        self.assertEqual(_status_str_to_code("RUNNING"), LLM_STATUS_RUNNING)
        self.assertEqual(_status_str_to_code("starting"), LLM_STATUS_STARTING)
        self.assertEqual(_status_str_to_code("loading"), LLM_STATUS_STARTING)
        self.assertEqual(_status_str_to_code("idle"), LLM_STATUS_IDLE)
        self.assertEqual(_status_str_to_code("stopped"), LLM_STATUS_IDLE)
        self.assertEqual(_status_str_to_code("unknown", has_active_model=True), LLM_STATUS_RUNNING)
        self.assertEqual(_status_str_to_code("unknown", has_active_model=False), LLM_STATUS_IDLE)
        self.assertEqual(_status_str_to_code(""), LLM_STATUS_IDLE)

    def test_get_status_success(self):
        sample = {
            "status": "running",
            "active_model": "gemma4",
            "tps": 40.3,
            "port": 8088,
            "system": {},
        }
        client = LLMControlClient()
        with mock.patch.object(client, "_request", return_value=sample):
            res = client.get_status()
        self.assertEqual(res, sample)

    def test_get_status_error_returns_none(self):
        client = LLMControlClient()
        with mock.patch.object(client, "_request", return_value=None):
            res = client.get_status()
        self.assertIsNone(res)

    def test_get_models_success(self):
        models = [
            {"id": "m1", "name": "Model 1", "is_favorite": False},
            {"id": "m2", "name": "Model 2", "is_favorite": True, "default_profile": "fast"},
        ]
        client = LLMControlClient()
        with mock.patch.object(client, "_request", return_value=models):
            res = client.get_models()
        self.assertEqual(len(res), 2)

    def test_stop_all(self):
        client = LLMControlClient()
        with mock.patch.object(client, "_request", return_value={"status": "stopped_all"}):
            self.assertTrue(client.stop_all())
        with mock.patch.object(client, "_request", return_value=None):
            self.assertFalse(client.stop_all())

    def test_start_model(self):
        client = LLMControlClient()
        with mock.patch.object(client, "_request", return_value={"status": "starting"}) as m_req:
            ok = client.start_model("gemma4", profile="default")
            self.assertTrue(ok)
            m_req.assert_called_once_with(
                "POST",
                "/api/models/gemma4/start",
                data={"profile": "default"},
            )

    def test_start_favorite(self):
        models = [
            {"id": "m1", "name": "Model 1", "is_favorite": False},
            {"id": "m2", "name": "Model 2", "is_favorite": True, "default_profile": "turbo"},
        ]
        client = LLMControlClient()
        with mock.patch.object(client, "get_models", return_value=models), \
             mock.patch.object(client, "start_model", return_value=True) as m_start:
            ok = client.start_favorite()
            self.assertTrue(ok)
            m_start.assert_called_once_with("m2", profile="turbo")

    def test_start_favorite_fallback_first(self):
        models = [
            {"id": "m1", "name": "Model 1", "is_favorite": False},
        ]
        client = LLMControlClient()
        with mock.patch.object(client, "get_models", return_value=models), \
             mock.patch.object(client, "start_model", return_value=True) as m_start:
            ok = client.start_favorite()
            self.assertTrue(ok)
            m_start.assert_called_once_with("m1", profile="default")

    def test_start_favorite_no_models(self):
        client = LLMControlClient()
        with mock.patch.object(client, "get_models", return_value=[]):
            self.assertFalse(client.start_favorite())


class TestLlmMonitor(unittest.TestCase):
    def test_monitor_polling_and_snapshot(self):
        client = LLMControlClient()
        sample = {
            "status": "running",
            "active_model": "gemma4",
            "tps": 40.3,
            "llm_metrics": {
                "phase": "generating",
                "cache_hit_pct": 88.5,
                "prompt_tokens": 12000,
                "has_alert": False,
            },
        }
        with mock.patch.object(client, "get_status", return_value=sample):
            mon = LLMMonitor(client=client, poll_interval=0.05)
            try:
                # wait briefly for worker thread to poll
                for _ in range(20):
                    st, tps, model, cache_hit, prompt_k, alert = mon.snapshot()
                    if st == LLM_STATUS_GENERATING:
                        break
                    time.sleep(0.01)
                self.assertEqual(st, LLM_STATUS_GENERATING)
                self.assertAlmostEqual(tps, 40.3)
                self.assertEqual(model, "gemma4")
                self.assertEqual(cache_hit, 88)
                self.assertEqual(prompt_k, 12000)
                self.assertFalse(alert)
            finally:
                mon.stop()

    def test_monitor_graceful_offline(self):
        client = LLMControlClient()
        with mock.patch.object(client, "get_status", return_value=None):
            mon = LLMMonitor(client=client, poll_interval=0.05)
            try:
                st, tps, model, cache_hit, prompt_k, alert = mon.snapshot()
                self.assertEqual(st, LLM_STATUS_OFFLINE)
                self.assertEqual(tps, 0.0)
                self.assertEqual(model, "")
                self.assertEqual(cache_hit, 255)
            finally:
                mon.stop()

    def test_monitor_stop_all_clears_cache(self):
        client = LLMControlClient()
        mon = LLMMonitor(client=client, poll_interval=10.0)
        try:
            with mon._lock:
                mon._status = LLM_STATUS_RUNNING
                mon._tps = 30.0
                mon._active_model = "test"
            with mock.patch.object(client, "stop_all", return_value=True):
                self.assertTrue(mon.stop_all())
            st, tps, model, cache_hit, prompt_k, alert = mon.snapshot()
            self.assertEqual(st, LLM_STATUS_IDLE)
            self.assertEqual(tps, 0.0)
            self.assertEqual(model, "")
        finally:
            mon.stop()


class TestLlmSnapshot(unittest.TestCase):
    def test_none_monitor(self):
        self.assertEqual(metrics.llm_snapshot(None), (255, 0.0, "", 255, 0, False))

    def test_valid_monitor(self):
        class _FakeMon:
            def snapshot(self):
                return (LLM_STATUS_RUNNING, 42.0, "m", 90, 5, False)
        self.assertEqual(metrics.llm_snapshot(_FakeMon()), (LLM_STATUS_RUNNING, 42.0, "m", 90, 5, False))


class TestLlmModelsTelemetry(unittest.TestCase):
    def test_build_empty_models(self):
        from host.protocol import build_llm_models
        data = build_llm_models([])
        self.assertEqual(data, b"\x00")

    def test_build_models_serialization(self):
        from host.protocol import build_llm_models
        models = [
            {"id": "gemma4", "is_favorite": True, "runtime": {"status": "running"}},
            {"id": "cyber", "is_favorite": False, "runtime": {"status": "stopped"}},
        ]
        data = build_llm_models(models)
        self.assertEqual(data[0], 2) # count
        # first model: 14 bytes name + 1 byte is_fav + 1 byte status
        self.assertEqual(data[1:8], b"gemma4\x00")
        self.assertEqual(data[15], 1) # is_fav = 1
        self.assertEqual(data[16], 1) # status = 1 (running)
        # second model:
        self.assertEqual(data[17:23], b"cyber\x00")
        self.assertEqual(data[31], 0) # is_fav = 0
        self.assertEqual(data[32], 0) # status = 0 (stopped)


class TestLlmProfilesTelemetry(unittest.TestCase):
    def test_build_empty_profiles(self):
        from host.protocol import build_llm_profiles
        data = build_llm_profiles("qwen9", [])
        self.assertEqual(data[:14], b"qwen9" + b"\x00" * 9)
        self.assertEqual(data[14], 0)  # count = 0

    def test_build_profiles_serialization(self):
        from host.protocol import build_llm_profiles, FIELD_LLM_PROFILES, pack_frame
        profiles = [
            {"name": "default", "ctx_size": 262144, "kv_type": "q4_0", "description": "ctx 256K, q4_0 KV"},
            {"name": "fast", "ctx_size": 8192, "kv_type": "f16", "description": "ctx 8K, f16 KV"},
            {"name": "vision", "ctx_size": 16384, "use_vision": True},
        ]
        data = build_llm_profiles("qwen9", profiles)
        self.assertEqual(data[:14], b"qwen9" + b"\x00" * 9)
        self.assertEqual(data[14], 3)  # count = 3
        # First profile: 12 bytes name, 4 bytes ctx, 18 bytes desc
        off = 15
        p1_name = data[off : off + 12].rstrip(b"\x00").decode()
        p1_ctx = struct.unpack_from("<I", data, off + 12)[0]
        p1_desc = data[off + 16 : off + 34].rstrip(b"\x00").decode()
        self.assertEqual(p1_name, "default")
        self.assertEqual(p1_ctx, 262144)
        self.assertEqual(p1_desc, "ctx 256K q4_0")

        frame = pack_frame([(FIELD_LLM_PROFILES, data)])
        self.assertTrue(len(frame) > 0)

    def test_parse_get_profiles_command(self):
        from host.protocol import parse_serial_command
        self.assertEqual(parse_serial_command("CMD:GET_PROFILES:qwen9"), ("GET_PROFILES", "qwen9"))
        self.assertEqual(parse_serial_command("PROFILES:seed"), ("GET_PROFILES", "seed"))


if __name__ == "__main__":
    sys.exit(unittest.main())