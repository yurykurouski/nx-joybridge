// BLE gamepad feasibility spike for Nintendo Switch (libnx).
//
// Question this answers: can homebrew make the Switch advertise itself as a
// Bluetooth LE "HID over GATT" gamepad that a Mac can see and connect to?
//
// It does NOT send any controller input yet. It only walks through the steps
// needed to become a BLE peripheral and prints the result code of every step
// (screen + sdmc:/switch/ble_spike.log) so a failure shows exactly where the
// Switch's Bluetooth stack refuses.
//
// Controls: + = quit (cleans up the Bluetooth state before exiting).

#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_PATH "sdmc:/switch/ble_spike.log"

static FILE *g_log;
static u8 g_server_if;        // server interface used for the GATT calls
static bool g_add_ok;         // set when a ServerAddAttribute event reports success
static bool g_connected;      // set when a client (the Mac) connects

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    consoleUpdate(NULL);
}

#define MAX_STEPS 40
static struct { char name[56]; Result rc; } g_steps[MAX_STEPS];
static int g_nsteps;
static int g_evcount[16];         // distinct events seen, by BtdrvBleEventType
static int g_suppressed[2];       // identical repeats skipped, per stream

static void step(const char *name, Result rc) {
    if (g_nsteps < MAX_STEPS) {
        snprintf(g_steps[g_nsteps].name, sizeof(g_steps[0].name), "%s", name);
        g_steps[g_nsteps].rc = rc;
        g_nsteps++;
    }
    if (R_SUCCEEDED(rc))
        say("[ OK ] %s\n", name);
    else
        say("[FAIL] %s  rc=0x%X (module %u, desc %u)\n", name, rc, R_MODULE(rc), R_DESCRIPTION(rc));
}

static BtdrvGattAttributeUuid uuid16(u16 v) {
    BtdrvGattAttributeUuid u;
    memset(&u, 0, sizeof(u));
    u.size = 2;
    u.uuid[0] = v & 0xFF;   // little-endian, as on the wire
    u.uuid[1] = v >> 8;
    return u;
}

static void hexdump(const u8 *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        say("%02X ", p[i]);
    say("\n");
}

static void print_event(BtdrvBleEventType type, const BtdrvBleEventInfo *e, const char *stream) {
    switch (type) {
        case BtdrvBleEventType_ServerRegistration:
            say("  <%s> ServerRegistration result=0x%X server_if=%u status=%u\n", stream,
                e->server_registration.result, e->server_registration.server_if, e->server_registration.status);
            break;
        case BtdrvBleEventType_ServerAddAttribute:
            say("  <%s> ServerAddAttribute result=0x%X server_if=%u type=%u svc=%u attr=%u\n", stream,
                e->server_add_attribute.result, e->server_add_attribute.server_if,
                e->server_add_attribute.attr_type, e->server_add_attribute.service_id,
                e->server_add_attribute.attr_id);
            if (e->server_add_attribute.result == 0)
                g_add_ok = true;
            break;
        case BtdrvBleEventType_ServerConnection:
            say("  <%s> ServerConnection conn_id=%u addr=%02X:%02X:%02X:%02X:%02X:%02X reason=%u\n", stream,
                e->server_connection.conn_id,
                e->server_connection.address.address[0], e->server_connection.address.address[1],
                e->server_connection.address.address[2], e->server_connection.address.address[3],
                e->server_connection.address.address[4], e->server_connection.address.address[5],
                e->server_connection.reason);
            g_connected = true;
            break;
        case BtdrvBleEventType_ServerAttributeOperation:
            say("  <%s> ServerAttributeOperation %s svc=%u attr=%u size=%u\n", stream,
                e->server_attribute_operation.operation ? "WRITE" : "READ",
                e->server_attribute_operation.service_id, e->server_attribute_operation.attr_id,
                e->server_attribute_operation.size);
            break;
        case BtdrvBleEventType_ConnectionUpdate:
            say("  <%s> ConnectionUpdate result=0x%X conn_id=%u\n", stream,
                e->connection_update.result, e->connection_update.conn_id);
            break;
        default:
            say("  <%s> event type %d, first bytes: ", stream, (int)type);
            hexdump(e->data, 16);
            break;
    }
}

