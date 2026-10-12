// NX Remote: Switch Lite -> USB -> nice!nano -> Bluetooth LE Xbox controller.
//
// Reads the Lite's own controls and sends the bridge's Xbox report frames
// (docs/BRIDGE.md) to a nice!nano running nicenano/xbox_bridge, over USB with
// the Switch as USB host. The nice!nano forwards them over Bluetooth.
//
// Threads:
//   worker  - USB discovery, pad polling every ~4 ms, sending reports
//   main    - status screen (fixed lines, no scrolling), quit handling
// Every setup step is also logged to sdmc:/switch/nx-remote.log.
//
// Hold + and - together for 1 second to quit.

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "xbox_report.h"

#define BRIDGE_VID        0x239A          // Adafruit USB IDs used by the bridge firmware
#define LOG_PATH          "sdmc:/switch/nx-remote.log"
#define POLL_NS           (4ULL * 1000000)
#define KEEPALIVE_NS      (150ULL * 1000000)
#define PING_NS           (2000ULL * 1000000)
#define RESCAN_NS         (500ULL * 1000000)
#define QUIT_HOLD_NS      (1000ULL * 1000000)

// ---------------------------------------------------------------------------
// Shared state (worker writes, UI reads), guarded by g.lock
// ---------------------------------------------------------------------------
static struct {
    Mutex lock;
    bool  usb_ready;
    char  usb_msg[96];
    char  bridge_msg[64];
    u64   frames_sent;
    u64   send_errors;
    u8    report[XBOX_REPORT_SIZE];
    SwitchInput input;
    bool  home_touch;             // on-screen XBOX button is held (set by main)
    float quit_progress;          // 0..1 while + and - are held
    bool  quit;                   // set by the worker when the quit combo completes
    volatile bool stop;           // set by main to end the worker
} g;

static FILE *g_log;
static Mutex g_log_lock;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    mutexLock(&g_log_lock);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    mutexUnlock(&g_log_lock);
}

static void set_usb_msg(bool ready, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mutexLock(&g.lock);
    bool changed = strcmp(buf, g.usb_msg) != 0;
    g.usb_ready = ready;
    strcpy(g.usb_msg, buf);
    mutexUnlock(&g.lock);
    if (changed) logf_("USB: %s", buf);
}

// ---------------------------------------------------------------------------
// USB link to the bridge
// ---------------------------------------------------------------------------
typedef struct {
    bool connected;
    UsbHsClientIfSession data_if, comm_if;
    UsbHsClientEpSession ep_out, ep_in;
    bool comm_acquired, in_pending;
    u8 *out_buf, *in_buf, *ctrl_buf;   // 0x1000-aligned, as usbhs requires
    char line[64];
    size_t line_len;
} Link;

static bool find_bulk(struct usb_endpoint_descriptor *descs, struct usb_endpoint_descriptor **out) {
    for (int i = 0; i < 15; i++) {
        if (descs[i].bLength != 0 && (descs[i].bmAttributes & 0x3) == USB_TRANSFER_TYPE_BULK) {
            *out = &descs[i];
            return true;
        }
    }
    return false;
}

static void link_close(Link *lk) {
    if (lk->connected || usbHsIfIsActive(&lk->data_if)) {
        usbHsEpClose(&lk->ep_out);
        usbHsEpClose(&lk->ep_in);
        usbHsIfClose(&lk->data_if);
    }
    if (lk->comm_acquired)
        usbHsIfClose(&lk->comm_if);
    lk->connected = lk->comm_acquired = lk->in_pending = false;
    lk->line_len = 0;
    memset(&lk->data_if, 0, sizeof(lk->data_if));
    memset(&lk->comm_if, 0, sizeof(lk->comm_if));
}

static bool link_send(Link *lk, uint8_t type, const uint8_t *payload, uint8_t len) {
    size_t n = bridge_frame(type, payload, len, lk->out_buf);
    u32 done = 0;
    Result rc = usbHsEpPostBuffer(&lk->ep_out, lk->out_buf, (u32)n, &done);
    if (R_FAILED(rc) || done != n) {
        logf_("send failed: rc=0x%X transferred=%u of %u", rc, done, (unsigned)n);
        return false;
    }
    return true;
}

