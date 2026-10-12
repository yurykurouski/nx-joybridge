# nice!nano bridge: Mac → USB → Bluetooth Xbox controller

The receiver (`receiver/nxinput_receiver.py --gamepad`) turns Switch input into
an Xbox controller report and sends it over USB serial to the nice!nano running
`nicenano/xbox_bridge/`. The board forwards it over Bluetooth LE as an
Xbox Wireless Controller (Series X, model 1914), which macOS treats as a real
controller.

## Senders

Two programs speak this protocol to the bridge:

- `receiver/nxinput_receiver.py --gamepad` on the Mac (Wi-Fi mode), over the Mac's USB serial port.
- `switch_app/` (NX Remote) on the Switch (direct USB mode). The Lite is the USB host;
  the app claims the nice!nano's CDC data interface with libnx `usbHs`, writes frames
  to its bulk OUT endpoint, and raises DTR so the bridge's status replies come back.
  Its C mapping is checked byte-for-byte against the Python one (`switch_app/tests/crosscheck.py`).

## Serial frames (host → bridge)

USB CDC serial; the baud rate setting is ignored.

```
A5 5A  TYPE  LEN  PAYLOAD[LEN]  CHK        CHK = XOR of TYPE, LEN and every PAYLOAD byte
```

| TYPE | Name | LEN | Payload / effect |
|------|------|-----|------------------|
| 0x01 | report | 16 | Xbox input report (below); becomes the controller state |
| 0x02 | test | 1 | `1` = built-in test pattern on, `0` = off (back to neutral) |
| 0x03 | bootloader | 0 | reboot into the UF2 bootloader (`NICENANO` drive) for flashing |
| 0x04 | ping | 0 | answers `#PONG connected=0\|1` (1 = a Bluetooth host is connected) |

The parser resyncs on the next `A5 5A` after any bad byte; a frame with a wrong
checksum is dropped and reported as `#ERR checksum`.

## Status lines (bridge → Mac)

Text lines starting with `#`: `#READY xbox_bridge`, `#ACTIVE` (first report
received), `#CONNECTED <peer>`, `#DISCONNECTED reason=0x..`, `#FAILSAFE neutral`,
`#PONG ...`, `#TEST on|off`, `#BOOTLOADER`, `#ERR ...`. The receiver logs them as
`bridge: ...`.

## Xbox input report (16 bytes, little-endian)

| Offset | Size | Field | Values |
|--------|------|-------|--------|
| 0 | 2 | left stick X | 0..65535, centre 0x8000, right = high |
| 2 | 2 | left stick Y | 0..65535, centre 0x8000, **up = low** (HID convention) |
| 4 | 2 | right stick X | as above |
| 6 | 2 | right stick Y | as above |
| 8 | 2 | left trigger | 0..1023 |
| 10 | 2 | right trigger | 0..1023 |
| 12 | 1 | d-pad (hat) | 0 = centre, 1..8 = N, NE, E, SE, S, SW, W, NW |
| 13 | 2 | buttons | A 0x0001, B 0x0002, X 0x0008, Y 0x0010, LB 0x0040, RB 0x0080, View 0x0400, Menu 0x0800, Xbox 0x1000, LS 0x2000, RS 0x4000 |
| 15 | 1 | share | bit 0 |

## Switch → Xbox mapping (receiver)

Positional: the button in the same place on the pad.

| Switch | Xbox | | Switch | Xbox |
|--------|------|-|--------|------|
| B (bottom) | A | | L / R | LB / RB |
| A (right) | B | | ZL / ZR | left / right trigger, full (the Switch's are digital) |
| Y (left) | X | | − / + | View / Menu |
| X (top) | Y | | stick clicks | LS / RS |
| D-pad | d-pad | | sticks | sticks (Y flipped to HID convention; `--no-invert-y` to disable) |

## Safety

- **Bridge failsafe:** no report for 500 ms → neutral, `#FAILSAFE neutral`.
- **Receiver keepalive:** resends the current report every 150 ms, so the failsafe
  only fires when the receiver stops or the USB link breaks.
- **Receiver timeout:** no packets from the Switch for 1 s → neutral.
- **Receiver shutdown** (Ctrl-C or `kill`): sends a neutral report before exiting.

## Verification (2026-10-12)

Simulated Switch (`receiver/fake_switch.py`) → receiver → USB → bridge →
Bluetooth → Apple GameController framework (`nicenano/check_gamecontroller`):
every face button, both bumpers and triggers, View/Menu, both stick clicks, all
four d-pad directions and both sticks in both axes arrived with the expected
mapping and direction.

Real Switch Lite (NXInput over Wi-Fi, same chain): face buttons, ZL/ZR as full
triggers, L/R (also held together), −/+, both stick clicks, all four d-pad directions and both sticks
(full circles with smooth analog values) arrived at GameController.

Direct USB mode (NX Remote app on the Lite, USB-C to USB-C cable to the nice!nano,
nothing running on the Mac): A/B/X/Y, L/R, ZL/ZR, −/+, both stick clicks, all four
d-pad directions and both sticks arrived at GameController.

Not yet measured: end-to-end latency.
