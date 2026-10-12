#!/usr/bin/env python3
"""Stand-in for the NXInput Switch client, for testing without a Switch.

Handshakes with the receiver, then presses every control once (sticks streamed
at ~60 Hz like the real client).

    python3 fake_switch.py [udp-port]      (default 8192, receiver must be running)
"""
import socket, struct, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8192
dst = ("127.0.0.1", PORT)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1)
s.sendto(b"xbox_switch\x00", dst)
assert s.recvfrom(16)[0] == b"xbox\x00"

BTN = {"A": 0x0C, "B": 0x0D, "X": 0x0E, "Y": 0x0F, "L": 0x09, "R": 0x0A, "ZL": 0x10, "ZR": 0x11,
       "MINUS": 0x05, "PLUS": 0x06, "LS": 0x07, "RS": 0x08,
       "UP": 0x01, "DOWN": 0x02, "LEFT": 0x03, "RIGHT": 0x04}
sticks = {"left": (0, 0), "right": (0, 0)}

def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for code, side in ((0x12, "left"), (0x13, "right")):
            x, y = sticks[side]
            s.sendto(bytes([code]) + struct.pack(">hh", x, y), dst)
        time.sleep(1 / 60)

pump(1.5)
for name in ["B", "A", "Y", "X", "L", "R", "ZL", "ZR", "MINUS", "PLUS", "LS", "RS", "UP", "RIGHT", "DOWN", "LEFT"]:
    print(f"{time.strftime('%H:%M:%S')}  switch {name}", flush=True)
    s.sendto(bytes([BTN[name], 1]), dst); pump(0.3)
    s.sendto(bytes([BTN[name], 0]), dst); pump(0.3)
for label, side, xy in [("left stick UP", "left", (0, 32767)), ("left stick RIGHT", "left", (32767, 0)),
                        ("right stick UP", "right", (0, 32767)), ("right stick LEFT", "right", (-32767, 0))]:
    print(f"{time.strftime('%H:%M:%S')}  switch {label}", flush=True)
    sticks[side] = xy; pump(0.5)
    sticks[side] = (0, 0); pump(0.5)
print("sequence done", flush=True)
