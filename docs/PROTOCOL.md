# NXInput UDP protocol (Switch client → receiver)

Derived from the source of [Vins98/NXInput](https://github.com/Vins98/NXInput)
(`Switch_Client/source/main.c` and `Windows_Server/NXInput Server/Form1.cs`).
Confirmed on hardware (Switch Lite → Mac): handshake, button and stick packets
decode as described. Not individually checked: right stick, ZL/ZR, stick clicks.

## Transport

- UDP, IPv4, **port 8192** on both sides (hard-coded in the client).
- The Switch client sends to the receiver's IP, which the user types in on the Switch.
  The default shown on the Switch is 192.168.1.255, so enter the Mac's real IP.
- The client uses one unbound socket (ephemeral source port) for the handshake
  and for all later packets.
- No encryption, no authentication, no sequence numbers, no checksums.

## 1. Handshake (required)

The client will not start sending input until it gets a reply.

1. Client sends the ASCII string `xbox_switch` plus a trailing NUL (12 bytes) to
   `<receiver-ip>:8192`. It repeats this about every 100 ms, for up to 15 s.
2. Receiver must reply to the sender's address with the ASCII string `xbox` plus
   a trailing NUL (5 bytes: `78 62 6F 78 00`).
3. The client takes the reply's source IP as the peer address and then sends all
   input to that IP on port 8192.

If no reply arrives within 15 s the client shows "Connection timed out" and
returns to the IP screen. The reference server only checks that the received text
contains `xbox_switch`.

## 2. Input packets

Two packet shapes, distinguished by the first byte (the "code").

### Button packet (2 bytes), sent only on change

| Byte | Meaning                          |
|------|----------------------------------|
| 0    | Button code (table below)        |
| 1    | `0x01` pressed, `0x00` released  |

The client sends one when a button goes down or up. It does **not** resend while
the button is held, so a lost packet can leave a button stuck. The receiver must
handle this (release timeout / release everything on disconnect).

| Code | Switch button | Xbox 360 equivalent used by reference server |
|------|---------------|-----------------------------------------------|
| 0x01 | D-pad Up      | Up                                            |
| 0x02 | D-pad Down    | Down                                          |
| 0x03 | D-pad Left    | Left                                          |
| 0x04 | D-pad Right   | Right                                         |
| 0x05 | Minus         | Back                                          |
| 0x06 | Plus          | Start                                         |
| 0x07 | Left stick click  | Left thumb                                |
| 0x08 | Right stick click | Right thumb                               |
| 0x09 | L             | Left shoulder                                 |
| 0x0A | R             | Right shoulder                                |
| 0x0B | (unused)      | none                                          |
| 0x0C | A (right face button)  | B  (positional swap)                 |
| 0x0D | B (bottom face button) | A  (positional swap)                 |
| 0x0E | X (top face button)    | Y  (positional swap)                 |
| 0x0F | Y (left face button)   | X  (positional swap)                 |
| 0x10 | ZL            | Left trigger, digital: 0x00 or 0xFF            |
| 0x11 | ZR            | Right trigger, digital: 0x00 or 0xFF           |

Codes 0x0C–0x0F are labelled by the Switch button the client reads. The
reference server swaps them so the physical positions match an Xbox pad.

### Stick packet (5 bytes), sent continuously

| Byte | Meaning                                   |
|------|-------------------------------------------|
| 0    | `0x12` = left stick, `0x13` = right stick |
| 1–2  | X axis, signed 16-bit, **big-endian**     |
| 3–4  | Y axis, signed 16-bit, **big-endian**     |

- Range is roughly -32767..32767 (libnx stick range). Positive Y is up.
- Decode in Python with `struct.unpack(">hh", data[1:5])`.
- The client sends both stick packets on every pass of its main loop, whether or
  not the sticks moved. That makes them a de-facto heartbeat: if stick packets
  stop arriving, the Switch client has stopped or the network dropped.

## 3. Other client behaviour

- Quit: holding Plus and Minus together ends the client. It sends no goodbye packet.
- The client sets the Switch CPU clock to 204 MHz while running.
- The Switch reads the first player's pad / handheld controls (the Lite's own
  buttons and sticks).

## 4. Receiver requirements derived from the above

1. Bind UDP 8192 on all interfaces.
2. Answer `xbox_switch` with `xbox\0` to the sender's address and port.
3. After the handshake, accept packets only from the Switch's IP (the reference
   server accepts from anyone; do not copy that on a laptop).
4. Ignore packets that are not 2 bytes (codes 0x01–0x11) or 5 bytes (0x12/0x13).
5. Use stick-packet arrival as a heartbeat; release all keys after a short timeout.
6. Treat the handshake as repeatable so the Switch can reconnect.

## Licence note

The NXInput source is GPL-3.0. This document describes the wire format only; the
receiver in this repo is written from this spec and does not copy its code.
