// Xbox controller bridge for nice!nano (nRF52840), Adafruit nRF52 core.
//
// The board appears to the Mac as an Xbox Wireless Controller (Series X, model
// 1914) over Bluetooth LE. The controller state comes from the Mac over USB
// serial (see docs/BRIDGE.md for the frame format):
//
//   A5 5A  TYPE  LEN  PAYLOAD[LEN]  CHK      CHK = XOR of TYPE, LEN, PAYLOAD
//   TYPE 0x01  report      LEN 16, the Xbox input report below
//   TYPE 0x02  test        LEN 1, 1 = run the built-in test pattern, 0 = stop
//   TYPE 0x03  bootloader  LEN 0, reboot into the UF2 bootloader (NICENANO drive)
//   TYPE 0x04  ping        LEN 0, answers "#PONG connected=0|1"
//
// Status lines go back to the Mac as text starting with '#'.
// Failsafe: if no report arrives for 500 ms, all inputs return to neutral.
//
// Test pattern (4 s loop): A for 0.3 s, then left stick fully right for 1 s.
//
// HID report descriptor, report layout and IDs are ported from
// ESP32-BLE-CompositeHID (MIT License, Copyright (c) 2021 lemmingDev and
// contributors), https://github.com/Mystfit/ESP32-BLE-CompositeHID
//
// Build: board "Adafruit Feather nRF52840 Express" (same chip, bootloader and
// SoftDevice S140 6.1.1 as the nice!nano). No GPIOs are used on purpose.

#include <bluefruit.h>

// ---------------------------------------------------------------------------
// Identity of a real Xbox Series X|S controller (model 1914)
// ---------------------------------------------------------------------------
static const char     DEVICE_NAME[] = "Xbox Wireless Controller";
static const uint16_t XBOX_VID      = 0x045E;   // Microsoft
static const uint16_t XBOX_1914_PID = 0x0B13;
static const uint16_t XBOX_1914_VER = 0x0509;

static const uint8_t XBOX_INPUT_REPORT_ID  = 0x01;
static const uint8_t XBOX_OUTPUT_REPORT_ID = 0x03;

// Report Reference descriptor types (Bluetooth HID over GATT spec)
static const uint8_t RPT_TYPE_INPUT  = 1;
static const uint8_t RPT_TYPE_OUTPUT = 2;

// Button bits in the 16-bit button field
static const uint16_t XBOX_BUTTON_A = 0x01;

// Input report (report ID 1), 16 bytes, little-endian
#pragma pack(push, 1)
struct XboxInputReport {
  uint16_t x, y;        // left stick,  0..65535, centre 0x8000
  uint16_t z, rz;       // right stick, 0..65535, centre 0x8000
  uint16_t brake;       // left trigger,  10 bits
  uint16_t accelerator; // right trigger, 10 bits
  uint8_t  hat;         // d-pad: 0 = none, 1..8 = N, NE, E, SE, S, SW, W, NW
  uint16_t buttons;     // 15 buttons
  uint8_t  share;       // share button in bit 0
};
#pragma pack(pop)
static_assert(sizeof(XboxInputReport) == 16, "Xbox input report must be 16 bytes");

static const uint8_t XBOX_1914_REPORT_MAP[] = {
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,                 // Gamepad, application collection
  0x85, XBOX_INPUT_REPORT_ID,
  0x09, 0x01, 0xA1, 0x00,                             //   pointer: X, Y
  0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
  0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
  0x09, 0x01, 0xA1, 0x00,                             //   pointer: Z, Rz
  0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
  0x95, 0x02, 0x75, 0x10, 0x81, 0x02, 0xC0,
  0x05, 0x02, 0x09, 0xC5, 0x15, 0x00, 0x26, 0xFF, 0x03, // brake, 10 bits
  0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
  0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
  0x05, 0x02, 0x09, 0xC4, 0x15, 0x00, 0x26, 0xFF, 0x03, // accelerator, 10 bits
  0x95, 0x01, 0x75, 0x0A, 0x81, 0x02,
  0x15, 0x00, 0x25, 0x00, 0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
  0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08,     // hat switch
  0x35, 0x00, 0x46, 0x3B, 0x01, 0x66, 0x14, 0x00,
  0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
  0x75, 0x04, 0x95, 0x01, 0x15, 0x00, 0x25, 0x00,
  0x35, 0x00, 0x45, 0x00, 0x65, 0x00, 0x81, 0x03,
  0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x15, 0x00,     // 15 buttons
  0x25, 0x01, 0x75, 0x01, 0x95, 0x0F, 0x81, 0x02,
  0x15, 0x00, 0x25, 0x00, 0x75, 0x01, 0x95, 0x01, 0x81, 0x03,
  0x05, 0x0C, 0x0A, 0xB2, 0x00, 0x15, 0x00, 0x25, 0x01, // share (Record)
  0x95, 0x01, 0x75, 0x01, 0x81, 0x02,
  0x15, 0x00, 0x25, 0x00, 0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
  0x05, 0x0F, 0x09, 0x21,                             // rumble output report
  0x85, XBOX_OUTPUT_REPORT_ID,
  0xA1, 0x02,
  0x09, 0x97, 0x15, 0x00, 0x25, 0x01, 0x75, 0x04, 0x95, 0x01, 0x91, 0x02,
  0x15, 0x00, 0x25, 0x00, 0x75, 0x04, 0x95, 0x01, 0x91, 0x03,
  0x09, 0x70, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x04, 0x91, 0x02,
  0x09, 0x50, 0x66, 0x01, 0x10, 0x55, 0x0E, 0x15, 0x00, 0x26, 0xFF, 0x00,
  0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
  0x09, 0xA7, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
  0x65, 0x00, 0x55, 0x00,
  0x09, 0x7C, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x91, 0x02,
  0xC0,
  0xC0
};

