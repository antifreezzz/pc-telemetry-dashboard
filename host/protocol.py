#!/usr/bin/env python3
"""Protocol v2 framing for the ESP32 dashboard link.

A single batched frame carrying TLV metric entries plus a CRC-8/ATM checksum:

    [0xAA] [len_lo] [len_hi] [type=0xF1] [payload...] [crc8]

len is the payload length only (uint16, little-endian). crc8 is computed over
[type, payload...] using CRC-8/ATM (poly 0x07, init 0x00, no refin/refout,
no xorout).

The payload is a sequence of TLV entries:

    [field_id: u8] [field_len: u8] [field_data...]

field_len covers field_data only, so a decoder can skip unknown field ids.
"""

import struct

SYNC = 0xAA
FRAME_TYPE = 0xF1

FIELD_CPU = 0x01
FIELD_RAM = 0x02
FIELD_GPU = 0x03
FIELD_NET = 0x04
FIELD_DISK = 0x05
FIELD_HEADER = 0x06
FIELD_PROC = 0x07

PROC_KIND_CPU = 0
PROC_KIND_RAM = 1
PROC_KIND_GPU = 2
PROC_KIND_DISK_RD = 3
PROC_KIND_DISK_WR = 4

N_NA = 255

_HOSTNAME_LEN = 24
_PROC_NAME_LEN = 16


def crc8(data: bytes) -> int:
    """CRC-8/ATM: poly 0x07, init 0x00, no refin/refout, no xorout."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def pack_kiBs(delta_bytes: int, interval_sec: float) -> int:
    """KiB/s rate, clamped to >= 0 (counters wrap/reset producing negatives)."""
    if interval_sec <= 0:
        return 0
    return max(0, int(delta_bytes / interval_sec / 1024))


def pack_pct(v: int, n_na: int = 255) -> int:
    """Clamp to [0, 100]; return n_na when input is negative (missing data)."""
    if v < 0:
        return n_na
    return max(0, min(100, v))


def net_kbps(delta_bytes: int, interval_sec: float) -> int:
    """Legacy KiB/s helper kept for the existing unit tests (== pack_kiBs)."""
    return pack_kiBs(delta_bytes, interval_sec)


def pack_net_kbps(rx_delta, rx_interval, tx_delta, tx_interval) -> tuple:
    return (
        pack_kiBs(rx_delta, rx_interval),
        pack_kiBs(tx_delta, tx_interval),
    )


def _u16(v: int) -> int:
    v = int(v)
    if v < 0:
        return 0
    return min(65535, v)


def _u32(v: int) -> int:
    v = int(v)
    if v < 0:
        return 0
    return min(0xFFFFFFFF, v)


def _block(name: str, size: int) -> bytes:
    """Null-terminated, null-padded block; truncate content safely for UTF-8."""
    raw = name.encode("utf-8", errors="replace")
    content = raw[: size - 1]
    return content.ljust(size, b"\x00")


def build_cpu(pct: int, cores: list) -> bytes:
    data = bytes([pack_pct(pct)])
    data += bytes(pack_pct(c, n_na=0) for c in cores)
    return data


def build_ram(pct: int, used_mb: int, total_mb: int) -> bytes:
    return struct.pack("<BII", pack_pct(pct), _u32(used_mb), _u32(total_mb))


def build_gpu(pct: int, vram_pct: int, vram_used_mb: int, vram_total_mb: int) -> bytes:
    if pct < 0:
        return struct.pack("<BBII", 255, 0, 0, 0)
    return struct.pack("<BBII", pack_pct(pct), pack_pct(vram_pct),
                       _u32(vram_used_mb), _u32(vram_total_mb))


def build_net(rx_kibs: int, tx_kibs: int) -> bytes:
    return struct.pack("<II", _u32(rx_kibs), _u32(tx_kibs))


def build_disk(rd_kibs: int, wr_kibs: int, used_pct: int) -> bytes:
    return struct.pack("<IIB", _u32(rd_kibs), _u32(wr_kibs), pack_pct(used_pct))


def build_header(uptime_s: int, epoch_s: int, hostname: str) -> bytes:
    return struct.pack("<II", _u32(uptime_s), _u32(epoch_s)) + _block(hostname, _HOSTNAME_LEN)


def build_proc(kind: int, entries: list) -> bytes:
    """entries: list of (value:int, pid:int, name:str).

    Layout: [kind:u8][count:u8] then count x [value:u32][pid:u16][name:16].
    The kind selects the metric the value is expressed in (see PROC_KIND_*).
    """
    if len(entries) > 255:
        raise ValueError("too many process entries")
    payload = bytearray([kind, len(entries)])
    for value, pid, name in entries:
        payload += struct.pack("<I", _u32(value))
        payload += struct.pack("<H", _u16(pid))
        payload += _block(name, _PROC_NAME_LEN)
    return bytes(payload)


def pack_frame(fields: list) -> bytes:
    """Build one complete frame from TLV (field_id, field_data) entries."""
    payload = bytearray()
    for field_id, data in fields:
        payload.append(field_id)
        payload.append(len(data))
        payload += data
    if len(payload) > 65535:
        raise ValueError("payload too long")
    body = bytes([FRAME_TYPE]) + bytes(payload)
    header = bytes([SYNC, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF, FRAME_TYPE])
    return header + payload + bytes([crc8(body)])