## Install

You need a Switch with custom firmware (Atmosphère), an nRF52840 board such as a
nice!nano with the Adafruit UF2 bootloader, and a Mac with Bluetooth. See the
[README](https://github.com/yurykurouski/nx-joybridge#readme) for details.

1. **Flash the board.** Double-tap its reset button so the `NICENANO` drive
   appears, then copy `xbox_bridge.uf2` onto it. The board reboots by itself.
2. **Pair it with the Mac.** System Settings → Bluetooth → "Xbox Wireless
   Controller" → Connect.
3. **Install the app on the Switch.** Copy `nx-joybridge.nro` to `/switch/` on the SD card.
4. **Play.** Start NX Joybridge from the Homebrew Menu and plug the board into the
   Switch with a USB-C cable.

Optional: check the downloads against `SHA256SUMS.txt` with `shasum -a 256 -c SHA256SUMS.txt`.

## Changes