static void link_post_read(Link *lk) {
    u32 xfer_id = 0;
    eventClear(usbHsEpGetXferEvent(&lk->ep_in));
    Result rc = usbHsEpPostBufferAsync(&lk->ep_in, lk->in_buf, lk->ep_in.desc.wMaxPacketSize, 0, &xfer_id);
    lk->in_pending = R_SUCCEEDED(rc);
    if (R_FAILED(rc)) logf_("read post failed: rc=0x%X", rc);
}

// Collects '#...' status lines the bridge sends back (needs DTR, see link_open).
static void link_poll_read(Link *lk) {
    if (!lk->in_pending || R_FAILED(eventWait(usbHsEpGetXferEvent(&lk->ep_in), 0)))
        return;
    UsbHsXferReport reports[4];
    u32 count = 0;
    if (R_SUCCEEDED(usbHsEpGetXferReport(&lk->ep_in, reports, 4, &count))) {
        for (u32 i = 0; i < count; i++) {
            if (R_FAILED(reports[i].res)) continue;
            for (u32 j = 0; j < reports[i].transferredSize; j++) {
                char ch = (char)lk->in_buf[j];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    lk->line[lk->line_len] = 0;
                    if (lk->line_len) {
                        logf_("bridge: %s", lk->line);
                        mutexLock(&g.lock);
                        snprintf(g.bridge_msg, sizeof(g.bridge_msg), "%s", lk->line);
                        mutexUnlock(&g.lock);
                    }
                    lk->line_len = 0;
                } else if (lk->line_len < sizeof(lk->line) - 1) {
                    lk->line[lk->line_len++] = ch;
                }
            }
        }
    }
    link_post_read(lk);
}

