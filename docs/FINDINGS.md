# Findings: routes tried for "Switch Lite as a Mac controller"

Short record of what was tried, what worked, and why the dead ends are dead.
Details live in the Linear project *Switch Lite → macOS controller (NXInput)*.

| Route | Result |
|-------|--------|
| NXInput over Wi-Fi → keyboard events on the Mac | **Works** (hardware-tested) |
| Virtual HID gamepad created by software on macOS | **No-go** (Apple entitlement) |
| Switch itself as a Bluetooth LE gamepad | **No-go** (Switch refuses) |
| nice!nano (nRF52840) imitating an Xbox controller over BLE | **Works** (macOS, gamepad-tester.com, a game, WoW retail) |
| Switch app → USB cable → nice!nano (Lite as USB host) | **Works** (every control, no Wi-Fi, nothing on the Mac) |

## 1. Keyboard output (works)

Lite runs the NXInput homebrew client, which sends button/stick packets over UDP
(see `PROTOCOL.md`). `receiver/nxinput_receiver.py` answers the handshake and
presses keys with `pynput`. Needs Accessibility permission for the terminal.
Fine for games with key bindings; games only see a keyboard, not a controller.

## 2. Virtual gamepad on macOS (no-go for now)

- Apple's supported ways to create a HID device in software (CoreHID
  `HIDVirtualDevice`, DriverKit) need restricted entitlements, which currently
  require the paid Apple Developer Program and Apple's approval.
- Disabling SIP/AMFI avoids that, but Apple advises against it.
- Apple engineers say the GameController framework deliberately ignores virtual
  HID devices, so even an entitled virtual pad may not reach games.

## 3. Switch as a Bluetooth LE peripheral (no-go)

Experiment: `experiments/switch-ble-spike/` (libnx homebrew).

- Works from homebrew: `btdrvInitialize`, `btdrvInitializeBle`, `btdrvEnableBle`,
  `btdrvRegisterGattServer` (event: ServerRegistration result 0, status 1).
- Fails: `btdrvAddGattService` returns `0x2A071` (module 113 "Bluetooth",
  description 336, undocumented) for every server interface tried (0–5), and no
  ServerAddAttribute event ever arrives. libnx itself marks these GATT-server
  calls as "broken behaviour due to internal bugs".
- Not tried (much larger work, no known precedent): a sysmodule with Bluetooth
  MITM like MissionControl, or going through `btm`/`btmu`.

## 4. nice!nano as an Xbox controller (works)

Firmware: `nicenano/xbox_bridge/` (started as a test-pattern-only build).

- A real BLE device avoids both problems above: macOS treats it as hardware.
- macOS's GameController framework only accepts known controller models, so the
  board imitates an Xbox Wireless Controller (Series X, model 1914): HID report
  map, report IDs (input 1, output 3) and PnP ID (VID `0x045E`, PID `0x0B13`,
  version `0x0509`), ported from
  [ESP32-BLE-CompositeHID](https://github.com/Mystfit/ESP32-BLE-CompositeHID) (MIT).
- Result on macOS 27.0.1: pairs as "Xbox Wireless Controller"; GameController
  lists it as category "Xbox One" with an extended gamepad profile; test input
  arrives exactly as sent; also works on gamepad-tester.com and in a game.
- Quirk: a board previously paired as something else (here, half of a split
  keyboard) can show up under its old cached name at first. After re-flashing,
  GameController reported "Xbox Wireless Controller".
- Works in WoW retail. WoW Forever is not tested yet (not available at the time
  of writing).

## 5. Direct USB from the Switch (works)

App: `switch_app/` (NX Remote). A normal homebrew app can be a USB host through
libnx `usbHs` (official example: `examples/switch/usb/usbhs` in devkitPro), as long
as no system module has claimed the device. The nice!nano's USB serial data
interface is unclaimed, so the app acquires it, opens its bulk endpoints and
writes the same frames the Mac receiver sends. The Lite powers the nice!nano over
a plain USB-C to USB-C cable. The bridge firmware needed no change: its USB serial
accepts data without the host raising DTR (only its replies need DTR).
