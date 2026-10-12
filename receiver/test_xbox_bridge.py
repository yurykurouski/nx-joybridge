import struct
import unittest

import nxinput_receiver as nr
import xbox_bridge as xb


class FakeLink:
    def __init__(self):
        self.frames = []

    def write(self, data):
        self.frames.append(data)

    def reports(self):
        """Decoded report payloads, checking every frame on the way."""
        out = []
        for f in self.frames:
            self.check(f)
            if f[2] == xb.T_REPORT:
                out.append(f[4:-1])
        return out

    @staticmethod
    def check(f):
        assert f[:2] == xb.SYNC, f
        typ, ln = f[2], f[3]
        assert len(f) == 4 + ln + 1, f
        chk = typ ^ ln
        for b in f[4:4 + ln]:
            chk ^= b
        assert f[-1] == chk, f


def unpack(rep):
    x, y, z, rz, brake, acc, hat, buttons, share = struct.unpack("<HHHHHHBHB", rep)
    return dict(x=x, y=y, z=z, rz=rz, brake=brake, acc=acc, hat=hat, buttons=buttons, share=share)


class FrameTests(unittest.TestCase):
    def test_frame_layout_and_checksum(self):
        f = xb.frame(xb.T_TEST, b"\x01")
        self.assertEqual(f, bytes([0xA5, 0x5A, 0x02, 0x01, 0x01, 0x02 ^ 0x01 ^ 0x01]))

    def test_empty_frame(self):
        self.assertEqual(xb.frame(xb.T_PING), bytes([0xA5, 0x5A, 0x04, 0x00, 0x04]))


class MappingTests(unittest.TestCase):
    def setUp(self):
        self.link = FakeLink()
        self.pad = xb.XboxGamepad(self.link)

    def test_neutral_report(self):
        r = unpack(self.pad.report())
        self.assertEqual(len(self.pad.report()), 16)
        self.assertEqual((r["x"], r["y"], r["z"], r["rz"]), (0x8000,) * 4)
        self.assertEqual((r["brake"], r["acc"], r["hat"], r["buttons"]), (0, 0, 0, 0))

    def test_positional_face_buttons(self):
        for switch_btn, xbox_bit in (("b", 0x01), ("a", 0x02), ("y", 0x08), ("x", 0x10)):
            self.pad.neutral()
            self.pad.button(switch_btn, True)
            self.assertEqual(unpack(self.pad.report())["buttons"], xbox_bit, switch_btn)

    def test_home_is_xbox_button_bit(self):
        self.pad.button("home", True)
        self.assertEqual(unpack(self.pad.report())["buttons"], 0x1000)

    def test_release_clears_bit(self):
        self.pad.button("l", True)
        self.pad.button("r", True)
        self.pad.button("l", False)
        self.assertEqual(unpack(self.pad.report())["buttons"], 0x80)

    def test_triggers_are_full_or_zero(self):
        self.pad.button("zl", True)
        self.pad.button("zr", True)
        r = unpack(self.pad.report())
        self.assertEqual((r["brake"], r["acc"]), (1023, 1023))
        self.pad.button("zl", False)
        self.assertEqual(unpack(self.pad.report())["brake"], 0)

    def test_hat_values(self):
        cases = {
            (): 0, ("dpad_up",): 1, ("dpad_up", "dpad_right"): 2, ("dpad_right",): 3,
            ("dpad_down", "dpad_right"): 4, ("dpad_down",): 5, ("dpad_down", "dpad_left"): 6,
            ("dpad_left",): 7, ("dpad_up", "dpad_left"): 8,
            ("dpad_up", "dpad_down"): 0, ("dpad_up", "dpad_down", "dpad_left"): 7,
        }
        for pressed, want in cases.items():
            self.pad.neutral()
            for d in pressed:
                self.pad.button(d, True)
            self.assertEqual(unpack(self.pad.report())["hat"], want, pressed)

    def test_axis_encoding(self):
        self.assertEqual(xb.axis(0), 0x8000)
        self.assertEqual(xb.axis(32767), 0xFFFF)
        self.assertEqual(xb.axis(-32768), 0)
        self.assertEqual(xb.axis(32767, invert=True), 0x0001)
        self.assertEqual(xb.axis(-32768, invert=True), 0xFFFF)   # clamped

    def test_stick_up_is_low_y_by_default(self):
        self.pad.stick("left", 0, 32767)          # Switch: positive Y = up
        self.assertLess(unpack(self.pad.report())["y"], 0x8000)
        pad = xb.XboxGamepad(FakeLink(), invert_y=False)
        pad.stick("left", 0, 32767)
        self.assertGreater(unpack(pad.report())["y"], 0x8000)

    def test_right_stick_goes_to_z_rz(self):
        self.pad.stick("right", 32767, 0)
        r = unpack(self.pad.report())
        self.assertEqual(r["z"], 0xFFFF)
        self.assertEqual(r["x"], 0x8000)

    def test_flush_sends_on_change_and_keepalive_only(self):
        self.pad.flush(now=0.0)
        self.pad.flush(now=0.05)                  # unchanged, too soon: nothing
        self.assertEqual(len(self.link.frames), 1)
        self.pad.button("b", True)
        self.pad.flush(now=0.06)                  # changed: sent
        self.pad.flush(now=0.30)                  # unchanged but keepalive due: sent
        self.assertEqual(len(self.link.frames), 3)
        reps = [unpack(r)["buttons"] for r in self.link.reports()]
        self.assertEqual(reps, [0, 1, 1])


