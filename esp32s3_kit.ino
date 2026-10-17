// language: C++, file: main.ino, target: ESP32-S3, Arduino core 3.3.12
// *spine — 34 slots, dashboard AP ownership, TFT redraw on change*
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertising.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include "slots.h"

#define BTN_PIN        0
#define LED_PIN        48
#define TFT_CS         10
#define TFT_DC         9
#define TFT_RST        8
#define TFT_BL         7
#define IR_TX_PIN      4
#define IR_RX_PIN      5
#define CC1101_CS      21
#define NRF24_CS       14
#define GPS_RX         16
#define GPS_TX         17

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
USBHIDKeyboard Keyboard;
USBHIDMouse    Mouse;
IRsend         irsend(IR_TX_PIN);
IRrecv         irrecv(IR_RX_PIN);

DNSServer dns;
WebServer http(80);

String        hid_payload = "http://192.168.4.1/omg.apk";
String        ducky_buf   = "";
String        creds_log   = "";
volatile int  activeSlot  = SLOT_IDLE;
volatile bool slotChanged = false;

// AP ownership — dashboard owns the AP by default; slots that hijack it flip this
bool dashApOwned = true;
static const char* DASH_AP_SSID = "LILBIG4DEV-S3";
static const char* DASH_AP_PASS = "letmecook123";
static const uint8_t DASH_AP_CH = 6;

static int scroll_top = 0;
#define ROW_H 10
#define VIEW_ROWS 12

void menu_scroll_to(int active) {
  if (active < scroll_top)                 scroll_top = active;
  if (active >= scroll_top + VIEW_ROWS)    scroll_top = active - VIEW_ROWS + 1;
  if (scroll_top < 0)                      scroll_top = 0;
  if (scroll_top > SLOT_COUNT - VIEW_ROWS) scroll_top = SLOT_COUNT - VIEW_ROWS;
}

void drawMenu() {
  menu_scroll_to(activeSlot);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.println("LILBIG4DEV ESP32-S3");
  for (int r = 0; r < VIEW_ROWS; r++) {
    int i = scroll_top + r;
    if (i >= SLOT_COUNT) break;
    if (i == activeSlot) { tft.setTextColor(ST77XX_BLACK, ST77XX_GREEN);
                           tft.printf("> %-12s\n", SLOTS[i].name); }
    else                 { tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
                           tft.printf("  %-12s\n", SLOTS[i].name); }
  }
  tft.setTextColor(ST77XX_GREEN, ST77XX_BLACK);
  tft.printf("slot %d/%d\n", activeSlot, SLOT_COUNT-1);
}

void switchSlot(int newSlot) {
  if (newSlot < 0 || newSlot >= SLOT_COUNT || newSlot == activeSlot) return;
  if (SLOTS[activeSlot].end) SLOTS[activeSlot].end();
  activeSlot = newSlot;
  slotChanged = true;
  LOGF("[*] slot -> %s\n", SLOTS[activeSlot].name);     // log line FIRST
  if (SLOTS[activeSlot].begin) SLOTS[activeSlot].begin();
}
void nextSlot() { switchSlot((activeSlot + 1) % SLOT_COUNT); }
void prevSlot() { switchSlot((activeSlot - 1 + SLOT_COUNT) % SLOT_COUNT); }

// ---- dashboard AP lifecycle ----
void dashboardDropAp() {
  if (!dashApOwned) return;
  WiFi.softAPdisconnect(true);
  dashApOwned = false;
}

void dashboardRestoreAp() {
  if (dashApOwned) return;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(DASH_AP_SSID, DASH_AP_PASS, DASH_AP_CH, 0, 4);
  dashApOwned = true;
  LOGF("[*] dashboard AP restored — %s\n", DASH_AP_SSID);
}

void handleLine(const String& line) {
  if (!line.length()) return;
  if (line == "next") { nextSlot(); return; }
  if (line == "prev") { prevSlot(); return; }
  if (line == "menu") { drawMenu(); return; }
  if (line.startsWith("slot ")) { switchSlot(line.substring(5).toInt()); return; }
  if (line == "help") {
    LOG("cmds: next prev slot N menu help\nverbs:");
    for (int i = 0; i < SLOT_COUNT; i++) { LOG(" "); LOG(SLOTS[i].verb); }
    LOG("\nsub-cmds: '<verb> ?' for options\n");
    return;
  }
  if (line == "url" || line.startsWith("url ")) {
  url_cmd(line.length() > 4 ? line.substring(4) : "");
  return;
}
  for (int i = 0; i < SLOT_COUNT; i++) {
    String v = SLOTS[i].verb;
    if (line == v) { switchSlot(i); return; }
    if (line.startsWith(v + " ")) {
      if (activeSlot != i) switchSlot(i);
      if (SLOTS[i].cmd) SLOTS[i].cmd(line.substring(v.length() + 1));
      return;
    }
  }
  if (SLOTS[activeSlot].cmd) { SLOTS[activeSlot].cmd(line); return; }
  LOGF("[!] unknown: %s\n", line.c_str());
}

void serialLoop() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length()) handleLine(line);
}

void buttonTask(void* pv) {
  unsigned long downAt = 0; bool wasDown = false;
  for (;;) {
    bool down = (digitalRead(BTN_PIN) == LOW);
    if (down && !wasDown) downAt = millis();
    if (!down && wasDown) {
      unsigned long held = millis() - downAt;
      if (held >= 800) prevSlot(); else nextSlot();
    }
    wasDown = down;
    vTaskDelay(30 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);   // USB-CDC → COM10  (ARDUINO_USB_CDC_ON_BOOT=1)
  Serial0.begin(115200);  // UART0  → COM9
  delay(1200);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  USB.begin();
  delay(300);
  Serial0.println("[*] boot: uart0 alive");
  Serial.println("[*] boot: usbserial alive");

  LittleFS.begin(true);
  if (LittleFS.exists("/url.txt")) {
  File f = LittleFS.open("/url.txt", "r");
  String u = f.readStringUntil('\n'); u.trim(); f.close();
  if (u.length()) hid_payload = u;
  }
  LOG("[*] payload url: " + hid_payload + "\n");
  irsend.begin();
  irrecv.enableIRIn();

  Keyboard.begin();

  tft.init(135, 240);
  tft.setRotation(1);
  drawMenu();

  dashboardInit();       // AP + sync WebServer + slot grid
  xTaskCreatePinnedToCore(buttonTask, "btn", 4096, NULL, 2, NULL, 1);

  LOG("[*] LILBIG4DEV ESP32-S3 ready\n");
  LOG("[*] type 'help' for commands\n");
}

void loop() {
  if (slotChanged) { slotChanged = false; drawMenu(); }
  dashboardTick();
  if (SLOTS[activeSlot].tick) SLOTS[activeSlot].tick();
  serialLoop();
  vTaskDelay(50 / portTICK_PERIOD_MS);
}