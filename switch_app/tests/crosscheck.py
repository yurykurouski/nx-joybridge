#!/usr/bin/env python3
"""Checks the Switch app's C mapping (source/xbox_report.c) against the Python
receiver's mapping (receiver/xbox_bridge.py), which is hardware-verified.

    python3 crosscheck.py [cases]        (default 5000)

Builds tests/report_cli with the system C compiler, feeds both implementations
the same random inputs plus edge cases, and compares report and frame bytes.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "receiver"))
import xbox_bridge as xb  # noqa: E402

NAMES = ["a", "b", "x", "y", "l", "r", "zl", "zr", "minus", "plus", "stick_l", "stick_r",
         "dpad_up", "dpad_down", "dpad_left", "dpad_right", "home"]
EDGES = [0, 1, -1, 32767, -32767, -32768, 40000, -40000, 16384, -16384]


class Sink:
    def write(self, data):
        self.last = data


def python_side(bits, sticks, invert):
    link = Sink()
    pad = xb.XboxGamepad(link, invert_y=invert)
    for name, on in zip(NAMES, bits):
        pad.button(name, bool(on))
    pad.stick("left", sticks[0], sticks[1])
    pad.stick("right", sticks[2], sticks[3])
    rep = pad.report()
    return rep.hex(), xb.frame(xb.T_REPORT, rep).hex()


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    rng = random.Random(1234)
    cases = []
    for i in range(17):                      # each button alone
        cases.append(([1 if j == i else 0 for j in range(17)], [0, 0, 0, 0], True))
    for e in EDGES:                          # stick edges on every axis, both Y conventions
        for k in range(4):
            s = [0, 0, 0, 0]
            s[k] = e
            cases.append(([0] * 17, s, True))
            cases.append(([0] * 17, s, False))
    while len(cases) < n:
        bits = [rng.randint(0, 1) for _ in range(17)]
        sticks = [rng.choice(EDGES) if rng.random() < 0.2 else rng.randint(-32767, 32767) for _ in range(4)]
        cases.append((bits, sticks, rng.random() < 0.8))

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "report_cli")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", os.path.join(HERE, "..", "source"),
                        os.path.join(HERE, "..", "source", "xbox_report.c"),
                        os.path.join(HERE, "report_cli.c"), "-o", exe], check=True)
        stdin = "".join(" ".join(map(str, bits + sticks + [int(inv)])) + "\n" for bits, sticks, inv in cases)
        out = subprocess.run([exe], input=stdin, capture_output=True, text=True, check=True).stdout.split("\n")

    bad = 0
    for (bits, sticks, inv), line in zip(cases, out):
        c_rep, c_frame = line.split()
        p_rep, p_frame = python_side(bits, sticks, inv)
        if (c_rep, c_frame) != (p_rep, p_frame):
            bad += 1
            if bad <= 5:
                print("MISMATCH", bits, sticks, inv, "\n  C ", c_rep, c_frame, "\n  Py", p_rep, p_frame)
    print(f"{len(cases)} cases, {bad} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