static bool link_open(Link *lk) {
    UsbHsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.Flags = UsbHsInterfaceFilterFlags_idVendor;
    filter.idVendor = BRIDGE_VID;

    static UsbHsInterface ifs[8];
    s32 total = 0;
    memset(ifs, 0, sizeof(ifs));
    Result rc = usbHsQueryAvailableInterfaces(&filter, ifs, sizeof(ifs), &total);
    if (R_FAILED(rc)) {
        set_usb_msg(false, "query failed rc=0x%X", rc);
        return false;
    }

    int data_i = -1, comm_i = -1;
    for (int i = 0; i < total; i++) {
        u8 cls = ifs[i].inf.interface_desc.bInterfaceClass;
        if (cls == USB_CLASS_DATA && data_i < 0) data_i = i;
        if (cls == USB_CLASS_COMM && comm_i < 0) comm_i = i;
    }
    if (data_i < 0) {
        s32 all = 0;
        static UsbHsInterface any[8];
        memset(any, 0, sizeof(any));
        if (R_SUCCEEDED(usbHsQueryAllInterfaces(&filter, any, sizeof(any), &all)) && all > 0)
            set_usb_msg(false, "nice!nano present but not available (%d ifs)", (int)all);
        else
            set_usb_msg(false, "waiting for nice!nano on USB...");
        return false;
    }
    logf_("found %d interface(s), data=%d comm=%d path=%s", (int)total, data_i, comm_i, ifs[data_i].pathstr);

    rc = usbHsAcquireUsbIf(&lk->data_if, &ifs[data_i]);
    if (R_FAILED(rc)) {
        set_usb_msg(false, "acquire data interface failed rc=0x%X", rc);
        return false;
    }

    struct usb_endpoint_descriptor *out_desc = NULL, *in_desc = NULL;
    if (!find_bulk(lk->data_if.inf.inf.output_endpoint_descs, &out_desc) ||
        !find_bulk(lk->data_if.inf.inf.input_endpoint_descs, &in_desc)) {
        set_usb_msg(false, "bulk endpoints not found");
        usbHsIfClose(&lk->data_if);
        return false;
    }
    rc = usbHsIfOpenUsbEp(&lk->data_if, &lk->ep_out, 1, out_desc->wMaxPacketSize, out_desc);
    if (R_SUCCEEDED(rc))
        rc = usbHsIfOpenUsbEp(&lk->data_if, &lk->ep_in, 1, in_desc->wMaxPacketSize, in_desc);
    if (R_FAILED(rc)) {
        set_usb_msg(false, "open endpoints failed rc=0x%X", rc);
        usbHsEpClose(&lk->ep_out);
        usbHsIfClose(&lk->data_if);
        return false;
    }
    logf_("endpoints open: out=0x%02X in=0x%02X max=%u", out_desc->bEndpointAddress,
          in_desc->bEndpointAddress, out_desc->wMaxPacketSize);

    // Optional: raise DTR so the bridge's status replies come back (its serial
    // port drops output until the host "opens" it). Reports work without it.
    u16 comm_num = 0;
    u32 done = 0;
    if (comm_i >= 0) {
        comm_num = ifs[comm_i].inf.interface_desc.bInterfaceNumber;
        rc = usbHsAcquireUsbIf(&lk->comm_if, &ifs[comm_i]);
        lk->comm_acquired = R_SUCCEEDED(rc);
        logf_("acquire comm interface %u: rc=0x%X", comm_num, rc);
    }
    UsbHsClientIfSession *ctrl_if = lk->comm_acquired ? &lk->comm_if : &lk->data_if;
    rc = usbHsIfCtrlXfer(ctrl_if, 0x21, 0x22 /* SET_CONTROL_LINE_STATE */, 0x0003 /* DTR|RTS */,
                         comm_num, 0, lk->ctrl_buf, &done);
    logf_("set DTR: rc=0x%X", rc);

    lk->connected = true;
    link_post_read(lk);
    link_send(lk, BRIDGE_T_PING, NULL, 0);
    set_usb_msg(true, "connected (%s)", ifs[data_i].pathstr);
    return true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
static SwitchInput read_input(PadState *pad) {
    u64 h = padGetButtons(pad);
    HidAnalogStickState ls = padGetStickPos(pad, 0), rs = padGetStickPos(pad, 1);
    SwitchInput in = {
        .a = h & HidNpadButton_A, .b = h & HidNpadButton_B,
        .x = h & HidNpadButton_X, .y = h & HidNpadButton_Y,
        .l = h & HidNpadButton_L, .r = h & HidNpadButton_R,
        .zl = h & HidNpadButton_ZL, .zr = h & HidNpadButton_ZR,
        .minus = h & HidNpadButton_Minus, .plus = h & HidNpadButton_Plus,
        .stick_l = h & HidNpadButton_StickL, .stick_r = h & HidNpadButton_StickR,
        .up = h & HidNpadButton_Up, .down = h & HidNpadButton_Down,
        .left = h & HidNpadButton_Left, .right = h & HidNpadButton_Right,
        .lx = ls.x, .ly = ls.y, .rx = rs.x, .ry = rs.y,
    };
    return in;
}

static void worker(void *arg) {
    (void)arg;
    PadState pad;
    padInitializeDefault(&pad);

    Link lk;
    memset(&lk, 0, sizeof(lk));
    lk.out_buf = memalign(0x1000, 0x1000);
    lk.in_buf = memalign(0x1000, 0x1000);
    lk.ctrl_buf = memalign(0x1000, 0x1000);
    if (!lk.out_buf || !lk.in_buf || !lk.ctrl_buf) {
        set_usb_msg(false, "out of memory");
        return;
    }

    u8 last_sent[XBOX_REPORT_SIZE] = {0};
    u64 last_send = 0, last_ping = 0, last_scan = 0, quit_since = 0;

    while (!g.stop) {
        u64 now = armTicksToNs(armGetSystemTick());

        // USB: connect when the bridge appears, drop the link when it goes away
        if (!lk.connected && now - last_scan >= RESCAN_NS) {
            last_scan = now;
            if (link_open(&lk)) {
                last_send = 0;                  // send the current state right away
                last_ping = now;
            }
        }
        if (lk.connected && R_SUCCEEDED(eventWait(usbHsGetInterfaceStateChangeEvent(), 0))) {
            eventClear(usbHsGetInterfaceStateChangeEvent());
            s32 n = 0;
            static UsbHsInterface acquired[8];
            if (R_SUCCEEDED(usbHsQueryAcquiredInterfaces(acquired, sizeof(acquired), &n)) && n == 0) {
                logf_("bridge unplugged");
                link_close(&lk);
                set_usb_msg(false, "nice!nano unplugged, waiting...");
            }
        }

        // Controls
        padUpdate(&pad);
        SwitchInput in = read_input(&pad);
        mutexLock(&g.lock);
        in.home = g.home_touch;
        mutexUnlock(&g.lock);
        u8 rep[XBOX_REPORT_SIZE];
        xbox_report_build(&in, true, rep);

        bool quit_combo = in.plus && in.minus;
        if (!quit_combo) quit_since = 0;
        else if (!quit_since) quit_since = now;
        float progress = quit_since ? (float)(now - quit_since) / (float)QUIT_HOLD_NS : 0.0f;

        // Send on change, plus keepalive so the bridge's 500 ms failsafe stays off
        if (lk.connected) {
            if (memcmp(rep, last_sent, sizeof(rep)) != 0 || now - last_send >= KEEPALIVE_NS) {
                if (link_send(&lk, BRIDGE_T_REPORT, rep, XBOX_REPORT_SIZE)) {
                    memcpy(last_sent, rep, sizeof(rep));
                    last_send = now;
                    mutexLock(&g.lock);
                    g.frames_sent++;
                    mutexUnlock(&g.lock);
                } else {
                    mutexLock(&g.lock);
                    g.send_errors++;
                    mutexUnlock(&g.lock);
                    link_close(&lk);
                    set_usb_msg(false, "send failed, reconnecting...");
                }
            }
            if (lk.connected && now - last_ping >= PING_NS) {
                last_ping = now;
                link_send(&lk, BRIDGE_T_PING, NULL, 0);
            }
            if (lk.connected)
                link_poll_read(&lk);
        }

        mutexLock(&g.lock);
        g.input = in;
        memcpy(g.report, rep, sizeof(rep));
        g.quit_progress = progress > 1.0f ? 1.0f : progress;
        if (progress >= 1.0f) g.quit = true;
        mutexUnlock(&g.lock);

        svcSleepThread(POLL_NS);
    }

    // Leave the controller neutral before letting go of the USB link
    if (lk.connected) {
        SwitchInput neutral;
        memset(&neutral, 0, sizeof(neutral));
        u8 rep[XBOX_REPORT_SIZE];
        xbox_report_build(&neutral, true, rep);
        link_send(&lk, BRIDGE_T_REPORT, rep, XBOX_REPORT_SIZE);
        logf_("sent neutral report on exit");
    }
    link_close(&lk);
    free(lk.out_buf);
    free(lk.in_buf);
    free(lk.ctrl_buf);
}

// ---------------------------------------------------------------------------
// Status screen (80x45 text console, fixed positions, redrawn every frame)
// ---------------------------------------------------------------------------
#define FG_WHITE   "\x1b[37m"
#define FG_GREY    "\x1b[37;2m"
#define FG_GREEN   "\x1b[32;1m"
#define FG_YELLOW  "\x1b[33;1m"
#define FG_RED     "\x1b[31;1m"
#define FG_CYAN    "\x1b[36;1m"
#define PRESSED    "\x1b[30;42m"     // black on green
#define RESET      "\x1b[0m"

static void at(int row, int col, const char *color, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("\x1b[%d;%dH%s%s" RESET, row, col, color, buf);
}

static void clear_row(int row) {
    printf("\x1b[%d;1H\x1b[K", row);
}

// One button cap: highlighted while pressed
static void cap(int row, int col, bool on, const char *label) {
    at(row, col, on ? PRESSED : FG_WHITE, "%s", label);
}

// A stick as a 9x5 box with a dot where the stick points; frame lights up on click
static void stick_box(int row, int col, int32_t x, int32_t y, bool click, const char *name) {
    const int W = 9, H = 5;
    int32_t xs = x < -32767 ? -32767 : (x > 32767 ? 32767 : x);
    int32_t ys = y < -32767 ? -32767 : (y > 32767 ? 32767 : y);
    int cx = (int)((xs + 32767) * (W - 1) / 65534);  // 0 .. W-1, centre W/2
    int cy = (int)((32767 - ys) * (H - 1) / 65534);  // +y is up, so it maps to the top row
    const char *frame = click ? FG_GREEN : FG_GREY;
    at(row, col, frame, "+---------+");
    for (int r = 0; r < H; r++) {
        char inner[16];
        for (int c = 0; c < W; c++) inner[c] = ' ';
        inner[W / 2] = (r == H / 2) ? '+' : ' ';
        if (r == cy) inner[cx] = 'O';
        inner[W] = 0;
        at(row + 1 + r, col, frame, "|");
        at(row + 1 + r, col + 1, (r == cy) ? FG_CYAN : FG_GREY, "%s", inner);
        at(row + 1 + r, col + 10, frame, "|");
    }
    at(row + 6, col, frame, "+---------+");
    at(row + 7, col + 1, FG_GREY, "%s %+4d %+4d", name, (int)(x * 100 / 32767), (int)(y * 100 / 32767));
}

// On-screen touch buttons
#define TB_ROW      40
#define TB_HEIGHT   5
#define TB_WIDTH    20
#define TB_XBOX_COL 3
#define TB_SCRN_COL 58

static bool in_box(u32 px, u32 py, int col) {
    int c = (int)(px / 16) + 1, r = (int)(py / 16) + 1;   // touch pixels -> console cell
    return r >= TB_ROW - 1 && r <= TB_ROW + TB_HEIGHT &&    // one cell of slack around the box
           c >= col - 1 && c <= col + TB_WIDTH;
}

static void touch_button(int col, const char *color, const char *label) {
    char edge[TB_WIDTH + 1], mid[TB_WIDTH + 1], lab[TB_WIDTH + 1];
    memset(edge, '-', TB_WIDTH); edge[0] = edge[TB_WIDTH - 1] = '+'; edge[TB_WIDTH] = 0;
    memset(mid, ' ', TB_WIDTH);  mid[0] = mid[TB_WIDTH - 1] = '|';   mid[TB_WIDTH] = 0;
    memcpy(lab, mid, sizeof(lab));
    size_t n = strlen(label);
    memcpy(lab + (TB_WIDTH - n) / 2, label, n);
    at(TB_ROW + 0, col, color, "%s", edge);
    at(TB_ROW + 1, col, color, "%s", mid);
    at(TB_ROW + 2, col, color, "%s", lab);
    at(TB_ROW + 3, col, color, "%s", mid);
    at(TB_ROW + 4, col, color, "%s", edge);
}

typedef struct {
    u32  battery;          // percent
    bool charging;
    bool battery_ok;
    bool display_off;
    bool display_ctl_ok;   // lbl service available
    u64  rate;             // reports per second
    bool home_held;        // XBOX touch button held
} UiExtra;

static void draw(const UiExtra *ex) {
    mutexLock(&g.lock);
    bool ready = g.usb_ready;
    char usb[96], bridge[64];
    strcpy(usb, g.usb_msg);
    strcpy(bridge, g.bridge_msg);
    u64 sent = g.frames_sent, errs = g.send_errors;
    SwitchInput in = g.input;
    float qp = g.quit_progress;
    mutexUnlock(&g.lock);

    // Header: title, clock, battery
    // Switch clock in the Switch's own timezone setting (time()/localtime() would give UTC)
    u64 ts = 0;
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo cinfo;
    bool clock_ok = R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &ts)) &&
                    R_SUCCEEDED(timeToCalendarTimeWithMyRule(ts, &ct, &cinfo));
    at(2, 3, FG_CYAN, "NX REMOTE");
    at(3, 3, FG_GREY, "Switch Lite -> USB -> nice!nano -> Bluetooth");
    if (clock_ok) at(2, 55, FG_WHITE, "%02u:%02u", ct.hour, ct.minute);
    else          at(2, 55, FG_GREY, "--:--");
    if (ex->battery_ok) {
        const char *bc = ex->battery > 50 ? FG_GREEN : ex->battery > 20 ? FG_YELLOW : FG_RED;
        char bar[11];
        int filled = (int)((ex->battery + 5) / 10);
        for (int i = 0; i < 10; i++) bar[i] = i < filled ? '#' : '-';
        bar[10] = 0;
        at(2, 63, bc, "[%s]%3u%%", bar, ex->battery);
        at(3, 63, ex->charging ? FG_GREEN : FG_GREY, "%s", ex->charging ? "charging        " : "on battery      ");
    } else {
        at(2, 63, FG_GREY, "battery n/a");
    }
    at(4, 3, FG_GREY, "------------------------------------------------------------------------------");

    // Link panel
    const char *bt_text, *bt_color;
    bool bt_up = strstr(bridge, "connected=1") != NULL;
    if (bt_up)                              { bt_text = "Mac connected";                      bt_color = FG_GREEN; }
    else if (strstr(bridge, "connected=0")) { bt_text = "not connected - pair \"Xbox Wireless Controller\""; bt_color = FG_YELLOW; }
    else                                    { bt_text = "status unknown (no reply from bridge)"; bt_color = FG_GREY; }
    clear_row(6);
    at(6, 3, FG_GREY, "USB");
    at(6, 15, ready ? FG_GREEN : FG_YELLOW, "%s %.62s", ready ? "*" : "o", usb);
    clear_row(7);
    at(7, 3, FG_GREY, "Bluetooth");
    at(7, 15, bt_color, "%s %s", bt_up ? "*" : "o", bt_text);
    clear_row(8);
    at(8, 3, FG_GREY, "Reports");
    at(8, 15, FG_WHITE, "%llu sent   %llu/s   ", (unsigned long long)sent, (unsigned long long)ex->rate);
    at(8, 40, errs ? FG_RED : FG_GREY, "%llu errors", (unsigned long long)errs);

    // Controller drawing
    const int top = 11;
    cap(top + 0, 10, in.zl, " ZL ");
    cap(top + 0, 60, in.zr, " ZR ");
    cap(top + 1, 10, in.l,  "  L  ");
    cap(top + 1, 59, in.r,  "  R  ");
    cap(top + 3, 24, in.minus, " - ");
    cap(top + 3, 47, in.plus,  " + ");
    stick_box(top + 4, 8, in.lx, in.ly, in.stick_l, "L");
    // face buttons (Switch labels; Xbox equivalents in brackets)
    cap(top + 5, 60, in.x, " X ");
    cap(top + 7, 55, in.y, " Y ");
    cap(top + 7, 65, in.a, " A ");
    cap(top + 9, 60, in.b, " B ");
    at(top + 11, 54, FG_GREY, "Xbox: X=Y Y=X A=B B=A");
    // d-pad
    cap(top + 14, 15, in.up,    " ^ ");
    cap(top + 15, 11, in.left,  " < ");
    cap(top + 15, 19, in.right, " > ");
    cap(top + 16, 15, in.down,  " v ");
    stick_box(top + 13, 54, in.rx, in.ry, in.stick_r, "R");

    // Footer
    clear_row(36);
    if (qp > 0.0f)
        at(36, 3, FG_YELLOW, "Quitting... keep holding + and -  (%d%%)", (int)(qp * 100));
    else
        at(36, 3, FG_GREY, "Hold + and - together for 1 second to quit.");
    clear_row(37);
    at(37, 3, FG_GREY, "Touch XBOX for the Xbox button. SCREEN OFF darkens the display; tap to wake.");
    at(38, 3, FG_GREY, "Log: %s", LOG_PATH);

    touch_button(TB_XBOX_COL, ex->home_held ? PRESSED : FG_WHITE, "XBOX");
    touch_button(TB_SCRN_COL, ex->display_ctl_ok ? FG_WHITE : FG_GREY,
                 ex->display_ctl_ok ? "SCREEN OFF" : "SCREEN OFF n/a");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    hidInitializeTouchScreen();
    mutexInit(&g.lock);
    mutexInit(&g_log_lock);
    strcpy(g.usb_msg, "starting...");

    g_log = fopen(LOG_PATH, "w");
    logf_("NX Remote start, HOS %u.%u.%u", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
          HOSVER_MICRO(hosversionGet()));

    Result rc = appletSetAutoSleepDisabled(true);   // the Lite is used as a controller, keep it awake
    logf_("auto-sleep disabled: rc=0x%X", rc);

    UiExtra ex;
    memset(&ex, 0, sizeof(ex));
    bool psm_ok = R_SUCCEEDED(rc = psmInitialize());
    logf_("psmInitialize: rc=0x%X", rc);
    ex.display_ctl_ok = R_SUCCEEDED(rc = lblInitialize());
    logf_("lblInitialize: rc=0x%X", rc);

    rc = usbHsInitialize();
    logf_("usbHsInitialize: rc=0x%X", rc);
    if (R_FAILED(rc)) {
        at(2, 3, FG_RED, "NX Remote: usbHsInitialize failed, rc=0x%X", rc);
        at(4, 3, FG_WHITE, "The USB host service is not available to this app. Press + to quit.");
        consoleUpdate(NULL);
        PadState pad;
        padInitializeDefault(&pad);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
            consoleUpdate(NULL);
        }
        if (ex.display_ctl_ok) lblExit();
        if (psm_ok) psmExit();
        if (g_log) fclose(g_log);
        consoleExit(NULL);
        return 0;
    }

    Thread t;
    rc = threadCreate(&t, worker, NULL, NULL, 0x10000, 0x2B, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&t);
    logf_("worker thread: rc=0x%X", rc);

    consoleClear();
    u64 last_battery = 0, last_rate_t = 0, last_rate_sent = 0;
    bool touching = false, wake_touch = false;

    while (appletMainLoop()) {
        u64 now = armTicksToNs(armGetSystemTick());

        if (psm_ok && now - last_battery >= 2000ULL * 1000000) {
            last_battery = now;
            u32 pct = 0;
            PsmChargerType ct = PsmChargerType_Unconnected;
            ex.battery_ok = R_SUCCEEDED(psmGetBatteryChargePercentage(&pct));
            ex.battery = pct;
            ex.charging = R_SUCCEEDED(psmGetChargerType(&ct)) && ct != PsmChargerType_Unconnected;
        }
        if (now - last_rate_t >= 1000ULL * 1000000) {
            mutexLock(&g.lock);
            u64 sent = g.frames_sent;
            mutexUnlock(&g.lock);
            ex.rate = last_rate_t ? sent - last_rate_sent : 0;
            last_rate_sent = sent;
            last_rate_t = now;
        }

        // Touchscreen (not part of the controller, so it never reaches the game by itself):
        //  - XBOX box held      -> Xbox button held
        //  - SCREEN OFF tapped  -> display off; while off, any tap wakes it (and is not an XBOX press)
        HidTouchScreenState ts;
        memset(&ts, 0, sizeof(ts));
        bool touched = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
        bool on_xbox = false, on_screen_btn = false;
        for (s32 i = 0; touched && i < ts.count && i < 16; i++) {
            on_xbox |= in_box(ts.touches[i].x, ts.touches[i].y, TB_XBOX_COL);
            on_screen_btn |= in_box(ts.touches[i].x, ts.touches[i].y, TB_SCRN_COL);
        }
        bool new_touch = touched && !touching;
        if (new_touch && ex.display_off) {
            rc = lblSwitchBacklightOn(0);
            ex.display_off = false;
            wake_touch = true;                  // ignore this touch until the finger lifts
            logf_("display on: rc=0x%X", rc);
        } else if (new_touch && on_screen_btn && ex.display_ctl_ok) {
            rc = lblSwitchBacklightOff(0);
            ex.display_off = true;
            logf_("display off: rc=0x%X", rc);
        }
        if (!touched) wake_touch = false;
        touching = touched;
        ex.home_held = on_xbox && !wake_touch && !ex.display_off;
        mutexLock(&g.lock);
        g.home_touch = ex.home_held;
        mutexUnlock(&g.lock);

        draw(&ex);
        consoleUpdate(NULL);
        mutexLock(&g.lock);
        bool quit = g.quit;
        mutexUnlock(&g.lock);
        if (quit) break;
        svcSleepThread(50ULL * 1000000);
    }

    g.stop = true;
    threadWaitForExit(&t);
    threadClose(&t);
    usbHsExit();
    if (ex.display_ctl_ok) {
        lblSwitchBacklightOn(0);        // never leave the screen dark
        lblExit();
    }
    if (psm_ok) psmExit();
    appletSetAutoSleepDisabled(false);
    logf_("NX Remote exit");
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
