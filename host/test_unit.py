#!/usr/bin/env python3
"""Unit tests for host protocol v2.

Run: python3 host/test_unit.py
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from host.protocol import net_kbps, pack_net_kbps
from host.protocol import (
    SYNC,
    FRAME_TYPE,
    FIELD_CPU,
    FIELD_RAM,
    FIELD_GPU,
    FIELD_NET,
    FIELD_DISK,
    FIELD_HEADER,
    FIELD_PROC,
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
)


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
        data = build_proc(entries)
        self.assertEqual(len(data), 1 + 2 * 19)
        self.assertEqual(data[0], 2)
        off = 1
        for cpu_pct, pid, name in entries:
            p, ppid, pname = struct.unpack_from("<B H 16s", data, off)
            self.assertEqual(p, cpu_pct)
            self.assertEqual(ppid, pid)
            self.assertEqual(pname.rstrip(b"\x00").decode(), name)
            off += 19

    def test_proc_long_name(self):
        data = build_proc([(5, 1, "y" * 40)])
        self.assertEqual(len(data[1:]), 19)
        self.assertLessEqual(len(data[1:].split(b"\x00", 1)[0]), 15)

    def test_proc_pid_clamp(self):
        data = build_proc([(50, 1 << 20, "p")])
        self.assertEqual(struct.unpack_from("<H", data, 2)[0], 0xFFFF)


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
            (FIELD_PROC, build_proc([(9, 1, "a"), (8, 2, "bb"), (7, 3, "ccc")])),
        ]
        frame = pack_frame(fields)
        decoded = dict(decode_frame(frame))
        self.assertEqual(set(decoded.keys()), {FIELD_CPU, FIELD_RAM, FIELD_GPU,
                                               FIELD_NET, FIELD_DISK,
                                               FIELD_HEADER, FIELD_PROC})
        self.assertEqual(decoded[FIELD_RAM], struct.pack("<BII", 61, 2048, 8192))
        self.assertEqual(decoded[FIELD_GPU], struct.pack("<BBII", 70, 33, 2048, 8192))
        self.assertEqual(len(decoded[FIELD_HEADER]), 32)
        self.assertEqual(len(decoded[FIELD_PROC]), 1 + 3 * 19)

    def test_unknown_field_skippable(self):
        frame = pack_frame([(0x7F, b"\x01\x02\x03"), (FIELD_CPU, bytes([5]))])
        decoded = decode_frame(frame)
        self.assertEqual(len(decoded), 2)
        self.assertEqual(decoded[1], (FIELD_CPU, b"\x05"))


if __name__ == "__main__":
    sys.exit(unittest.main())