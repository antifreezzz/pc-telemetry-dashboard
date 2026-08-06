#!/usr/bin/env python3
"""Unit tests for host network helpers.

Run: python3 host/test_unit.py
"""

import struct
import sys
import unittest

from host import net_kbps, pack_net_kbps


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


if __name__ == "__main__":
    sys.exit(unittest.main())
