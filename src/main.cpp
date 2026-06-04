#include <Arduino.h>
#include <IRac.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef IRSTATION_AP_SSID
#define IRSTATION_AP_SSID "IRStation"
#endif

#ifndef IRSTATION_AP_PASSWORD
#define IRSTATION_AP_PASSWORD "12345678"
#endif

#ifndef DEFAULT_AC_PROTOCOL
#define DEFAULT_AC_PROTOCOL "GREE"
#endif

#ifndef DEFAULT_AC_MODEL
#define DEFAULT_AC_MODEL 1
#endif

constexpr uint8_t PIN_IR_OUT = 1;
constexpr uint8_t PIN_IR_IN = 3;
constexpr uint8_t PIN_LCD_RST = 4;
constexpr uint8_t PIN_LCD_DC = 5;
constexpr uint8_t PIN_LCD_SCK = 6;
constexpr uint8_t PIN_LCD_SDA = 7;
constexpr uint8_t PIN_LCD_BL = 8;
constexpr uint8_t PIN_BTN1 = 9;
constexpr uint8_t PIN_BTN2 = 0;
constexpr uint8_t PIN_LED = 10;

constexpr bool LCD_BACKLIGHT_ACTIVE_LOW = true;
constexpr uint32_t BACKLIGHT_IDLE_MS = 60UL * 1000UL;
constexpr uint32_t DISPLAY_REFRESH_MS = 1000UL;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40UL;
constexpr uint8_t TEMP_MIN_C = 16;
constexpr uint8_t TEMP_MAX_C = 30;

U8G2_ST7567_ERC12864_F_4W_SW_SPI u8g2(
    U8G2_R0,
    PIN_LCD_SCK,
    PIN_LCD_SDA,
    U8X8_PIN_NONE,
    PIN_LCD_DC,
    PIN_LCD_RST);

WebServer server(80);
Preferences prefs;
IRac ac(PIN_IR_OUT);

struct AirState {
  bool power = false;
  uint8_t temp = 26;
  String mode = "cool";
  String fan = "auto";
  bool swing = false;
};

struct ButtonState {
  uint8_t pin = 0;
  bool stable = true;
  bool lastRaw = true;
  uint32_t lastChangeMs = 0;
};

AirState air;
ButtonState button1{PIN_BTN1};
ButtonState button2{PIN_BTN2};

