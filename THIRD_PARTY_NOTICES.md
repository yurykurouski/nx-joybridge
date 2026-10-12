# Third-party notices

nx-joybridge is MIT licensed (see `LICENSE`). It contains or builds on the
following third-party work.

## Included in this repository

### ESP32-BLE-CompositeHID (MIT)

The Xbox Wireless Controller HID report descriptor, report IDs and device IDs in
`nicenano/xbox_bridge/xbox_bridge.ino` are ported from
[ESP32-BLE-CompositeHID](https://github.com/Mystfit/ESP32-BLE-CompositeHID).

```
Software License Agreement (MIT License)

Copyright (c) 2021 lemmingDev - https://github.com/lemmingDev

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### devkitPro Switch application template (public domain)

`switch_app/Makefile` and `experiments/switch-ble-spike/Makefile` are based on the
application template from devkitPro's
[switch-examples](https://github.com/switchbrew/switch-examples), released under the
Public Domain Mark.

## Not included, but referenced

### NXInput (GPL-3.0)

[NXInput](https://github.com/Vins98/NXInput) is the Switch client used by the
Wi-Fi mode. It is not part of this repository. `docs/PROTOCOL.md` describes its
network protocol, and `receiver/` is an independent implementation of that
protocol; no NXInput code is copied here.

## Linked into binaries you build

These are fetched by the toolchains, not stored in this repository. If you
distribute prebuilt binaries, include their notices:

| Binary | Component | License |
|--------|-----------|---------|
| `nx-joybridge.nro`, BLE spike `.nro` | [libnx](https://github.com/switchbrew/libnx) | ISC |
| `xbox_bridge.uf2` | [Adafruit nRF52 Arduino core](https://github.com/adafruit/Adafruit_nRF52_Arduino) | LGPL-2.1 (core), with MIT-licensed libraries such as Bluefruit and TinyUSB |
| `xbox_bridge.uf2` | Nordic SoftDevice S140 | Nordic license; already on the board with its bootloader, not part of the `.uf2` built here |

The LGPL core is linked statically into the firmware. Because the full source of
`xbox_bridge` is in this repository and builds against the public core, anyone
can rebuild the firmware against a modified core, as the LGPL requires.

Python dependencies (installed by you, not vendored): pynput (LGPL-3.0), pyserial (BSD-3-Clause).

## Trademarks

Nintendo Switch is a trademark of Nintendo. Xbox is a trademark of Microsoft.
World of Warcraft is a trademark of Blizzard Entertainment. nice!nano is a product
of Nice Keyboards. This project is not affiliated with or endorsed by any of them.
