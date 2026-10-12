#!/usr/bin/env python3
"""Send a command to the nice!nano bridge over USB serial.

    python3 bridge_cmd.py ping            # is it there, is the Mac connected over BLE?
    python3 bridge_cmd.py test on|off     # built-in test pattern (A + left stick)
    python3 bridge_cmd.py bootloader      # reboot into the UF2 bootloader (NICENANO drive)
    python3 bridge_cmd.py --port /dev/cu.usbmodem101 ping
"""

import argparse
import sys
import time

import xbox_bridge as xb


def main(argv=None):
    ap = argparse.ArgumentParser(description="Command the nice!nano Xbox bridge")
    ap.add_argument("--port", help="serial port (default: find the bridge automatically)")
    ap.add_argument("command", choices=["ping", "test", "bootloader"])
    ap.add_argument("arg", nargs="?", choices=["on", "off"], help="for 'test'")
    args = ap.parse_args(argv)

    port = args.port or xb.find_port()
    if not port:
        sys.exit("No bridge found on USB.")
    link = xb.SerialLink(port)

    if args.command == "ping":
        link.write(xb.frame(xb.T_PING))
    elif args.command == "test":
        if args.arg not in ("on", "off"):
            sys.exit("usage: bridge_cmd.py test on|off")
        link.write(xb.frame(xb.T_TEST, bytes([1 if args.arg == "on" else 0])))
    else:
        link.write(xb.frame(xb.T_BOOTLOADER))

    deadline = time.monotonic() + 1.0
    got = False
    while time.monotonic() < deadline:
        try:
            lines = link.read_lines()
        except OSError:          # the port vanishes when the board reboots into the bootloader
            break
        for line in lines:
            print(line)
            got = True
        time.sleep(0.05)
    link.close()
    if not got and args.command != "bootloader":
        print("(no answer)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