String acProtocol = DEFAULT_AC_PROTOCOL;
int16_t acModel = DEFAULT_AC_MODEL;
String lastIrError;
uint32_t lastIrSendMs = 0;
uint32_t lastWebActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
bool backlightOn = false;
bool apMode = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IRStation</title>
<style>
:root{color-scheme:light dark;--bg:#f4f7f5;--panel:#ffffff;--ink:#17201d;--muted:#64706b;--line:#d8e0dc;--accent:#0b8f6a;--warn:#d97706;--heat:#c2410c;--cool:#1371b8}
@media(prefers-color-scheme:dark){:root{--bg:#121716;--panel:#1c2421;--ink:#eef5f1;--muted:#a5b2ac;--line:#34423d;--accent:#36c494;--warn:#fbbf24;--heat:#fb7a42;--cool:#60a5fa}}
*{box-sizing:border-box}body{margin:0;font:16px/1.4 system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--ink)}
main{width:min(720px,100%);margin:0 auto;padding:18px}
.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}.brand{font-weight:800;font-size:24px}.chip{border:1px solid var(--line);border-radius:999px;padding:6px 10px;color:var(--muted);font-size:13px}
.status{display:grid;grid-template-columns:1fr auto;gap:14px;align-items:center;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;margin-bottom:14px}
.temp{font-size:70px;font-weight:800;letter-spacing:0;line-height:.92}.temp small{font-size:28px}.meta{color:var(--muted);font-size:15px;margin-top:8px}
.power{width:92px;height:92px;border:0;border-radius:50%;background:var(--accent);color:white;font-size:38px;box-shadow:0 8px 24px #0002}.power.off{background:#6b7280}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}
.label{font-size:13px;color:var(--muted);margin-bottom:9px}.seg{display:grid;grid-template-columns:repeat(auto-fit,minmax(70px,1fr));gap:7px}
button{min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;font-weight:650}button.active{background:var(--accent);border-color:var(--accent);color:white}
.step{display:grid;grid-template-columns:54px 1fr 54px;gap:8px;align-items:center}.step button{font-size:26px}.value{text-align:center;font-size:32px;font-weight:800}
.foot{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;color:var(--muted);font-size:13px}.foot span{border:1px solid var(--line);border-radius:999px;padding:5px 9px}
@media(max-width:520px){main{padding:12px}.grid{grid-template-columns:1fr}.temp{font-size:58px}.power{width:78px;height:78px}.status{padding:14px}}
</style>
</head>
<body>
<main>
  <div class="top"><div class="brand">IRStation</div><div class="chip" id="net">连接中</div></div>
  <section class="status">
    <div><div class="temp"><span id="temp">--</span><small>°C</small></div><div class="meta"><span id="mode">--</span> · 风速 <span id="fan">--</span> · 摆风 <span id="swing">--</span></div></div>
    <button class="power off" id="power" title="电源">⏻</button>
  </section>
  <section class="grid">
    <div class="panel"><div class="label">温度</div><div class="step"><button id="down">−</button><div class="value" id="temp2">--</div><button id="up">+</button></div></div>
    <div class="panel"><div class="label">模式</div><div class="seg" id="modes">
      <button data-v="auto">自动</button><button data-v="cool">制冷</button><button data-v="heat">制热</button><button data-v="dry">除湿</button><button data-v="fan">送风</button>
    </div></div>
    <div class="panel"><div class="label">风速</div><div class="seg" id="fans">
      <button data-v="auto">自动</button><button data-v="low">低</button><button data-v="medium">中</button><button data-v="high">高</button>
    </div></div>
    <div class="panel"><div class="label">摆风</div><div class="seg" id="swings"><button data-v="off">关闭</button><button data-v="on">开启</button></div></div>
  </section>
  <div class="foot"><span id="proto">协议 --</span><span id="ip">IP --</span><span id="rssi">RSSI --</span></div>
</main>
<script>
const labels={mode:{auto:'自动',cool:'制冷',heat:'制热',dry:'除湿',fan:'送风'},fan:{auto:'自动',low:'低',medium:'中',high:'高'},swing:{true:'开启',false:'关闭'}};
let state=null;
async function api(path){const r=await fetch(path,{cache:'no-store'}); if(!r.ok)throw new Error(await r.text()); return r.json();}
function mark(group,value){document.querySelectorAll(group+' button').forEach(b=>b.classList.toggle('active',b.dataset.v==value));}
function render(s){
 state=s.state; document.getElementById('temp').textContent=state.temp; document.getElementById('temp2').textContent=state.temp+'°';
 document.getElementById('mode').textContent=labels.mode[state.mode]||state.mode; document.getElementById('fan').textContent=labels.fan[state.fan]||state.fan;
 document.getElementById('swing').textContent=state.swing?'开启':'关闭'; document.getElementById('power').classList.toggle('off',!state.power);
 document.getElementById('net').textContent=s.device.wifi+' · '+s.device.ip; document.getElementById('ip').textContent='IP '+s.device.ip;
 document.getElementById('rssi').textContent='RSSI '+s.device.rssi; document.getElementById('proto').textContent='协议 '+s.ir.protocol+'/'+s.ir.model;
 mark('#modes',state.mode); mark('#fans',state.fan); mark('#swings',state.swing?'on':'off');
}
async function refresh(){try{render(await api('/api/state'))}catch(e){document.getElementById('net').textContent='离线'}}
async function control(q){render(await api('/api/control?'+q));}
document.getElementById('power').onclick=()=>api('/api/power?value=toggle').then(render);
document.getElementById('up').onclick=()=>api('/api/temp?delta=1').then(render);
document.getElementById('down').onclick=()=>api('/api/temp?delta=-1').then(render);
document.querySelectorAll('#modes button').forEach(b=>b.onclick=()=>control('mode='+b.dataset.v));
document.querySelectorAll('#fans button').forEach(b=>b.onclick=()=>control('fan='+b.dataset.v));
document.querySelectorAll('#swings button').forEach(b=>b.onclick=()=>control('swing='+b.dataset.v));
refresh(); setInterval(refresh,5000);
</script>
</body>
</html>
)HTML";

String onOff(bool value) {
  return value ? "on" : "off";
}

String jsonEscape(const String &text) {
  String out;
  out.reserve(text.length() + 8);
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out += c;
    }
  }
  return out;
}

