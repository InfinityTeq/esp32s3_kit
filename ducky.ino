// language: C++, file: src/ducky.ino, target: ESP32-S3
// *DuckyScript parser — STRING / DELAY / ENTER / GUI / CTRL / ALT / SHIFT / TAB / ESC / REPEAT*
// *fed by slot 14 or serial; blank line fires the run*
#include <Arduino.h>
#include <USBHIDKeyboard.h>
#include "slots.h"

static int count_lines(const String& s);

extern USBHIDKeyboard Keyboard;

// ---- key token map: bare-word keys → HID codes ----
struct KeyMap { const char* name; uint8_t code; };
static const KeyMap KEY_TOKENS[] = {
  {"ENTER",     KEY_RETURN},
  {"RETURN",    KEY_RETURN},
  {"TAB",       KEY_TAB},
  {"ESC",       KEY_ESC},
  {"ESCAPE",    KEY_ESC},
  {"SPACE",     ' '},
  {"BACKSPACE", KEY_BACKSPACE},
  {"DELETE",    KEY_DELETE},
  {"UP",        KEY_UP_ARROW},
  {"DOWN",      KEY_DOWN_ARROW},
  {"LEFT",      KEY_LEFT_ARROW},
  {"RIGHT",     KEY_RIGHT_ARROW},
  {"HOME",      KEY_HOME},
  {"END",       KEY_END},
  {"PAGEUP",    KEY_PAGE_UP},
  {"PAGEDOWN",  KEY_PAGE_DOWN},
  {"F1",        KEY_F1},  {"F2", KEY_F2},  {"F3", KEY_F3},  {"F4", KEY_F4},
  {"F5",        KEY_F5},  {"F6", KEY_F6},  {"F7", KEY_F7},  {"F8", KEY_F8},
  {"F9",        KEY_F9},  {"F10",KEY_F10}, {"F11",KEY_F11}, {"F12",KEY_F12},
};
static const int KEY_TOKEN_COUNT = sizeof(KEY_TOKENS)/sizeof(KEY_TOKENS[0]);

static uint8_t key_lookup(const String& name) {
  for (int i=0;i<KEY_TOKEN_COUNT;i++)
    if (name.equalsIgnoreCase(KEY_TOKENS[i].name)) return KEY_TOKENS[i].code;
  return 0;
}

// ---- modifier combo: CTRL/ALT/SHIFT/GUI + single key ----
static void press_combo(uint8_t mod, char k) {
  Keyboard.press(mod);
  Keyboard.press(k);
  delay(30);
  Keyboard.releaseAll();
}

static void press_combo_named(uint8_t mod, const String& name) {
  uint8_t k = key_lookup(name);
  if (k == 0 && name.length() == 1) k = name[0];
  if (k == 0) return;
  Keyboard.press(mod);
  Keyboard.press(k);
  delay(30);
  Keyboard.releaseAll();
}

// ============================================================
// main runner — call after buffer is filled + blank line hit
// ============================================================
void ducky_run() {
  if (!ducky_buf.length()) {
    LOG("[!] ducky buffer empty\n");
    return;
  }

  LOGF("[*] ducky: %d lines queued\n", 1 + count_lines(ducky_buf));
  Keyboard.begin();
  delay(1200);

  int i = 0, n = ducky_buf.length();
  while (i < n) {
    int e = ducky_buf.indexOf('\n', i);
    if (e < 0) e = n;
    String L = ducky_buf.substring(i, e);
    i = e + 1;
    L.trim();
    if (!L.length()) continue;
    if (L.startsWith("//")) continue;      // comment line

    String u = L;
    u.toUpperCase();

    // ---- DELAY ms ----
    if (u.startsWith("DELAY ")) {
      int ms = L.substring(6).toInt();
      if (ms > 0) delay(ms);
    }
    // ---- STRING text (raw, preserves case) ----
    else if (u.startsWith("STRING ")) {
      Keyboard.print(L.substring(7));
    }
    // ---- STRINGLN text (type + enter) ----
    else if (u.startsWith("STRINGLN ")) {
      Keyboard.print(L.substring(9));
      Keyboard.write(KEY_RETURN);
    }
    // ---- bare key: ENTER / TAB / ESC / UP / F5 / ... ----
    else if (key_lookup(L)) {
      Keyboard.write(key_lookup(L));
    }
    // ---- modifier + key: GUI r / CTRL c / ALT F4 / SHIFT TAB ----
    else if (u.startsWith("GUI ") || u.startsWith("WINDOWS ") || u.startsWith("WIN ")) {
      int off = u.startsWith("GUI ") ? 4 : (u.startsWith("WINDOWS ") ? 8 : 4);
      press_combo_named(KEY_LEFT_GUI, L.substring(off));
    }
    else if (u.startsWith("CTRL ") || u.startsWith("CONTROL ")) {
      int off = u.startsWith("CTRL ") ? 5 : 8;
      press_combo_named(KEY_LEFT_CTRL, L.substring(off));
    }
    else if (u.startsWith("ALT ")) {
      press_combo_named(KEY_LEFT_ALT, L.substring(4));
    }
    else if (u.startsWith("SHIFT ")) {
      press_combo_named(KEY_LEFT_SHIFT, L.substring(6));
    }
    // ---- REPEAT N <key> — fires the named key N times ----
    else if (u.startsWith("REPEAT ")) {
      String rest = L.substring(7);
      int sp = rest.indexOf(' ');
      int count = (sp > 0 ? rest.substring(0, sp) : rest).toInt();
      uint8_t key = KEY_RETURN;
      if (sp > 0) {
        String keyName = rest.substring(sp + 1);
        uint8_t looked = key_lookup(keyName);
        key = looked ? looked : (keyName.length() ? keyName[0] : KEY_RETURN);
      }
      for (int j = 0; j < count; j++) {
        Keyboard.write(key);
        delay(50);
      }
    }
    // ---- STRING_DELAY ms — sets inter-char delay for subsequent STRING ----
    else if (u.startsWith("STRING_DELAY ")) {
      int d = L.substring(13).toInt();
      if (d > 0 && d < 2000) {
        // not persisted across lines in this simple parser
        delay(d);
      }
    }
    // ---- REM comment ----
    else if (u.startsWith("REM")) {
      // ignore
    }
    // ---- unknown verb — log + skip ----
    else {
      LOGF("[ducky] ? %s\n", L.c_str());
    }
  }

  Keyboard.end();
  LOG("[*] ducky done\n");
  ducky_buf = "";
}

// helper — used only for the LOGF count line
static int count_lines(const String& s) {
  int c = 0;
  for (size_t i = 0; i < s.length(); i++) if (s[i] == '\n') c++;
  return c;
}