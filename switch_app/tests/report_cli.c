// Desktop harness for xbox_report.c, driven by crosscheck.py.
// stdin, one case per line (bools as 0/1):
//   a b x y l r zl zr minus plus stick_l stick_r up down left right home lx ly rx ry invert_y
// stdout, one line per case: <report hex> <frame hex>
#include <stdio.h>

#include "xbox_report.h"

int main(void) {
    int f[18];
    long lx, ly, rx, ry;
    while (scanf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %ld %ld %ld %ld %d",
                 &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10],
                 &f[11], &f[12], &f[13], &f[14], &f[15], &f[16], &lx, &ly, &rx, &ry, &f[17]) == 22) {
        SwitchInput in = {
            .a = f[0], .b = f[1], .x = f[2], .y = f[3], .l = f[4], .r = f[5], .zl = f[6], .zr = f[7],
            .minus = f[8], .plus = f[9], .stick_l = f[10], .stick_r = f[11],
            .up = f[12], .down = f[13], .left = f[14], .right = f[15], .home = f[16],
            .lx = (int32_t)lx, .ly = (int32_t)ly, .rx = (int32_t)rx, .ry = (int32_t)ry,
        };
        uint8_t rep[XBOX_REPORT_SIZE], frame[BRIDGE_FRAME_MAX];
        xbox_report_build(&in, f[17], rep);
        size_t n = bridge_frame(BRIDGE_T_REPORT, rep, XBOX_REPORT_SIZE, frame);
        for (int i = 0; i < XBOX_REPORT_SIZE; i++) printf("%02x", rep[i]);
        printf(" ");
        for (size_t i = 0; i < n; i++) printf("%02x", frame[i]);
        printf("\n");
    }
    return 0;
}
