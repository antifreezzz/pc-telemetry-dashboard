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
FIELD_LLM = 0x08
FIELD_LLM_MODELS = 0x09
FIELD_LLM_PROFILES = 0x0A

PROC_KIND_CPU = 0
PROC_KIND_RAM = 1
PROC_KIND_GPU = 2
PROC_KIND_DISK_RD = 3
PROC_KIND_DISK_WR = 4

LLM_STATUS_IDLE = 0
LLM_STATUS_RUNNING = 1
LLM_STATUS_STARTING = 2
LLM_STATUS_OFFLINE = 255

CMD_STOP_ALL = 0x01
CMD_START_FAVORITE = 0x02

N_NA = 255

_HOSTNAME_LEN = 24
_PROC_NAME_LEN = 16
_MODEL_NAME_LEN = 24
_MODEL_ID_LEN = 14
_PROFILE_NAME_LEN = 12
_PROFILE_DESC_LEN = 18


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


def build_llm(status: int, tps: float, model: str) -> bytes:
    """Pack LLM telemetry: [status:u8][tps_x10:u16][model_name:24s]."""
    if status < 0 or status == LLM_STATUS_OFFLINE:
        st = LLM_STATUS_OFFLINE
    else:
        st = min(255, max(0, int(status)))
    tps_x10 = _u16(int(round(max(0.0, float(tps)) * 10)))
    return struct.pack("<BH", st, tps_x10) + _block(model, _MODEL_NAME_LEN)


def build_llm_models(models: list) -> bytes:
    """Pack models list for ESP32 selector screen.
    
    Layout: [count:u8] followed by count x [id:14s][is_fav:u8][status:u8]
    status: 0=stopped, 1=running, 2=starting. Total size <= 225 bytes (fits in u8 TLV len).
    """
    if not models:
        return bytes([0])
    
    clamped = models[:14]
    data = bytearray([len(clamped)])
    for m in clamped:
        m_id = m.get("id") or m.get("name") or ""
        is_fav = 1 if m.get("is_favorite") else 0
        rt = m.get("runtime") or {}
        st_str = (rt.get("status") or "").lower()
        if st_str == "running":
            st_val = 1
        elif st_str in ("starting", "loading"):
            st_val = 2
        else:
            st_val = 0
        
        data += _block(m_id, _MODEL_ID_LEN)
        data += bytes([is_fav, st_val])
    return bytes(data)


def build_llm_profiles(model_id: str, profiles: list) -> bytes:
    """Pack model profiles for ESP32 profile picker screen.
    
    Layout: [model_id:14s][count:u8] followed by count x [name:12s][ctx_size:u32][desc:18s]
    Max 6 profiles. Total payload size <= 14 + 1 + 6 * 34 = 219 bytes (fits in u8 TLV len).
    """
    data = bytearray(_block(model_id, _MODEL_ID_LEN))
    if not profiles:
        data.append(0)
        return bytes(data)

    clamped = profiles[:6]
    data.append(len(clamped))
    for p in clamped:
        name = p.get("name", "")
        ctx = int(p.get("ctx_size") or 0)
        
        # Build clean short desc, e.g. "ctx 16K q4_0" or "ctx 256K"
        raw_desc = p.get("description") or ""
        ctx_k = f"{ctx // 1024}K" if ctx >= 1024 else f"{ctx}"
        kv = p.get("kv_type") or ""
        if kv and kv != "f16":
            short_desc = f"ctx {ctx_k} {kv}"
        elif p.get("use_vision"):
            short_desc = f"ctx {ctx_k} vision"
        else:
            short_desc = f"ctx {ctx_k}"

        data += _block(name, _PROFILE_NAME_LEN)
        data += struct.pack("<I", _u32(ctx))
        data += _block(short_desc, _PROFILE_DESC_LEN)
    return bytes(data)


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


FRAME_TYPE_CMD = 0xF2


def build_cmd_frame(cmd_id: int, arg: str = "") -> bytes:
    """Build a binary command frame: [0xAA][len_lo][len_hi][0xF2][cmd_id][arg][crc8]."""
    arg_bytes = arg.encode("utf-8")
    payload = bytes([cmd_id]) + arg_bytes
    body = bytes([FRAME_TYPE_CMD]) + payload
    header = bytes([SYNC, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF, FRAME_TYPE_CMD])
    return header + payload + bytes([crc8(body)])


def parse_serial_command(line_or_data: str | bytes) -> tuple | None:
    """Parse text command or binary command frame.

    Returns (cmd_code_or_str, optional_arg) or None.
    """
    if isinstance(line_or_data, bytes):
        # Check for binary command frame: [0xAA][len_lo][len_hi][0xF2][payload...][crc8]
        if (
            len(line_or_data) >= 5
            and line_or_data[0] == SYNC
            and line_or_data[3] == FRAME_TYPE_CMD
        ):
            payload_len = line_or_data[1] | (line_or_data[2] << 8)
            if len(line_or_data) >= 4 + payload_len + 1:
                expected_crc = line_or_data[4 + payload_len]
                actual_crc = crc8(line_or_data[3 : 4 + payload_len])
                if expected_crc == actual_crc and payload_len >= 1:
                    cmd_id = line_or_data[4]
                    arg = ""
                    if payload_len > 1:
                        arg = line_or_data[5 : 4 + payload_len].decode("utf-8", errors="replace")
                    return cmd_id, (arg if arg else None)
        try:
            line_str = line_or_data.decode("utf-8", errors="replace").strip()
        except Exception:
            return None
    else:
        line_str = line_or_data.strip()

    if not line_str:
        return None

    # Handle text commands
    raw = line_str
    if raw.upper().startswith("CMD:"):
        raw = raw[4:].strip()

    cmd_upper = raw.upper()
    if cmd_upper in ("STOP_ALL", "LLM_STOP_ALL", "STOP"):
        return CMD_STOP_ALL, None
    elif cmd_upper in ("START_FAVORITE", "LLM_START_FAVORITE", "START_FAV"):
        return CMD_START_FAVORITE, None
    elif cmd_upper.startswith("START_MODEL:") or cmd_upper.startswith("START:"):
        _, arg = raw.split(":", 1)
        return "START_MODEL", arg.strip()
    elif cmd_upper.startswith("GET_PROFILES:") or cmd_upper.startswith("PROFILES:"):
        _, arg = raw.split(":", 1)
        return "GET_PROFILES", arg.strip()

    return None