// ---------------------------------------------------------------------------
// HID service with the report IDs the real controller uses.
// BLEHidGeneric numbers reports 1, 2, 3... by position; the Xbox controller
// has input report 1 and output report 3, so begin() is replaced here.
// ---------------------------------------------------------------------------
class XboxHid : public BLEHidGeneric {
public:
  XboxHid() : BLEHidGeneric(1, 1, 0) {}

  err_t begin(void) override {
    VERIFY((_report_map != NULL) && _report_map_len, NRF_ERROR_INVALID_PARAM);
    VERIFY_STATUS(BLEService::begin());

    _chr_inputs[0].setUuid(UUID16_CHR_REPORT);
    _chr_inputs[0].setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    _chr_inputs[0].setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    _chr_inputs[0].setReportRefDescriptor(XBOX_INPUT_REPORT_ID, RPT_TYPE_INPUT);
    _chr_inputs[0].setFixedLen(sizeof(XboxInputReport));
    VERIFY_STATUS(_chr_inputs[0].begin());

    _chr_outputs[0].setUuid(UUID16_CHR_REPORT);
    _chr_outputs[0].setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    _chr_outputs[0].setPermission(SECMODE_ENC_NO_MITM, SECMODE_ENC_NO_MITM);
    _chr_outputs[0].setReportRefDescriptor(XBOX_OUTPUT_REPORT_ID, RPT_TYPE_OUTPUT);
    _chr_outputs[0].setFixedLen(8);
    VERIFY_STATUS(_chr_outputs[0].begin());
    uint8_t zero[8] = {0};
    _chr_outputs[0].write(zero, sizeof(zero));

    BLECharacteristic report_map(UUID16_CHR_REPORT_MAP);
    report_map.setTempMemory();
    report_map.setProperties(CHR_PROPS_READ);
    report_map.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    report_map.setFixedLen(_report_map_len);
    VERIFY_STATUS(report_map.begin());
    report_map.write(_report_map, _report_map_len);

    BLECharacteristic hid_info(UUID16_CHR_HID_INFORMATION);
    hid_info.setTempMemory();
    hid_info.setProperties(CHR_PROPS_READ);
    hid_info.setPermission(SECMODE_ENC_NO_MITM, SECMODE_NO_ACCESS);
    hid_info.setFixedLen(sizeof(_hid_info));
    VERIFY_STATUS(hid_info.begin());
    hid_info.write(_hid_info, sizeof(_hid_info));

    _chr_control.setProperties(CHR_PROPS_WRITE_WO_RESP);
    _chr_control.setPermission(SECMODE_NO_ACCESS, SECMODE_ENC_NO_MITM);
    _chr_control.setFixedLen(1);
    VERIFY_STATUS(_chr_control.begin());
    _chr_control.write8(0);
    return ERROR_NONE;
  }

  bool send(const XboxInputReport &r) {
    return inputReport(XBOX_INPUT_REPORT_ID, &r, sizeof(r));
  }
};

BLEDis  bledis;
BLEBas  blebas;
XboxHid xbox;

// ---------------------------------------------------------------------------
// Bridge state
// ---------------------------------------------------------------------------
static const uint8_t  FRAME_SYNC1 = 0xA5, FRAME_SYNC2 = 0x5A;
static const uint8_t  T_REPORT = 0x01, T_TEST = 0x02, T_BOOTLOADER = 0x03, T_PING = 0x04;
static const uint32_t FAILSAFE_MS = 500;    // no report for this long -> neutral
static const uint32_t RESEND_MS   = 1000;   // resend unchanged state this often

static XboxInputReport g_state;             // what the controller should report now
static XboxInputReport g_sent;              // what was last sent over BLE
static uint32_t g_lastSendMs   = 0;
static uint32_t g_lastReportMs = 0;
static bool     g_failsafe     = true;      // start neutral until the Mac sends something
static bool     g_testPattern  = false;

static XboxInputReport neutral() {
  XboxInputReport r;
  memset(&r, 0, sizeof(r));
  r.x = r.y = r.z = r.rz = 0x8000;
  return r;
}