bool isTruthy(const String &value) {
  return value == "1" || value == "on" || value == "true" || value == "yes";
}

bool isFalsy(const String &value) {
  return value == "0" || value == "off" || value == "false" || value == "no";
}

bool parseSwitchArg(const String &value, bool current, bool &result) {
  String v = value;
  v.toLowerCase();
  if (v == "toggle") {
    result = !current;
    return true;
  }
  if (isTruthy(v)) {
    result = true;
    return true;
  }
  if (isFalsy(v)) {
    result = false;
    return true;
  }
  return false;
}

bool validMode(const String &value) {
  return value == "auto" || value == "cool" || value == "heat" || value == "dry" || value == "fan";
}

bool validFan(const String &value) {
  return value == "auto" || value == "low" || value == "medium" || value == "high";
}

stdAc::opmode_t toStdMode(const String &mode) {
  if (mode == "auto") return stdAc::opmode_t::kAuto;
  if (mode == "heat") return stdAc::opmode_t::kHeat;
  if (mode == "dry") return stdAc::opmode_t::kDry;
  if (mode == "fan") return stdAc::opmode_t::kFan;
  return stdAc::opmode_t::kCool;
}

stdAc::fanspeed_t toStdFan(const String &fan) {
  if (fan == "low") return stdAc::fanspeed_t::kLow;
  if (fan == "medium") return stdAc::fanspeed_t::kMedium;
  if (fan == "high") return stdAc::fanspeed_t::kHigh;
  return stdAc::fanspeed_t::kAuto;
}

void setBacklight(bool on) {
  backlightOn = on;
  const uint8_t onLevel = LCD_BACKLIGHT_ACTIVE_LOW ? LOW : HIGH;
  digitalWrite(PIN_LCD_BL, on ? onLevel : !onLevel);
}

void noteActivity() {
  lastWebActivityMs = millis();
  setBacklight(true);
}

void saveState() {
  prefs.putBool("power", air.power);
  prefs.putUChar("temp", air.temp);
  prefs.putString("mode", air.mode);
  prefs.putString("fan", air.fan);
  prefs.putBool("swing", air.swing);
}

void saveConfig() {
  prefs.putString("proto", acProtocol);
  prefs.putShort("model", acModel);
}

void loadState() {
  air.power = prefs.getBool("power", air.power);
  air.temp = prefs.getUChar("temp", air.temp);
  air.mode = prefs.getString("mode", air.mode);
  air.fan = prefs.getString("fan", air.fan);
  air.swing = prefs.getBool("swing", air.swing);
  acProtocol = prefs.getString("proto", acProtocol);
  acModel = prefs.getShort("model", acModel);

  if (air.temp < TEMP_MIN_C || air.temp > TEMP_MAX_C) air.temp = 26;
  if (!validMode(air.mode)) air.mode = "cool";
  if (!validFan(air.fan)) air.fan = "auto";
}

bool sendCurrentAc() {
  String proto = acProtocol;
  proto.toUpperCase();
  const decode_type_t protocol = strToDecodeType(proto.c_str());
  if (protocol == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(protocol)) {
    lastIrError = "Unsupported protocol: " + acProtocol;
    return false;
  }

  ac.next.protocol = protocol;
  ac.next.model = acModel;
  ac.next.power = air.power;
  ac.next.mode = toStdMode(air.mode);
  ac.next.celsius = true;
  ac.next.degrees = air.temp;
  ac.next.fanspeed = toStdFan(air.fan);
  ac.next.swingv = air.swing ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
  ac.next.swingh = stdAc::swingh_t::kOff;
  ac.next.quiet = false;
  ac.next.turbo = false;
  ac.next.econo = false;
  ac.next.light = false;
  ac.next.filter = false;
  ac.next.clean = false;
  ac.next.beep = true;
  ac.next.sleep = -1;
  ac.next.clock = -1;

  if (!ac.sendAc()) {
    lastIrError = "IR send failed for protocol: " + acProtocol;
    return false;
  }

  lastIrError = "";
  lastIrSendMs = millis();
  digitalWrite(PIN_LED, HIGH);
  delay(35);
  digitalWrite(PIN_LED, LOW);
  return true;
}

