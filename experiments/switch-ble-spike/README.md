# Switch BLE spike (dead end, kept for reference)

Homebrew test that tries to make the Switch advertise as a Bluetooth LE
"HID over GATT" gamepad. Result: **no-go**. See `docs/FINDINGS.md`, section 3.

It walks through btdrv's BLE GATT-server calls and prints each result, then shows
a one-screen summary (also written to `sdmc:/switch/ble_spike.log`).

Observed on a Switch Lite: server registration succeeds, but
`btdrvAddGattService` returns `0x2A071` for every server interface (0–5).

## Build

Requires devkitPro with `switch-dev` installed.

```sh
export DEVKITPRO=/opt/devkitpro
make            # produces switch-ble-spike.nro
```

Copy the `.nro` to `/switch/` on the SD card and launch it from the Homebrew Menu
(hold R while starting a game). Press + to quit; it cleans up Bluetooth state.
