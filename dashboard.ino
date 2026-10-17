// language: C++, file: dashboard.ino, target: ESP32-S3
// *sync WebServer + 1s polling. no ESPAsyncWebServer, no AsyncTCP*
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "slots.h"

extern WebServer http;
extern DNSServer dns;

const char* AP_SSID = "LILBIG4DEV-S3";
const char* AP_PASS = "letmecook123";
static const uint8_t AP_CH = 6;

static String s_logbuf;
void LOG(const String& s) {
  Serial.print(s);
  s_logbuf += s;
  if (s_logbuf.length() > 8192) s_logbuf.remove(0, s_logbuf.length() - 8192);
}
void LOGF(const char* fmt, ...) {
  char buf[256]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  Serial.print(buf);
  s_logbuf += buf;
  if (s_logbuf.length() > 8192) s_logbuf.remove(0, s_logbuf.length() - 8192);
}

static const char DASH_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LILBIG4DEV S3</title>
<style>
body{margin:0;background:#0b0b0f;color:#e6e6e6;padding:14px;font:13px ui-monospace,monospace}
h1{font-size:15px;margin:0 0 4px;color:#39ff14}.sub{color:#888;font-size:11px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:6px;margin-bottom:12px}
.slot{background:#1a1a22;border:1px solid #2a2a34;color:#e6e6e6;padding:8px 6px;font-size:11px;
      cursor:pointer;text-align:left;border-radius:4px;font-family:inherit}
.slot:hover{border-color:#39ff14}.slot.on{background:#39ff14;color:#000;font-weight:700}
.slot small{display:block;color:#888;font-size:9px;margin-top:2px}.slot.on small{color:#003}
#log{background:#050508;border:1px solid #2a2a34;border-radius:4px;padding:10px;
     height:38vh;overflow-y:auto;white-space:pre-wrap;font-size:11px;color:#39ff14}
.bar{display:flex;gap:6px;margin-top:8px}
input{flex:1;background:#050508;border:1px solid #2a2a34;color:#e6e6e6;padding:9px;
      border-radius:4px;font-family:inherit;font-size:12px;outline:0}
input:focus{border-color:#39ff14}
button{background:#39ff14;color:#000;border:0;padding:9px 14px;font-weight:700;
       border-radius:4px;cursor:pointer;font-family:inherit}
#stat{margin-top:8px;color:#666;font-size:10px}
</style>
<h1>LILBIG4DEV ESP32-S3</h1>
<div class=sub id=sub>linking…</div>
<div class=bar style="margin-bottom:8px">
  <input id=url placeholder="payload url (http://x/y.apk)" autocomplete=off>
  <button onclick=seturl()>set url</button>
</div>
<div class=grid id=grid></div>
<div id=log></div>
<div class=bar><input id=cmd placeholder="scan | info | help" autocomplete=off>
<button onclick=send()>run</button></div>
<div id=stat></div>
<script>
const log=document.getElementById('log'),grid=document.getElementById('grid');
let lastLen=0;
function add(t){log.textContent+=t;log.scrollTop=log.scrollHeight;}
async function poll(){
  try{
    const r=await fetch('/api/log');const j=await r.json();
    if(j.log.length>lastLen){add(j.log.slice(lastLen));lastLen=j.log.length;}
    else if(j.log.length<lastLen){log.textContent=j.log;lastLen=j.log.length;}
    document.getElementById('stat').textContent=j.stat;
  }catch(e){}
  setTimeout(poll,1000);
}
async function init(){
  const r=await fetch('/api/init');const j=await r.json();
  document.getElementById('sub').textContent='linked · '+location.host;
  document.getElementById('url').value = j.url || '';
  grid.innerHTML='';
  j.slots.forEach(s=>{const b=document.createElement('button');
    b.className='slot'+(s.id===j.active?' on':'');
    b.innerHTML=s.name+'<small>'+s.verb+'</small>';
    b.onclick=()=>fetch('/api/slot?id='+s.id).then(()=>setTimeout(init,200));
    grid.appendChild(b);});
}
async function seturl(){
  const u=document.getElementById('url').value.trim();
  if(!u)return;
  await fetch('/api/cmd?c='+encodeURIComponent('url set '+u));
  add('> url set '+u+'\n');
}
function send(){const i=document.getElementById('cmd');const v=i.value.trim();
  if(!v)return;add('> '+v+'\n');
  fetch('/api/cmd?c='+encodeURIComponent(v)).then(()=>{i.value='';});}
document.getElementById('cmd').addEventListener('keydown',e=>{if(e.key==='Enter')send()});
init();poll();
</script>)HTML";

void dashboardInit() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CH, 0, 4);
  dashApOwned = true;

  http.on("/", HTTP_GET, [](){
    http.send_P(200, "text/html", DASH_HTML);
  });
  http.on("/api/init", HTTP_GET, [](){
    String s = "{\"active\":" + String((int)activeSlot) +
               ",\"url\":\"" + hid_payload + "\",\"slots\":[";
    for (int i = 0; i < SLOT_COUNT; i++) {
      s += "{\"id\":" + String(i) + ",\"name\":\"" + String(SLOTS[i].name) +
           "\",\"verb\":\"" + String(SLOTS[i].verb) + "\"}";
      if (i < SLOT_COUNT-1) s += ",";
    }
    s += "]}";
    http.send(200, "application/json", s);
  });
  http.on("/api/log", HTTP_GET, [](){
    char stat[160];
    snprintf(stat, sizeof(stat),
      "heap %dKB · psram %dKB · slot %d/%d · clients %d · ap %s",
      ESP.getFreeHeap()/1024, ESP.getPsramSize()/1024,
      (int)activeSlot, SLOT_COUNT-1, WiFi.softAPgetStationNum(),
      dashApOwned ? AP_SSID : "(hijacked)");
    String o = "{\"log\":\"";
    for (size_t i = 0; i < s_logbuf.length(); i++) {
      char c = s_logbuf[i];
      if      (c == '"')  o += "\\\"";
      else if (c == '\\') o += "\\\\";
      else if (c == '\n') o += "\\n";
      else if (c == '\r') o += "\\r";
      else if (c == '\t') o += "\\t";
      else if ((uint8_t)c < 0x20) o += ' ';
      else o += c;
    }
    o += "\",\"stat\":\""; o += stat; o += "\"}";
    http.send(200, "application/json", o);
  });
  http.on("/api/slot", HTTP_GET, [](){
    if (http.hasArg("id")) switchSlot(http.arg("id").toInt());
    http.send(200, "text/plain", "ok");
  });
  http.on("/api/cmd", HTTP_GET, [](){
    if (http.hasArg("c")) handleLine(http.arg("c"));
    http.send(200, "text/plain", "ok");
  });
  http.on("/api/restore-ap", HTTP_GET, [](){
    dashboardRestoreAp();
    http.send(200, "text/plain", "restored");
  });
  http.begin();
  LOGF("\n[*] dashboard http://%s\n", WiFi.softAPIP().toString().c_str());
  LOGF("[*] AP: %s  pass: %s\n", AP_SSID, AP_PASS);
}

void dashboardTick() { http.handleClient(); }