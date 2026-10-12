#include "xbox_report.h"

#include <string.h>

#define XBOX_A        0x0001
#define XBOX_B        0x0002
#define XBOX_X        0x0008
#define XBOX_Y        0x0010
#define XBOX_LB       0x0040
#define XBOX_RB       0x0080
#define XBOX_VIEW     0x0400
#define XBOX_MENU     0x0800
#define XBOX_HOME     0x1000
#define XBOX_LS       0x2000
#define XBOX_RS       0x4000
#define TRIGGER_MAX   1023

static uint16_t axis(int32_t value, bool invert) {
    int32_t v = 0x8000 + (invert ? -value : value);
    if (v < 0) v = 0;
    if (v > 0xFFFF) v = 0xFFFF;
    return (uint16_t)v;
}

static uint8_t hat(bool up, bool down, bool left, bool right) {
    if (up && down) up = down = false;       // opposite directions cancel
    if (left && right) left = right = false;
    if (up && right) return 2;
    if (down && right) return 4;
    if (down && left) return 6;
    if (up && left) return 8;
    if (up) return 1;
    if (right) return 3;
    if (down) return 5;
    if (left) return 7;
    return 0;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

void xbox_report_build(const SwitchInput *in, bool invert_y, uint8_t out[XBOX_REPORT_SIZE]) {
    uint16_t buttons = 0;
    if (in->b) buttons |= XBOX_A;
    if (in->a) buttons |= XBOX_B;
    if (in->y) buttons |= XBOX_X;
    if (in->x) buttons |= XBOX_Y;
    if (in->l) buttons |= XBOX_LB;
    if (in->r) buttons |= XBOX_RB;
    if (in->minus) buttons |= XBOX_VIEW;
    if (in->plus) buttons |= XBOX_MENU;
    if (in->home) buttons |= XBOX_HOME;
    if (in->stick_l) buttons |= XBOX_LS;
    if (in->stick_r) buttons |= XBOX_RS;

    put16(out + 0, axis(in->lx, false));
    put16(out + 2, axis(in->ly, invert_y));
    put16(out + 4, axis(in->rx, false));
    put16(out + 6, axis(in->ry, invert_y));
    put16(out + 8, in->zl ? TRIGGER_MAX : 0);
    put16(out + 10, in->zr ? TRIGGER_MAX : 0);
    out[12] = hat(in->up, in->down, in->left, in->right);
    put16(out + 13, buttons);
    out[15] = 0;   // share button: the Switch has none to map
}

size_t bridge_frame(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out) {
    uint8_t chk = type ^ len;
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = type;
    out[3] = len;
    for (uint8_t i = 0; i < len; i++) {
        out[4 + i] = payload[i];
        chk ^= payload[i];
    }
    out[4 + len] = chk;
    return (size_t)len + 5;
}
