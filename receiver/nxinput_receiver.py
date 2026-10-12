#!/usr/bin/env python3
"""NXInput receiver for macOS.

Listens for the NXInput Switch homebrew client (UDP port 8192), answers its
handshake, decodes button/stick packets (see docs/PROTOCOL.md) and turns them
into keyboard events.

Usage:
    python3 nxinput_receiver.py                 # run with default mapping
    python3 nxinput_receiver.py --debug         # print every decoded input
    python3 nxinput_receiver.py --dry-run       # no key presses, just log
    python3 nxinput_receiver.py --config my.json
"""

import argparse
import json
import logging
import signal
import socket
import struct
import sys
import time

PORT = 8192
HANDSHAKE_REQ = b"xbox_switch"
HANDSHAKE_REPLY = b"xbox\x00"
STICK_MAX = 32767

# code -> config key
BUTTONS = {
    0x01: "dpad_up",
    0x02: "dpad_down",
    0x03: "dpad_left",
    0x04: "dpad_right",
    0x05: "minus",
    0x06: "plus",
    0x07: "stick_l",
    0x08: "stick_r",
    0x09: "l",
    0x0A: "r",
    0x0C: "a",
    0x0D: "b",
    0x0E: "x",
    0x0F: "y",
    0x10: "zl",
    0x11: "zr",
}
STICK_CODES = {0x12: "left", 0x13: "right"}

DEFAULT_CONFIG = {
    "port": PORT,
    "deadzone": 0.5,        # fraction of full stick travel before a direction counts
    "timeout": 1.0,         # seconds without packets before all keys are released
    "buttons": {
        "dpad_up": "up",
        "dpad_down": "down",
        "dpad_left": "left",
        "dpad_right": "right",
        "a": "l",
        "b": "k",
        "x": "i",
        "y": "j",
        "l": "q",
        "r": "e",
        "zl": "u",
        "zr": "o",
        "plus": "enter",
        "minus": "esc",
        "stick_l": "f",
        "stick_r": "g",
    },
    "sticks": {
        "left": {"up": "w", "down": "s", "left": "a", "right": "d"},
        "right": {},
    },
}

log = logging.getLogger("nxinput")


# --------------------------------------------------------------------------
# Key backends
# --------------------------------------------------------------------------
class DryRunBackend:
    """Logs key events instead of sending them."""

    def press(self, key):
        log.info("KEY DOWN  %s", key)

    def release(self, key):
        log.info("KEY UP    %s", key)


class PynputBackend:
    def __init__(self):
        try:
            from pynput.keyboard import Controller, Key
        except ImportError:
            sys.exit("pynput is not installed. Run:  python3 -m pip install pynput")
        self._kb = Controller()
        self._special = {
            "up": Key.up, "down": Key.down, "left": Key.left, "right": Key.right,
            "enter": Key.enter, "esc": Key.esc, "space": Key.space, "tab": Key.tab,
            "shift": Key.shift, "ctrl": Key.ctrl, "alt": Key.alt, "cmd": Key.cmd,
            "backspace": Key.backspace,
        }

    def _resolve(self, key):
        return self._special.get(key, key)

    def press(self, key):
        self._kb.press(self._resolve(key))

    def release(self, key):
        self._kb.release(self._resolve(key))


# --------------------------------------------------------------------------
# Core logic (no sockets, so it can be unit-tested)
# --------------------------------------------------------------------------
def merge_config(base, override):
    out = json.loads(json.dumps(base))
    for k, v in override.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = merge_config(out[k], v)
        else:
            out[k] = v
    return out


