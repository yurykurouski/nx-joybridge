import socket
import struct
import time
import unittest

import nxinput_receiver as nr


class FakeBackend:
    def __init__(self):
        self.events = []

    def press(self, key):
        self.events.append(("down", key))

    def release(self, key):
        self.events.append(("up", key))


SWITCH = ("192.168.1.50", 40000)
OTHER = ("192.168.1.99", 40001)


def stick(code, x, y):
    return bytes([code]) + struct.pack(">hh", x, y)


class CoreTests(unittest.TestCase):
    def setUp(self):
        self.be = FakeBackend()
        self.rx = nr.Receiver(nr.DEFAULT_CONFIG, self.be)

    def connect(self, now=0.0):
        return self.rx.handle(b"xbox_switch\x00", SWITCH, now)

    def test_handshake_reply(self):
        self.assertEqual(self.connect(), b"xbox\x00")
        self.assertEqual(self.rx.peer_ip, SWITCH[0])

    def test_input_before_handshake_ignored(self):
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.0)
        self.assertEqual(self.be.events, [])

    def test_button_down_up(self):
        self.connect()
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)   # A -> l
        self.rx.handle(bytes([0x0C, 0]), SWITCH, 0.2)
        self.assertEqual(self.be.events, [("down", "l"), ("up", "l")])

    def test_repeated_down_presses_once(self):
        self.connect()
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.2)
        self.assertEqual(self.be.events, [("down", "l")])

    def test_packets_from_other_ip_ignored(self):
        self.connect()
        self.rx.handle(bytes([0x0C, 1]), OTHER, 0.1)
        self.assertEqual(self.be.events, [])

    def test_stick_directions_and_deadzone(self):
        self.connect()
        self.rx.handle(stick(0x12, 0, 8000), SWITCH, 0.1)     # inside deadzone
        self.assertEqual(self.be.events, [])
        self.rx.handle(stick(0x12, 0, 30000), SWITCH, 0.2)    # up -> w
        self.rx.handle(stick(0x12, 30000, 30000), SWITCH, 0.3)  # up+right -> w, d
        self.rx.handle(stick(0x12, 0, 0), SWITCH, 0.4)        # centre
        self.assertEqual(self.be.events, [
            ("down", "w"), ("down", "d"), ("up", "w"), ("up", "d")])

    def test_stick_y_negative_is_down(self):
        self.connect()
        self.rx.handle(stick(0x12, 0, -30000), SWITCH, 0.1)
        self.assertEqual(self.be.events, [("down", "s")])

    def test_right_stick_unmapped_by_default(self):
        self.connect()
        self.rx.handle(stick(0x13, 30000, 30000), SWITCH, 0.1)
        self.assertEqual(self.be.events, [])

    def test_malformed_packets_ignored(self):
        self.connect()
        for bad in (b"", b"\x0c", b"\x0c\x01\x01", b"\x12\x00", b"\xff\x01", b"\x0b\x01"):
            self.rx.handle(bad, SWITCH, 0.1)
        self.assertEqual(self.be.events, [])

    def test_timeout_releases_keys(self):
        self.connect(0.0)
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)
        self.rx.tick(0.5)
        self.assertEqual(self.be.events, [("down", "l")])
        self.rx.tick(2.0)
        self.assertEqual(self.be.events, [("down", "l"), ("up", "l")])

    def test_reconnect_from_new_ip_releases_keys(self):
        self.connect()
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)
        self.rx.handle(b"xbox_switch\x00", OTHER, 0.2)
        self.assertEqual(self.be.events[-1], ("up", "l"))
        self.assertEqual(self.rx.peer_ip, OTHER[0])

    def test_allow_ip_blocks_handshake(self):
        rx = nr.Receiver(nr.DEFAULT_CONFIG, self.be, allow_ip="10.0.0.1")
        self.assertIsNone(rx.handle(b"xbox_switch\x00", SWITCH, 0.0))
        self.assertIsNone(rx.peer_ip)

    def test_shared_key_stays_down_until_last_source_releases(self):
        cfg = nr.merge_config(nr.DEFAULT_CONFIG, {"buttons": {"a": "w"}})  # A and stick-up both -> w
        rx = nr.Receiver(cfg, self.be)
        rx.handle(b"xbox_switch\x00", SWITCH, 0.0)
        rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)
        rx.handle(stick(0x12, 0, 30000), SWITCH, 0.2)
        rx.handle(bytes([0x0C, 0]), SWITCH, 0.3)
        self.assertEqual(self.be.events, [("down", "w")])
        rx.handle(stick(0x12, 0, 0), SWITCH, 0.4)
        self.assertEqual(self.be.events, [("down", "w"), ("up", "w")])

    def test_config_merge_overrides_only_given_keys(self):
        cfg = nr.merge_config(nr.DEFAULT_CONFIG, {"buttons": {"a": "space"}})
        self.assertEqual(cfg["buttons"]["a"], "space")
        self.assertEqual(cfg["buttons"]["b"], "k")
        self.assertEqual(nr.DEFAULT_CONFIG["buttons"]["a"], "l")  # original untouched

    def test_set_config_releases_held_keys(self):
        self.connect()
        self.rx.handle(bytes([0x0C, 1]), SWITCH, 0.1)
        self.rx.set_config(nr.merge_config(nr.DEFAULT_CONFIG, {"buttons": {"a": "space"}}))
        self.assertEqual(self.be.events, [("down", "l"), ("up", "l")])


class LoopbackTest(unittest.TestCase):
    """Run the real script as a subprocess and play the Switch client against it."""

    def test_end_to_end(self):
        import os
        import signal
        import subprocess
        import sys

        port = 38192
        here = os.path.dirname(os.path.abspath(__file__))
        proc = subprocess.Popen(
            [sys.executable, os.path.join(here, "nxinput_receiver.py"), "--dry-run", "--port", str(port)],
            stderr=subprocess.PIPE, text=True)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            time.sleep(0.6)
            s.settimeout(1.0)
            s.sendto(b"xbox_switch\x00", ("127.0.0.1", port))
            reply, _ = s.recvfrom(64)
            self.assertEqual(reply, b"xbox\x00")

            s.sendto(bytes([0x0C, 1]), ("127.0.0.1", port))
            s.sendto(stick(0x12, 0, 32767), ("127.0.0.1", port))
            time.sleep(0.3)
            s.sendto(bytes([0x0C, 0]), ("127.0.0.1", port))
            s.sendto(stick(0x12, 0, 0), ("127.0.0.1", port))
            time.sleep(0.3)

            # go quiet with a key held: the timeout must release it
            s.sendto(bytes([0x0D, 1]), ("127.0.0.1", port))   # B -> k
            time.sleep(1.8)

            # hold another key, then stop the receiver: shutdown must release it
            s.sendto(bytes([0x0E, 1]), ("127.0.0.1", port))   # X -> i
            time.sleep(0.3)
            proc.send_signal(signal.SIGINT)
            _, err = proc.communicate(timeout=5)
        finally:
            s.close()
            if proc.poll() is None:
                proc.kill()

        for expected in ("Listening on UDP 38192", "Switch connected", "KEY DOWN  l", "KEY DOWN  w",
                         "KEY UP    l", "KEY UP    w", "KEY DOWN  k", "releasing all keys",
                         "KEY UP    k", "KEY DOWN  i", "KEY UP    i", "all keys released"):
            self.assertIn(expected, err)
        # k must be released by the timeout before i is pressed
        self.assertLess(err.index("KEY UP    k"), err.index("KEY DOWN  i"))
        self.assertLess(err.index("KEY DOWN  i"), err.index("KEY UP    i"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
