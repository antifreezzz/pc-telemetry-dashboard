#!/usr/bin/env python3
"""Combined test: open serial, send host.py data, read ESP32 debug, all in one process."""

import threading
import time
import struct
import psutil
import serial
from host import pack_net_kbps

SYNC = 0xAA
PORT = "/dev/ttyUSB0"
BAUD = 115200
DURATION = 20


def sender(ser, stop):
    """Send CPU/RAM/NET packets every 500ms."""
    seq = 0
    while not stop.is_set():
        try:
            cpu = int(psutil.cpu_percent(interval=None))
            mem = psutil.virtual_memory()
            ram_pct = int(mem.percent)
            ram_used = mem.used // (1024 * 1024)
            ram_total = mem.total // (1024 * 1024)
            n1 = psutil.net_io_counters()
            time.sleep(0.4)
            n2 = psutil.net_io_counters()
            rx, tx = host.pack_net_kbps(
                n2.bytes_recv - n1.bytes_recv, 0.4,
                n2.bytes_sent - n1.bytes_sent, 0.4,
            )

            # type 0x01 CPU
            ser.write(bytes([SYNC, 1, 0, 0x01, cpu]))
            # type 0x02 RAM
            ram_payload = struct.pack("<BII", ram_pct, ram_used, ram_total)
            ser.write(bytes([SYNC, len(ram_payload), 0, 0x02]) + ram_payload)
            # type 0x04 NET
            net_payload = struct.pack("<II", rx, tx)
            ser.write(bytes([SYNC, len(net_payload), 0, 0x04]) + net_payload)

            seq += 1
            print(f"[tx] seq={seq} cpu={cpu} ram={ram_used}/{ram_total} rx={rx} tx={tx}", flush=True)
        except Exception as e:
            print(f"[tx] error: {e}", flush=True)
            break


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

    stop = threading.Event()
    t_send = threading.Thread(target=sender, args=(s, stop), daemon=True)
    t_read = threading.Thread(target=reader, args=(s, stop), daemon=True)
    t_send.start()
    t_read.start()

    time.sleep(DURATION)
    stop.set()
    time.sleep(0.5)
    s.close()
    print("[main] done", flush=True)


if __name__ == "__main__":
    main()