String isoTime() {
  time_t now = time(nullptr);
  if (now < 100000) return "";
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmNow);
  return String(buf);
}

String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

String stateJson() {
  String json;
  json.reserve(520);
  json += "{\"ok\":true";
  json += ",\"state\":{\"power\":";
  json += air.power ? "true" : "false";
  json += ",\"temp\":";
  json += air.temp;
  json += ",\"mode\":\"";
  json += air.mode;
  json += "\",\"fan\":\"";
  json += air.fan;
  json += "\",\"swing\":";
  json += air.swing ? "true" : "false";
  json += "}";
  json += ",\"device\":{\"wifi\":\"";
  json += apMode ? "ap" : "sta";
  json += "\",\"ip\":\"";
  json += ipString();
  json += "\",\"rssi\":";
  json += apMode ? 0 : WiFi.RSSI();
  json += ",\"heap\":";
  json += ESP.getFreeHeap();
  json += ",\"uptime\":";
  json += millis() / 1000;
  json += ",\"backlight\":";
  json += backlightOn ? "true" : "false";
  json += "}";
  json += ",\"ir\":{\"protocol\":\"";
  json += jsonEscape(acProtocol);
  json += "\",\"model\":";
  json += acModel;
  json += ",\"lastSendMs\":";
  json += lastIrSendMs;
  json += ",\"lastError\":\"";
  json += jsonEscape(lastIrError);
  json += "\"}";
  json += ",\"time\":\"";
  json += isoTime();
  json += "\"}";
  return json;
}

void sendApi(const String &body, int code = 200) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

void sendError(int code, const String &message) {
  sendApi("{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}", code);
}

bool shouldSendIr() {
  if (!server.hasArg("send")) return true;
  String value = server.arg("send");
  value.toLowerCase();
  return !isFalsy(value);
}

bool applyControlArgs() {
  bool changed = false;

  if (server.hasArg("power")) {
    bool value;
    if (!parseSwitchArg(server.arg("power"), air.power, value)) return false;
    air.power = value;
    changed = true;
  }

  if (server.hasArg("temp")) {
    int value = server.arg("temp").toInt();
    if (value < TEMP_MIN_C || value > TEMP_MAX_C) return false;
    air.temp = value;
    changed = true;
  }

  if (server.hasArg("delta")) {
    int value = static_cast<int>(air.temp) + server.arg("delta").toInt();
    air.temp = constrain(value, TEMP_MIN_C, TEMP_MAX_C);
    changed = true;
  }

  if (server.hasArg("mode")) {
    String value = server.arg("mode");
    value.toLowerCase();
    if (!validMode(value)) return false;
    air.mode = value;
    changed = true;
  }

  if (server.hasArg("fan")) {
    String value = server.arg("fan");
    value.toLowerCase();
    if (!validFan(value)) return false;
    air.fan = value;
    changed = true;
  }

  if (server.hasArg("swing")) {
    bool value;
    if (!parseSwitchArg(server.arg("swing"), air.swing, value)) return false;
    air.swing = value;
    changed = true;
  }

  if (changed) saveState();
  return true;
}