static void onConnect(uint16_t conn_handle) {
  char peer[32] = {0};
  Bluefruit.Connection(conn_handle)->getPeerName(peer, sizeof(peer));
  Serial.print("#CONNECTED ");
  Serial.println(peer);
  g_lastSendMs = 0;                         // push the current state right away
}

static void onDisconnect(uint16_t conn_handle, uint8_t reason) {
  (void)conn_handle;
  Serial.print("#DISCONNECTED reason=0x");
  Serial.println(reason, HEX);
}

// ---------------------------------------------------------------------------
// Serial frame parser (byte at a time, resyncs on any error)
// ---------------------------------------------------------------------------
static void handleFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  switch (type) {
    case T_REPORT:
      if (len != sizeof(XboxInputReport)) {
        Serial.println("#ERR report length");
        return;
      }
      memcpy(&g_state, payload, sizeof(g_state));
      g_lastReportMs = millis();
      if (g_failsafe) {
        g_failsafe = false;
        Serial.println("#ACTIVE");
      }
      break;
    case T_TEST:
      g_testPattern = (len == 1 && payload[0] != 0);
      Serial.println(g_testPattern ? "#TEST on" : "#TEST off");
      if (!g_testPattern)
        g_state = neutral();
      break;
    case T_BOOTLOADER:
      Serial.println("#BOOTLOADER");
      Serial.flush();
      delay(100);
      enterUf2Dfu();                        // does not return
      break;
    case T_PING:
      Serial.print("#PONG connected=");
      Serial.println(Bluefruit.connected() ? 1 : 0);
      break;
    default:
      Serial.println("#ERR unknown type");
      break;
  }
}

static void pollSerial() {
  static uint8_t state = 0, type = 0, len = 0, idx = 0, chk = 0;
  static uint8_t buf[32];
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    switch (state) {
      case 0: state = (b == FRAME_SYNC1) ? 1 : 0; break;
      case 1: state = (b == FRAME_SYNC2) ? 2 : (b == FRAME_SYNC1 ? 1 : 0); break;
      case 2: type = b; chk = b; state = 3; break;
      case 3:
        len = b; chk ^= b; idx = 0;
        if (len > sizeof(buf)) state = 0;
        else state = (len == 0) ? 5 : 4;
        break;
      case 4:
        buf[idx++] = b; chk ^= b;
        if (idx == len) state = 5;
        break;
      case 5:
        if (b == chk) handleFrame(type, buf, len);
        else Serial.println("#ERR checksum");
        state = 0;
        break;
    }
  }
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  g_state = g_sent = neutral();

  Bluefruit.autoConnLed(false);     // keep the Feather LED pins untouched on the nice!nano
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setAppearance(BLE_APPEARANCE_HID_GAMEPAD);
  Bluefruit.Periph.setConnInterval(6, 12);  // 7.5-15 ms
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  // PnP ID: vendor ID source 0x02 (USB-IF), VID, PID, version, little-endian
  static const uint8_t pnp[7] = {
    0x02,
    (uint8_t)(XBOX_VID & 0xFF),      (uint8_t)(XBOX_VID >> 8),
    (uint8_t)(XBOX_1914_PID & 0xFF), (uint8_t)(XBOX_1914_PID >> 8),
    (uint8_t)(XBOX_1914_VER & 0xFF), (uint8_t)(XBOX_1914_VER >> 8),
  };
  bledis.setManufacturer("Microsoft");
  bledis.setModel(DEVICE_NAME);
  bledis.setPNPID((const char *)pnp, sizeof(pnp));
  bledis.begin();

  blebas.begin();
  blebas.write(100);

  xbox.setReportMap(XBOX_1914_REPORT_MAP, sizeof(XBOX_1914_REPORT_MAP));
  xbox.setHidInfo(0x0111, 0x00, 0x01);
  xbox.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addAppearance(BLE_APPEARANCE_HID_GAMEPAD);
  Bluefruit.Advertising.addService(xbox);
  Bluefruit.ScanResponse.addName();   // name is too long for the main packet
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);

  Serial.println("#READY xbox_bridge");
}

void loop() {
  pollSerial();
  uint32_t now = millis();

  if (g_testPattern) {
    uint32_t phase = now % 4000;
    g_state = neutral();
    if (phase < 300)
      g_state.buttons |= XBOX_BUTTON_A;
    if (phase >= 1000 && phase < 2000)
      g_state.x = 0xFFFF;
  } else if (!g_failsafe && now - g_lastReportMs > FAILSAFE_MS) {
    g_state = neutral();
    g_failsafe = true;
    Serial.println("#FAILSAFE neutral");
  }

  if (Bluefruit.connected()) {
    bool changed = memcmp(&g_state, &g_sent, sizeof(g_state)) != 0;
    if (changed || now - g_lastSendMs > RESEND_MS) {
      if (xbox.send(g_state)) {
        g_sent = g_state;
        g_lastSendMs = now;
      }
    }
  }
  delay(1);
}
