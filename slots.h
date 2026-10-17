// language: C++, file: include/slots.h, target: ESP32-S3
// *shared declarations — 34 slots, dashboard helpers, APSTA constants*
#pragma once
#include <Arduino.h>

enum Slot {
  SLOT_IDLE=0, SLOT_WIFI_SCAN, SLOT_DEAUTH, SLOT_BEACON, SLOT_PROBE_SNIFF,
  SLOT_PROBE_FLOOD, SLOT_PMKID, SLOT_HANDSHAKE, SLOT_EVIL_TWIN, SLOT_DEAUTH_DETECT,
  SLOT_BLE_SPAM, SLOT_BLE_SCAN, SLOT_BLE_BEACON, SLOT_HID_TYPE, SLOT_HID_DUCKYS,
  SLOT_HID_MOUSE, SLOT_RF_REPLAY, SLOT_IR_BLAST, SLOT_BADUSB, SLOT_NRF,
  SLOT_PINEAPPLE, SLOT_PORTAL_CREDS, SLOT_GPS, SLOT_INFO, SLOT_PANIC,
  SLOT_IR_RECV, SLOT_RF_EDGE, SLOT_CREDS_PERSIST, SLOT_SUBGHZ_BRUTE,
  SLOT_HONEYPOT, SLOT_BT_PAIR, SLOT_OTA,
  SLOT_DROPSERV, SLOT_ANDROID, SLOT_APKUP,          // ← new
  SLOT_COUNT
};

struct SlotDef {
  const char* name;
  const char* verb;
  void (*begin)();
  void (*tick)();
  void (*end)();
  void (*cmd)(const String&);
};

extern const SlotDef SLOTS[SLOT_COUNT];
extern volatile int  activeSlot;
extern volatile bool slotChanged;
extern String        hid_payload;
extern String        ducky_buf;
extern String        creds_log;

// dashboard-owned globals (defined in main.ino, used by dropserv/apkup/pine)
extern bool  dashApOwned;              // true when dashboard's AP is the live one

void switchSlot(int);
void nextSlot();
void prevSlot();
void handleLine(const String&);
void ducky_run();
void LOG(const String&);
void LOGF(const char* fmt, ...);
void dashboardInit();
void dashboardTick();
void dashboardRestoreAp();             // re-bring-up LILBIG4DEV-S3 after a slot hijacked the AP
void dashboardDropAp();                // tear down the dashboard AP (a slot wants the radio)
void url_cmd(const String&);