class Receiver:
    def __init__(self, config, backend, allow_ip=None, gamepad=None):
        self.backend = backend
        self.gamepad = gamepad      # xbox_bridge.XboxGamepad: send input as a gamepad, not keys
        self.allow_ip = allow_ip
        self.peer_ip = None
        self.last_rx = 0.0
        self._sources = {}   # key -> set of active sources holding it
        self._active = {}    # source -> key currently held for that source
        self.set_config(config)

    def set_config(self, config):
        self.release_all()
        self.config = config
        self.deadzone = int(float(config["deadzone"]) * STICK_MAX)
        self.timeout = float(config["timeout"])

    # -- key bookkeeping ---------------------------------------------------
    def _set_source(self, source, key, active):
        """Hold/release `key` on behalf of `source`; a key stays down while any source holds it."""
        if not key:
            return
        if active:
            if source in self._active:
                return
            self._active[source] = key
            holders = self._sources.setdefault(key, set())
            if not holders:
                self.backend.press(key)
            holders.add(source)
        else:
            held = self._active.pop(source, None)
            if held is None:
                return
            holders = self._sources.get(held, set())
            holders.discard(source)
            if not holders:
                self.backend.release(held)

    def release_all(self):
        for source, key in list(self._active.items()):
            self._set_source(source, key, False)
        if self.gamepad:
            self.gamepad.neutral()

    # -- packet handling ---------------------------------------------------
    def handle(self, data, addr, now=None):
        """Process one datagram. Returns bytes to send back to `addr`, or None."""
        now = time.monotonic() if now is None else now
        ip = addr[0]

        if HANDSHAKE_REQ in data:
            if self.allow_ip and ip != self.allow_ip:
                log.warning("Ignoring handshake from %s (allowed: %s)", ip, self.allow_ip)
                return None
            if ip != self.peer_ip:
                log.info("Switch connected from %s", ip)
                self.release_all()
                self.peer_ip = ip
            self.last_rx = now
            return HANDSHAKE_REPLY

        if ip != self.peer_ip:
            return None   # not our Switch

        self.last_rx = now
        if len(data) == 2 and data[0] in BUTTONS:
            self._on_button(data[0], bool(data[1]))
        elif len(data) == 5 and data[0] in STICK_CODES:
            x, y = struct.unpack(">hh", data[1:5])
            self._on_stick(STICK_CODES[data[0]], x, y)
        else:
            log.debug("Ignoring malformed packet: %r", data)
        return None

    def _on_button(self, code, pressed):
        name = BUTTONS[code]
        log.debug("button %-8s %s", name, "down" if pressed else "up")
        if self.gamepad:
            self.gamepad.button(name, pressed)
            return
        self._set_source(("btn", name), self.config["buttons"].get(name), pressed)

    def _on_stick(self, side, x, y):
        log.debug("stick %-5s x=%6d y=%6d", side, x, y)
        if self.gamepad:
            self.gamepad.stick(side, x, y)
            return
        mapping = self.config["sticks"].get(side, {})
        dz = self.deadzone
        states = {"up": y > dz, "down": y < -dz, "left": x < -dz, "right": x > dz}
        for direction, active in states.items():
            self._set_source(("stick", side, direction), mapping.get(direction), active)

    def tick(self, now=None):
        """Call regularly; releases everything if the Switch went quiet."""
        now = time.monotonic() if now is None else now
        holding = bool(self._active) or (self.gamepad is not None and not self.gamepad.is_neutral())
        if self.peer_ip and holding and now - self.last_rx > self.timeout:
            log.warning("No packets for %.1fs - releasing all keys", self.timeout)
            self.release_all()
        if self.gamepad:
            self.gamepad.flush(now)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
def load_config(path):
    config = DEFAULT_CONFIG
    if path:
        with open(path) as f:
            config = merge_config(DEFAULT_CONFIG, json.load(f))
    return config


def main(argv=None):
    ap = argparse.ArgumentParser(description="NXInput receiver for macOS")
    ap.add_argument("--config", help="JSON mapping file (overrides defaults; SIGHUP reloads it)")
    ap.add_argument("--port", type=int, help="UDP port (default 8192 - must match the Switch client)")
    ap.add_argument("--allow-ip", help="only accept a Switch with this IP address")
    ap.add_argument("--dry-run", action="store_true", help="log key events instead of pressing keys")
    ap.add_argument("--debug", action="store_true", help="print every decoded input packet")
    ap.add_argument("--dump-config", action="store_true", help="print the default config and exit")
    ap.add_argument("--gamepad", nargs="?", const="auto", metavar="PORT",
                    help="send input as an Xbox controller through the nice!nano bridge "
                         "on this serial port (default: find it automatically) instead of keys")
    ap.add_argument("--no-invert-y", action="store_true",
                    help="gamepad mode: do not flip the stick Y axes")
    args = ap.parse_args(argv)

    if args.dump_config:
        print(json.dumps(DEFAULT_CONFIG, indent=2))
        return 0

    logging.basicConfig(level=logging.DEBUG if args.debug else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    config = load_config(args.config)
    port = args.port or config["port"]
    gamepad = link = None
    if args.gamepad:
        import xbox_bridge
        if args.dry_run:
            link = xbox_bridge.LogLink()
        else:
            serial_port = xbox_bridge.find_port() if args.gamepad == "auto" else args.gamepad
            if not serial_port:
                sys.exit("No nice!nano bridge found on USB. Plug it in or pass --gamepad /dev/cu.usbmodemXXXX")
            try:
                link = xbox_bridge.SerialLink(serial_port)
            except ImportError:
                sys.exit("pyserial is not installed. Run:  python3 -m pip install pyserial")
        gamepad = xbox_bridge.XboxGamepad(link, invert_y=not args.no_invert_y)
        log.info("Gamepad mode: sending to the bridge on %s", link.port)
        backend = DryRunBackend()   # keys are not used in gamepad mode
    else:
        backend = DryRunBackend() if args.dry_run else PynputBackend()
    rx = Receiver(config, backend, allow_ip=args.allow_ip, gamepad=gamepad)

    reload_flag = {"v": False}
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, lambda *_: reload_flag.update(v=True))

    def _stop(*_):
        raise KeyboardInterrupt     # same clean shutdown as Ctrl-C: release keys, neutral pad
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)  # also when started from a shell that ignores SIGINT

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(0.2)
    log.info("Listening on UDP %d. On the Switch, enter this Mac's IP address.", port)

    try:
        while True:
            if reload_flag["v"]:
                reload_flag["v"] = False
                try:
                    rx.set_config(load_config(args.config))
                    log.info("Config reloaded")
                except Exception as e:  # keep running on a bad edit
                    log.error("Config reload failed: %s", e)
            if link:
                for line in link.read_lines():
                    log.info("bridge: %s", line)
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                rx.tick()
                continue
            reply = rx.handle(data, addr)
            if reply:
                sock.sendto(reply, addr)
            rx.tick()
    except KeyboardInterrupt:
        pass
    finally:
        rx.release_all()
        if gamepad:
            gamepad.flush(force=True)   # leave the controller neutral
            link.close()
        sock.close()
        log.info("Stopped; all keys released.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
