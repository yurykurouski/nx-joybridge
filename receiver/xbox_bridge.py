"""Switch input -> Xbox controller report, sent to the nice!nano bridge over USB serial.

Frame format (host -> bridge), see docs/BRIDGE.md:
    A5 5A  TYPE  LEN  PAYLOAD[LEN]  CHK      CHK = XOR of TYPE, LEN and PAYLOAD bytes
The bridge answers with text lines starting with '#'.
"""

import logging
import struct
import time

log = logging.getLogger("nxinput")

SYNC = b"\xA5\x5A"
T_REPORT, T_TEST, T_BOOTLOADER, T_PING = 0x01, 0x02, 0x03, 0x04
ADAFRUIT_USB_VID = 0x239A      # the bridge firmware enumerates with Adafruit's USB IDs

# Switch button -> Xbox button bit. Positional mapping: the button in the same
# place on the pad, so the Switch's bottom face button (B) is the Xbox A.
BUTTON_BITS = {
    "b": 0x0001,        # Xbox A
    "a": 0x0002,        # Xbox B
    "y": 0x0008,        # Xbox X
    "x": 0x0010,        # Xbox Y
    "l": 0x0040,        # LB
    "r": 0x0080,        # RB
    "minus": 0x0400,    # View
    "plus": 0x0800,     # Menu
    "home": 0x1000,     # Xbox button (not sent by NXInput; NX Joybridge has a touch button)
    "stick_l": 0x2000,  # left stick click
    "stick_r": 0x4000,  # right stick click
}
TRIGGERS = {"zl": "brake", "zr": "accelerator"}   # digital on the Switch: 0 or full
TRIGGER_MAX = 1023
DPAD = ("dpad_up", "dpad_down", "dpad_left", "dpad_right")

_HAT = {  # (up, down, left, right) -> HID hat value (1..8 = N..NW, 0 = centre)
    (True, False, False, False): 1, (True, False, False, True): 2,
    (False, False, False, True): 3, (False, True, False, True): 4,
    (False, True, False, False): 5, (False, True, True, False): 6,
    (False, False, True, False): 7, (True, False, True, False): 8,
}


def frame(type_, payload=b""):
    chk = type_ ^ len(payload)
    for b in payload:
        chk ^= b
    return SYNC + bytes([type_, len(payload)]) + bytes(payload) + bytes([chk])


def hat_value(up, down, left, right):
    if up and down:
        up = down = False
    if left and right:
        left = right = False
    return _HAT.get((up, down, left, right), 0)


def axis(value, invert=False):
    """Switch stick value (-32768..32767) -> HID axis (0..65535, centre 0x8000)."""
    v = -int(value) if invert else int(value)
    return max(0, min(0xFFFF, 0x8000 + v))


class XboxGamepad:
    """Holds the controller state and sends it to the bridge when it changes."""

    def __init__(self, link, invert_y=True, keepalive=0.15):
        # HID convention: Y grows downwards, the Switch reports "up" as positive.
        self.link = link
        self.invert_y = invert_y
        self.keepalive = keepalive
        self._last_sent = None
        self._last_time = 0.0
        self.neutral()

    def neutral(self):
        self.buttons = 0
        self.dpad = {d: False for d in DPAD}
        self.lx = self.ly = self.rx = self.ry = 0
        self.brake = self.accelerator = 0

    def is_neutral(self):
        return (self.buttons == 0 and not any(self.dpad.values())
                and self.lx == self.ly == self.rx == self.ry == 0
                and self.brake == self.accelerator == 0)

    def button(self, name, pressed):
        if name in BUTTON_BITS:
            if pressed:
                self.buttons |= BUTTON_BITS[name]
            else:
                self.buttons &= ~BUTTON_BITS[name]
        elif name in TRIGGERS:
            setattr(self, TRIGGERS[name], TRIGGER_MAX if pressed else 0)
        elif name in self.dpad:
            self.dpad[name] = pressed

    def stick(self, side, x, y):
        if side == "left":
            self.lx, self.ly = x, y
        elif side == "right":
            self.rx, self.ry = x, y

    def report(self):
        """The 16-byte Xbox input report (report ID 1, little-endian)."""
        hat = hat_value(self.dpad["dpad_up"], self.dpad["dpad_down"],
                        self.dpad["dpad_left"], self.dpad["dpad_right"])
        return struct.pack("<HHHHHHBHB",
                           axis(self.lx), axis(self.ly, self.invert_y),
                           axis(self.rx), axis(self.ry, self.invert_y),
                           self.brake, self.accelerator, hat, self.buttons, 0)

    def flush(self, now=None, force=False):
        """Send the report if it changed, or as a keepalive so the bridge's failsafe stays off."""
        now = time.monotonic() if now is None else now
        rep = self.report()
        if force or rep != self._last_sent or now - self._last_time >= self.keepalive:
            self.link.write(frame(T_REPORT, rep))
            if rep != self._last_sent:
                log.debug("gamepad report %s", rep.hex())
            self._last_sent = rep
            self._last_time = now


# --------------------------------------------------------------------------
# Links to the bridge
# --------------------------------------------------------------------------
def find_port():
    """Serial port of the bridge (first device with Adafruit's USB vendor ID)."""
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == ADAFRUIT_USB_VID:
            return p.device
    return None


class SerialLink:
    def __init__(self, port):
        import serial
        self._ser = serial.Serial(port, 115200, timeout=0, write_timeout=0.5)
        self._rx = b""
        self.port = port

    def write(self, data):
        self._ser.write(data)

    def read_lines(self):
        """Status lines the bridge has sent since the last call."""
        self._rx += self._ser.read(4096)
        *lines, self._rx = self._rx.split(b"\n")
        return [l.decode(errors="replace").strip() for l in lines if l.strip()]

    def close(self):
        self._ser.close()


class LogLink:
    """--dry-run stand-in: logs what would be sent to the bridge."""

    def __init__(self):
        self.port = "(dry run)"
        self._last = None

    def write(self, data):
        if data != self._last:
            log.info("BRIDGE %s", data.hex())
            self._last = data

    def read_lines(self):
        return []

    def close(self):
        pass