class ReceiverIntegrationTests(unittest.TestCase):
    SWITCH = ("192.168.1.50", 40000)

    def setUp(self):
        self.link = FakeLink()
        self.pad = xb.XboxGamepad(self.link)
        self.keys = []

        class Keys:
            def press(_, k): self.keys.append(("down", k))
            def release(_, k): self.keys.append(("up", k))

        self.rx = nr.Receiver(nr.DEFAULT_CONFIG, Keys(), gamepad=self.pad)
        self.rx.handle(b"xbox_switch\x00", self.SWITCH, 0.0)

    def feed(self, data, now):
        self.rx.handle(data, self.SWITCH, now)
        self.rx.tick(now)

    def test_buttons_and_sticks_reach_gamepad_not_keys(self):
        self.feed(bytes([0x0D, 1]), 0.1)                       # Switch B -> Xbox A
        self.feed(bytes([0x12]) + struct.pack(">hh", 32767, 0), 0.12)
        last = unpack(self.link.reports()[-1])
        self.assertEqual(last["buttons"], 0x01)
        self.assertEqual(last["x"], 0xFFFF)
        self.assertEqual(self.keys, [])

    def test_timeout_sends_neutral(self):
        self.feed(bytes([0x0D, 1]), 0.1)
        self.rx.tick(2.0)                                      # Switch went quiet
        self.assertEqual(unpack(self.link.reports()[-1])["buttons"], 0)

    def test_reconnect_resets_gamepad(self):
        self.feed(bytes([0x0D, 1]), 0.1)
        self.rx.handle(b"xbox_switch\x00", ("192.168.1.77", 40000), 0.2)
        self.rx.tick(0.2)
        self.assertEqual(unpack(self.link.reports()[-1])["buttons"], 0)


class MainWiringTest(unittest.TestCase):
    """Gamepad mode with a real (serial) link must still listen on the UDP port it was given."""

    def test_gamepad_mode_keeps_udp_port(self):
        import socket, threading
        from unittest import mock

        bound = {}

        class StopLoop(Exception):
            pass

        class FakeSock:
            def __init__(self, *a): pass
            def setsockopt(self, *a): pass
            def bind(self, addr): bound["addr"] = addr
            def settimeout(self, t): pass
            def recvfrom(self, n): raise StopLoop()
            def close(self): pass

        class FakeSerial(FakeLink):
            port = "/dev/cu.fake"
            def read_lines(self): return []
            def close(self): pass

        with mock.patch.object(xb, "find_port", return_value="/dev/cu.fake"), \
             mock.patch.object(xb, "SerialLink", lambda port: FakeSerial()), \
             mock.patch.object(nr.socket, "socket", FakeSock):
            with self.assertRaises(StopLoop):
                nr.main(["--gamepad", "--port", "38555"])
        self.assertEqual(bound["addr"], ("0.0.0.0", 38555))


if __name__ == "__main__":
    unittest.main(verbosity=2)