// Pull every pending event from both BLE event streams and print it.
static void drain(Event *ev, u64 wait_ms) {
    if (wait_ms)
        eventWait(ev, wait_ms * 1000000ULL);
    static int last_type[2] = {-1, -1};
    static u8 last_data[2][sizeof(BtdrvBleEventInfo)];
    for (int i = 0; i < 16; i++) {
        BtdrvBleEventInfo info;
        BtdrvBleEventType type;
        bool got = false;

        for (int stream = 0; stream < 2; stream++) {
            memset(&info, 0, sizeof(info));
            Result r = stream == 0 ? btdrvGetBleManagedEventInfo(&info, sizeof(info), &type)
                                   : btdrvGetLeHidEventInfo(&info, sizeof(info), &type);
            if (R_FAILED(r))
                continue;
            if ((int)type == last_type[stream] && memcmp(&info, last_data[stream], sizeof(info)) == 0) {
                g_suppressed[stream]++;       // same stale event again: ignore it
                continue;
            }
            last_type[stream] = (int)type;
            memcpy(last_data[stream], &info, sizeof(info));
            if ((int)type >= 0 && (int)type < 16)
                g_evcount[(int)type]++;
            print_event(type, &info, stream == 0 ? "managed" : "lehid");
            got = true;
        }
        if (!got)
            break;
    }
}

// Add one characteristic and print the outcome.
static void add_char(const BtdrvGattAttributeUuid *svc, u16 char_id, u16 perms, u8 props, const char *name, Event *ev) {
    BtdrvGattAttributeUuid c = uuid16(char_id);
    Result rc = btdrvAddGattCharacteristic(g_server_if, svc, &c, perms, props);
    step(name, rc);
    drain(ev, 150);
}

static void add_desc(const BtdrvGattAttributeUuid *svc, u16 desc_id, u16 perms, const char *name, Event *ev) {
    BtdrvGattAttributeUuid d = uuid16(desc_id);
    Result rc = btdrvAddGattDescriptor(g_server_if, svc, &d, perms);
    step(name, rc);
    drain(ev, 150);
}

// One short screen with every step's result, so nothing has to be scrolled.
static void print_summary(void) {
    consoleClear();
    say("=== SUMMARY (all results also in %s) ===\n", LOG_PATH);
    for (int i = 0; i < g_nsteps; i++) {
        if (R_SUCCEEDED(g_steps[i].rc))
            say("OK   %s\n", g_steps[i].name);
        else
            say("FAIL %s rc=0x%X\n", g_steps[i].name, g_steps[i].rc);
    }
    say("\nDistinct events seen (type:count):");
    for (int t = 0; t < 16; t++)
        if (g_evcount[t])
            say(" %d:%d", t, g_evcount[t]);
    say("\n  type 1=ServerRegistration 12=ServerAddAttribute 5=ServerConnection\n");
    say("Stale repeats skipped: managed=%d lehid=%d\n", g_suppressed[0], g_suppressed[1]);
    say("ServerAddAttribute success seen: %s\n", g_add_ok ? "YES" : "NO");
}