void handleRoot() {
  noteActivity();
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleState() {
  noteActivity();
  sendApi(stateJson());
}

void handleControl() {
  noteActivity();
  if (!applyControlArgs()) {
    sendError(400, "Invalid control argument");
    return;
  }
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handlePower() {
  String value = server.hasArg("value") ? server.arg("value") : "toggle";
  noteActivity();
  bool parsed;
  if (!parseSwitchArg(value, air.power, parsed)) {
    sendError(400, "Invalid power value");
    return;
  }
  air.power = parsed;
  saveState();
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handleTemp() {
  noteActivity();
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    if (value < TEMP_MIN_C || value > TEMP_MAX_C) {
      sendError(400, "Temperature must be 16..30");
      return;
    }
    air.temp = value;
  } else if (server.hasArg("delta")) {
    int value = static_cast<int>(air.temp) + server.arg("delta").toInt();
    air.temp = constrain(value, TEMP_MIN_C, TEMP_MAX_C);
  } else {
    sendError(400, "Missing value or delta");
    return;
  }
  saveState();
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handleMode() {
  noteActivity();
  if (!server.hasArg("value")) {
    sendError(400, "Missing mode value");
    return;
  }
  String value = server.arg("value");
  value.toLowerCase();
  if (!validMode(value)) {
    sendError(400, "Invalid mode value");
    return;
  }
  air.mode = value;
  saveState();
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handleFan() {
  noteActivity();
  if (!server.hasArg("value")) {
    sendError(400, "Missing fan value");
    return;
  }
  String value = server.arg("value");
  value.toLowerCase();
  if (!validFan(value)) {
    sendError(400, "Invalid fan value");
    return;
  }
  air.fan = value;
  saveState();
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handleSwing() {
  noteActivity();
  String value = server.hasArg("value") ? server.arg("value") : "toggle";
  bool parsed;
  if (!parseSwitchArg(value, air.swing, parsed)) {
    sendError(400, "Invalid swing value");
    return;
  }
  air.swing = parsed;
  saveState();
  if (shouldSendIr()) sendCurrentAc();
  sendApi(stateJson());
}

void handleConfig() {
  noteActivity();
  if (server.hasArg("protocol")) {
    String protocol = server.arg("protocol");
    protocol.toUpperCase();
    const decode_type_t type = strToDecodeType(protocol.c_str());
    if (type == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(type)) {
      sendError(400, "Unsupported IR protocol");
      return;
    }
    acProtocol = protocol;
  }
  if (server.hasArg("model")) {
    acModel = server.arg("model").toInt();
  }
  saveConfig();
  sendApi(stateJson());
}

void handleSend() {
  noteActivity();
  sendCurrentAc();
  sendApi(stateJson());
}

void handleHelp() {
  noteActivity();
  sendApi(F("{\"ok\":true,\"endpoints\":[\"/api/state\",\"/api/control?power=on&mode=cool&temp=26&fan=auto&swing=off\",\"/api/power?value=toggle\",\"/api/temp?delta=1\",\"/api/mode?value=heat\",\"/api/fan?value=high\",\"/api/swing?value=toggle\",\"/api/send\",\"/api/config?protocol=GREE&model=1\"]}"));
}

void handleNotFound() {
  noteActivity();
  sendError(404, "Not found");
}

void drawWifiBars(int x, int y, int rssi) {
  const int bars = apMode ? 4 : (rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0);
  for (int i = 0; i < 4; i++) {
    const int h = 2 + i * 2;
    if (i < bars) {
      u8g2.drawBox(x + i * 4, y - h, 3, h);
    } else {
      u8g2.drawFrame(x + i * 4, y - h, 3, h);
    }
  }
}

void drawModeIcon(int x, int y) {
  if (air.mode == "cool") {
    u8g2.drawLine(x - 6, y, x + 6, y);
    u8g2.drawLine(x - 4, y - 5, x + 4, y + 5);
    u8g2.drawLine(x - 4, y + 5, x + 4, y - 5);
    u8g2.drawCircle(x, y, 2);
  } else if (air.mode == "heat") {
    u8g2.drawDisc(x, y, 5);
    for (int i = 0; i < 8; i++) {
      const float a = i * 0.785398f;
      u8g2.drawLine(x + cos(a) * 8, y + sin(a) * 8, x + cos(a) * 11, y + sin(a) * 11);
    }
  } else if (air.mode == "dry") {
    u8g2.drawTriangle(x, y - 9, x - 7, y + 3, x + 7, y + 3);
    u8g2.drawCircle(x, y + 3, 7);
  } else if (air.mode == "fan") {
    u8g2.drawCircle(x, y, 2);
    u8g2.drawEllipse(x, y - 5, 3, 7);
    u8g2.drawEllipse(x - 5, y + 3, 7, 3);
    u8g2.drawEllipse(x + 5, y + 3, 7, 3);
  } else {
    u8g2.drawFrame(x - 9, y - 9, 18, 18);
    u8g2.drawStr(x - 5, y + 4, "A");
  }
}

void drawFanIcon(int x, int y) {
  const uint8_t filled = air.fan == "high" ? 4 : air.fan == "medium" ? 3 : air.fan == "low" ? 2 : 1;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t h = 3 + i * 2;
    if (i < filled) {
      u8g2.drawBox(x + i * 5, y - h, 3, h);
    } else {
      u8g2.drawFrame(x + i * 5, y - h, 3, h);
    }
  }
}

void drawDisplay() {
  u8g2.clearBuffer();

  char buf[24] = {0};
  time_t now = time(nullptr);
  struct tm tmNow;
  const bool hasTime = now > 100000 && localtime_r(&now, &tmNow);

  u8g2.setFont(u8g2_font_6x10_tf);
  if (hasTime) {
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &tmNow);
    u8g2.drawStr(0, 8, buf);
  } else {
    u8g2.drawStr(0, 8, apMode ? "AP MODE" : "TIME --:--");
  }
  drawWifiBars(111, 8, WiFi.RSSI());
  u8g2.drawHLine(0, 11, 128);

  drawModeIcon(14, 31);
  u8g2.setFont(u8g2_font_logisoso28_tf);
  snprintf(buf, sizeof(buf), "%u", air.temp);
  u8g2.drawStr(39, 43, buf);
  u8g2.drawCircle(78, 20, 2);
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(82, 29, "C");

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(96, 24, air.power ? "ON" : "OFF");
  u8g2.drawFrame(94, 28, 23, 12);
  if (air.power) u8g2.drawBox(97, 31, 17, 6);

  u8g2.drawHLine(0, 47, 128);
  drawFanIcon(2, 61);
  u8g2.drawStr(24, 61, air.fan.c_str());
  u8g2.drawStr(64, 61, air.swing ? "SWING" : "FIX");

  if (lastIrError.length()) {
    u8g2.drawStr(102, 61, "IR?");
  } else if (lastIrSendMs && millis() - lastIrSendMs < 3000) {
    u8g2.drawStr(104, 61, "IR");
  }

  u8g2.sendBuffer();
}

void refreshBacklight() {
  if (backlightOn && millis() - lastWebActivityMs > BACKLIGHT_IDLE_MS) {
    setBacklight(false);
  }
}

void pollButton(ButtonState &button) {
  const bool raw = digitalRead(button.pin);
  const uint32_t now = millis();
  if (raw != button.lastRaw) {
    button.lastRaw = raw;
    button.lastChangeMs = now;
  }
  if (now - button.lastChangeMs < BUTTON_DEBOUNCE_MS) return;
  if (raw == button.stable) return;

  button.stable = raw;
  if (!button.stable) {
    noteActivity();
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  if (String(WIFI_SSID).length()) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(200);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    return;
  }

  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(IRSTATION_AP_SSID, IRSTATION_AP_PASSWORD);
}

void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/power", HTTP_GET, handlePower);
  server.on("/api/temp", HTTP_GET, handleTemp);
  server.on("/api/mode", HTTP_GET, handleMode);
  server.on("/api/fan", HTTP_GET, handleFan);
  server.on("/api/swing", HTTP_GET, handleSwing);
  server.on("/api/send", HTTP_GET, handleSend);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/help", HTTP_GET, handleHelp);
  server.onNotFound(handleNotFound);
  server.begin();
}

void setup() {
  pinMode(PIN_LCD_BL, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_IR_IN, INPUT);
  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  digitalWrite(PIN_LED, LOW);
  setBacklight(true);
  lastWebActivityMs = millis();

  Serial.begin(115200);
  delay(100);

  prefs.begin("irstation", false);
  loadState();

  u8g2.begin();
  u8g2.setContrast(20);
  drawDisplay();

  setupWiFi();
  setupWeb();
  drawDisplay();
}

void loop() {
  server.handleClient();
  pollButton(button1);
  pollButton(button2);
  refreshBacklight();

  if (millis() - lastDisplayRefreshMs > DISPLAY_REFRESH_MS) {
    lastDisplayRefreshMs = millis();
    drawDisplay();
  }
}
