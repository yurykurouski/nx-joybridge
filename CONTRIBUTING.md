# Contributing

Thanks for helping. Bug reports, hardware test results and pull requests are all
welcome.

## Reporting problems

Please include:

- Switch model and firmware, Atmosphère version
- board (nice!nano, clone, other nRF52840) and the contents of `INFO_UF2.TXT`
- computer and OS version
- which mode (direct USB, Wi-Fi, keyboard)
- `/switch/nx-joybridge.log` from the SD card, and a photo of the NX Joybridge screen if
  something looks wrong
- for the Mac side: output of `nicenano/check_gamecontroller 20` and
  `python3 receiver/bridge_cmd.py ping`

Reports of it working on other boards, Switch models or operating systems are
useful too.

## Pull requests

- Keep the bridge protocol (`docs/BRIDGE.md`) backward compatible, or bump it and
  update both senders (`switch_app/` and `receiver/`) in the same change.
- The Switch app's C mapping and the receiver's Python mapping must stay
  identical. Change both, then run the tests below.
- Update the docs when behaviour changes.
- Don't commit build output, personal paths or IP addresses.
- Nothing related to piracy: no bypassing console protections, no keys, firmware
  or game files, no links to such tools. See "No piracy" in the README.

## Tests

Only Python 3 and a C compiler are needed:

```sh
(cd receiver && python3 -m unittest test_receiver test_xbox_bridge)
python3 switch_app/tests/crosscheck.py
```

The same checks run in CI on every push and pull request. Hardware changes
(firmware, Switch app) also need a test on real devices; say in the PR what you
tested on.

## License

By contributing you agree that your contributions are licensed under the MIT
License (see `LICENSE`).