int main(int argc, char **argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen(LOG_PATH, "w");
    say("BLE gamepad spike  (log: %s)\n", g_log ? LOG_PATH : "NOT WRITABLE");
    say("HOS version: %u.%u.%u\n\n", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()),
        HOSVER_MICRO(hosversionGet()));

    bool btdrv_up = false, ble_up = false, server_up = false;
    Event ble_event;
    memset(&ble_event, 0, sizeof(ble_event));

    Result rc = btdrvInitialize();
    step("1. btdrvInitialize (can homebrew reach btdrv?)", rc);
    if (R_FAILED(rc)) {
        say("\nVERDICT: homebrew cannot open the btdrv service. Stop here.\n");
        goto wait_exit;
    }
    btdrv_up = true;

    rc = btdrvInitializeBle(&ble_event);
    step("2. btdrvInitializeBle", rc);
    if (R_FAILED(rc)) {
        say("\nVERDICT: BLE is not available to this process (btm owns it?).\n");
        goto wait_exit;
    }
    ble_up = true;

    step("3. btdrvEnableBle", btdrvEnableBle());
    drain(&ble_event, 200);

    BtdrvGattAttributeUuid app = uuid16(0xBEEF);   // arbitrary app id for the server registration
    rc = btdrvRegisterGattServer(&app);
    step("4. btdrvRegisterGattServer", rc);
    drain(&ble_event, 500);
    if (R_FAILED(rc))
        goto wait_exit;
    server_up = true;

    // The registration event's server_if is documented as junk, so find the real one by
    // trying small values until the stack reports a successful ServerAddAttribute.
    BtdrvGattAttributeUuid hid = uuid16(0x1812);   // HID service
    for (u8 sif = 0; sif <= 5 && !g_add_ok; sif++) {
        char nm[56];
        g_server_if = sif;
        rc = btdrvAddGattService(sif, &hid, 16, true);
        snprintf(nm, sizeof(nm), "5. AddGattService server_if=%u", sif);
        step(nm, rc);
        drain(&ble_event, 400);
    }
    if (!g_add_ok) {
        say("\nVERDICT: no server_if produced a successful AddAttribute event.\n"
            "GATT server is probably unusable from homebrew ('broken behaviour' per libnx docs).\n");
        goto wait_exit;
    }
    say("   -> using server_if=%u\n", g_server_if);

    const u16 R = BtdrvGattAttributePermission_Read;
    const u16 W = BtdrvGattAttributePermission_Write;
    const u8 PR = BtdrvGattCharacteristicProperty_Read;
    const u8 PN = BtdrvGattCharacteristicProperty_Notify;
    const u8 PW = BtdrvGattCharacteristicProperty_WriteNoResponse;
    add_char(&hid, 0x2A4A, R, PR, "6a. HID Information (2A4A)", &ble_event);
    add_char(&hid, 0x2A4B, R, PR, "6b. Report Map (2A4B)", &ble_event);
    add_char(&hid, 0x2A4C, W, PW, "6c. HID Control Point (2A4C)", &ble_event);
    add_char(&hid, 0x2A4D, R, PR | PN, "6d. Report (2A4D)", &ble_event);
    add_desc(&hid, 0x2908, R, "6e. Report Reference (2908)", &ble_event);
    add_desc(&hid, 0x2902, R | W, "6f. Client Config (2902)", &ble_event);

    step("7. btdrvEnableGattService", btdrvEnableGattService(g_server_if, &hid));
    drain(&ble_event, 300);

    BtdrvBleAdvertisePacketData adv;
    memset(&adv, 0, sizeof(adv));
    adv.adv_data_mask = BtdrvBleAdBit_Flags | BtdrvBleAdBit_Service | BtdrvBleAdBit_Appearance | BtdrvBleAdBit_DeviceName;
    adv.flag = BtdrvBleAdFlag_GeneralDiscovery | BtdrvBleAdFlag_BrEdrNotSupported;
    adv.appearance_data = 0x03C4;   // "Gamepad" appearance
    adv.num_service = 1;
    adv.uuid_val[0] = hid;
    step("8. btdrvSetBleAdvertiseData", btdrvSetBleAdvertiseData(&adv));
    step("9. btdrvSetBleVisibility(discoverable, connectable)", btdrvSetBleVisibility(true, true));
    drain(&ble_event, 300);

    say("\nAdvertising. On the Mac open a BLE scanner (e.g. the free LightBlue app)\n"
        "or System Settings > Bluetooth and look for a new device. Try to connect.\n"
        "Events will print below. Press + to quit.\n\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        drain(&ble_event, 250);
    }
    goto cleanup;

wait_exit:
    print_summary();
    say("\nPress + to quit.\n");
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

cleanup:
    say("\nCleaning up...\n");
    if (server_up)
        btdrvUnregisterGattServer(g_server_if);
    if (ble_up) {
        btdrvDisableBle();
        btdrvFinalizeBle();
        eventClose(&ble_event);
    }
    if (btdrv_up)
        btdrvExit();
    if (g_log)
        fclose(g_log);
    consoleExit(NULL);
    return 0;
}
