// Switch input -> Xbox controller report and bridge frames.
// Pure C (no libnx), so it can be tested on a desktop against the Python
// receiver's mapping (receiver/xbox_bridge.py). Format: docs/BRIDGE.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool a, b, x, y;           // face buttons, Switch labels
    bool l, r, zl, zr;
    bool minus, plus, stick_l, stick_r;
    bool up, down, left, right; // d-pad
    bool home;                  // Xbox button (NX Joybridge: on-screen touch button)
    int32_t lx, ly, rx, ry;     // sticks, -32767..32767, +y = up (libnx convention)
} SwitchInput;

#define XBOX_REPORT_SIZE 16

// Bridge frame types
#define BRIDGE_T_REPORT     0x01
#define BRIDGE_T_TEST       0x02
#define BRIDGE_T_BOOTLOADER 0x03
#define BRIDGE_T_PING       0x04
#define BRIDGE_FRAME_MAX    (5 + 32)

// Builds the 16-byte Xbox input report. Positional button mapping: the Switch's
// bottom face button (B) is Xbox A, right (A) is Xbox B, left (Y) is X, top (X) is Y.
// invert_y flips the stick Y axes to the HID convention (up = low values).
void xbox_report_build(const SwitchInput *in, bool invert_y, uint8_t out[XBOX_REPORT_SIZE]);

// Writes "A5 5A TYPE LEN PAYLOAD CHK" to out (at least 5 + len bytes) and returns its length.
size_t bridge_frame(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out);
