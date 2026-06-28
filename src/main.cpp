#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <OneButton.h>
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
constexpr uint32_t BUTTON_LONG_PRESS_MS = 800UL;
constexpr uint32_t CONFIG_SAVE_DELAY_MS = 10UL * 1000UL;
constexpr uint16_t LCD_NOTICE_MS = 2200;
constexpr uint16_t LCD_NOTICE_SHORT_MS = 1400;
constexpr uint8_t DEFAULT_PRESET_OFF_HOUR = 7;
constexpr uint8_t DEFAULT_PRESET_OFF_MINUTE = 30;
constexpr uint8_t TEMP_MIN_C = 18;
constexpr uint8_t TEMP_MAX_C = 35;
constexpr uint16_t IR_CAPTURE_BUFFER_SIZE = 1024;
constexpr uint8_t IR_CAPTURE_TIMEOUT_MS = 50;
constexpr const char *CONFIG_FILE = "/config.json";

U8G2_ST7567_ERC12864_F_4W_SW_SPI u8g2(
    U8G2_R0,
    PIN_LCD_SCK,
    PIN_LCD_SDA,
    U8X8_PIN_NONE,
    PIN_LCD_DC,
    PIN_LCD_RST);

WebServer server(80);
IRac ac(PIN_IR_OUT);
IRrecv irrecv(PIN_IR_IN, IR_CAPTURE_BUFFER_SIZE, IR_CAPTURE_TIMEOUT_MS, true);
decode_results irResults;

struct AirState {
  bool power = false;
  uint8_t temp = 26;
  String mode = "cool";
  String fan = "auto";
  bool swing = false;
  bool displayLight = true;
};

struct AcCapabilities {
  const char *protocol;
  uint8_t tempMin;
  uint8_t tempMax;
  uint8_t manualFanMax;
  bool supportsSwing;
  bool supportsLight;
};

AirState air;
AirState editAir;
AirState lastIrAir;
OneButton button1(PIN_BTN1, true, true);
OneButton button2(PIN_BTN2, true, true);

enum class SettingItem : uint8_t {
  Mode,
  Temp,
  Fan,
  Swing,
  Light,
  Count,
};

enum class LcdNoticeKind : uint8_t {
  Info,
  Success,
  Warning,
  Error,
};

constexpr const char *MODE_VALUES[] = {"auto", "cool", "heat", "dry", "fan"};
constexpr const char *FAN_VALUES[] = {"auto", "1", "2", "3", "4", "5"};
constexpr uint8_t MODE_VALUE_COUNT = sizeof(MODE_VALUES) / sizeof(MODE_VALUES[0]);
constexpr uint8_t FAN_VALUE_COUNT = sizeof(FAN_VALUES) / sizeof(FAN_VALUES[0]);
constexpr uint8_t SETTING_ITEM_COUNT = static_cast<uint8_t>(SettingItem::Count);

constexpr AcCapabilities DEFAULT_CAPABILITIES = {"*", 16, 30, 3, true, false};
constexpr AcCapabilities PROTOCOL_CAPABILITIES[] = {
    {"GREE", 16, 30, 3, true, true},
    {"KELVINATOR", 16, 30, 5, true, true},
};
constexpr uint8_t PROTOCOL_CAPABILITY_COUNT = sizeof(PROTOCOL_CAPABILITIES) / sizeof(PROTOCOL_CAPABILITIES[0]);

String acProtocol = DEFAULT_AC_PROTOCOL;
int16_t acModel = DEFAULT_AC_MODEL;
String wifiSsid = WIFI_SSID;
String wifiPassword = WIFI_PASSWORD;
String lastIrError;
String lastIrRxProtocol;
String lastIrRxSummary;
String lastIrRxError;
String lcdNoticeText;
String configError;
int16_t lastIrRxModel = -1;
uint32_t lastIrSendMs = 0;
uint32_t lastIrRxMs = 0;
uint32_t lastWebActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t lcdNoticeUntilMs = 0;
bool backlightOn = false;
bool apMode = false;
bool fileSystemReady = false;
bool configLoaded = false;
bool wifiRestartRequired = false;
bool configSavePending = false;
bool settingsMode = false;
bool lastIrRxAcDecoded = false;
bool displayDirty = true;
bool presetEnabled = false;
bool presetScheduleActive = false;
uint8_t settingsIndex = 0;
uint8_t presetOffHour = DEFAULT_PRESET_OFF_HOUR;
uint8_t presetOffMinute = DEFAULT_PRESET_OFF_MINUTE;
uint32_t configSaveDueMs = 0;
time_t presetOffEpoch = 0;
LcdNoticeKind lcdNoticeKind = LcdNoticeKind::Info;

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
select{width:100%;min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;font-weight:650;padding:0 10px}
input{width:100%;min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;padding:0 10px}
.step{display:grid;grid-template-columns:54px 1fr 54px;gap:8px;align-items:center}.step button{font-size:26px}.value{text-align:center;font-size:32px;font-weight:800}
.form{display:grid;grid-template-columns:1fr 1fr auto;gap:8px}.presetForm{display:grid;grid-template-columns:1fr auto auto auto;gap:8px;align-items:center}.muted{color:var(--muted);font-size:13px;margin-top:8px}
.foot{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;color:var(--muted);font-size:13px}.foot span{border:1px solid var(--line);border-radius:999px;padding:5px 9px}
@media(max-width:520px){main{padding:12px}.grid{grid-template-columns:1fr}.form,.presetForm{grid-template-columns:1fr}.temp{font-size:58px}.power{width:78px;height:78px}.status{padding:14px}}
</style>
</head>
<body>
<main>
  <div class="top"><div class="brand">IRStation</div><div class="chip" id="net">Connecting</div></div>
  <section class="status">
    <div><div class="temp"><span id="temp">--</span><small>C</small></div><div class="meta"><span id="mode">--</span> / Fan <span id="fan">--</span> / Swing <span id="swing">--</span></div></div>
    <button class="power off" id="power" title="Power">I/O</button>
  </section>
  <section class="grid">
    <div class="panel"><div class="label">Temperature</div><div class="step"><button id="down">-</button><div class="value" id="temp2">--</div><button id="up">+</button></div></div>
    <div class="panel"><div class="label">Mode</div><div class="seg" id="modes">
      <button data-v="auto">Auto</button><button data-v="cool">Cool</button><button data-v="heat">Heat</button><button data-v="dry">Dry</button><button data-v="fan">Fan</button>
    </div></div>
    <div class="panel"><div class="label">Fan speed</div><div class="seg" id="fans">
      <button data-v="auto">Auto</button><button data-v="1">1</button><button data-v="2">2</button><button data-v="3">3</button><button data-v="4">4</button><button data-v="5">5</button>
    </div></div>
    <div class="panel"><div class="label">Swing</div><div class="seg" id="swings"><button data-v="off">Off</button><button data-v="on">On</button></div></div>
  </section>
  <div class="foot"><span id="proto">Protocol --</span><span id="ip">IP --</span><span id="rssi">RSSI --</span></div>
  <div class="panel" style="margin-top:12px"><div class="label">AC brand</div><select id="protocolSelect"></select></div>
  <div class="panel" id="lightPanel" style="margin-top:12px"><div class="label">Indoor display light</div><div class="seg" id="lights"><button data-v="off">Off</button><button data-v="on">On</button></div></div>
  <div class="panel" id="presetPanel" style="margin-top:12px"><div class="label">One-key off time</div><div class="seg" id="presetEnabled"><button data-v="off">Off</button><button data-v="on">On</button></div><div class="presetForm" style="margin-top:8px"><input id="presetTime" type="time"><button id="savePreset">Save</button><button id="runPreset">Start</button><button id="cancelPreset">Cancel</button></div><div class="muted" id="presetNote"></div></div>
  <div class="panel" id="irPanel" style="margin-top:12px"><div class="label">IR receiver</div><div class="foot"><span id="irrx">No signal</span></div><button id="applyIr" style="margin-top:8px">Apply learned state</button></div>
  <div class="panel" id="wifiPanel" style="margin-top:12px"><div class="label">Wi-Fi setup</div><div class="form"><input id="ssid" placeholder="SSID"><input id="password" type="password" placeholder="Password"><button id="saveWifi">Save</button></div><div class="muted" id="wifiNote"></div></div>
</main>
<script>
const labels={mode:{auto:'Auto',cool:'Cool',heat:'Heat',dry:'Dry',fan:'Fan'},fan:{auto:'Auto','1':'1','2':'2','3':'3','4':'4','5':'5'},swing:{true:'On',false:'Off'}};
let state=null;
async function api(path){const r=await fetch(path,{cache:'no-store'}); if(!r.ok)throw new Error(await r.text()); return r.json();}
function mark(group,value){document.querySelectorAll(group+' button').forEach(b=>b.classList.toggle('active',b.dataset.v==value));}
async function loadProtocols(){const s=await api('/api/protocols'); const el=document.getElementById('protocolSelect'); el.innerHTML=''; s.protocols.forEach(p=>{const o=document.createElement('option'); o.value=p; o.textContent=p; el.appendChild(o);});}
function renderCaps(s){
 const c=s.capabilities||{fanMax:5,displayLight:true}; document.querySelectorAll('#fans button').forEach(b=>{const v=b.dataset.v; b.style.display=(v=='auto'||Number(v)<=c.fanMax)?'':'none';});
 document.getElementById('lightPanel').style.display=c.displayLight?'block':'none';
}
function render(s){
 state=s.state; document.getElementById('temp').textContent=state.temp; document.getElementById('temp2').textContent=state.temp+'C';
 document.getElementById('mode').textContent=labels.mode[state.mode]||state.mode; document.getElementById('fan').textContent=labels.fan[state.fan]||state.fan;
 document.getElementById('swing').textContent=state.swing?'On':'Off'; document.getElementById('power').classList.toggle('off',!state.power);
 document.getElementById('net').textContent=s.device.wifi+' / '+s.device.ip; document.getElementById('ip').textContent='IP '+s.device.ip;
 document.getElementById('rssi').textContent='RSSI '+s.device.rssi; document.getElementById('proto').textContent='Protocol '+s.ir.protocol+'/'+s.ir.model;
 const protocolSelect=document.getElementById('protocolSelect'); if(protocolSelect.options.length) protocolSelect.value=s.ir.protocol;
 renderCaps(s);
 mark('#modes',state.mode); mark('#fans',state.fan); mark('#swings',state.swing?'on':'off'); mark('#lights',state.displayLight?'on':'off');
 const preset=s.preset||{enabled:false,offTime:'07:30',scheduleActive:false}; mark('#presetEnabled',preset.enabled?'on':'off'); const presetTime=document.getElementById('presetTime'); if(document.activeElement!==presetTime)presetTime.value=preset.offTime||'07:30'; document.getElementById('presetNote').textContent=preset.scheduleActive?('Scheduled for '+(preset.offTime||'--:--')):(preset.enabled?'B2 preset ready':'B2 power toggle'); document.getElementById('cancelPreset').disabled=!preset.scheduleActive;
 const rx=s.ir.rx||{}; document.getElementById('irrx').textContent=rx.lastMs?(rx.protocol+' '+(rx.acDecoded?'AC':'raw')+' '+(rx.summary||'')):'No signal'; document.getElementById('applyIr').disabled=!rx.acDecoded;
 document.getElementById('wifiPanel').style.display=(s.device.wifi=='ap'||s.config.wifiRestartRequired)?'block':'none'; document.getElementById('wifiNote').textContent=s.config.wifiRestartRequired?'Saved. Wait 10s, then reboot to use the new Wi-Fi.':'AP mode setup';
}
async function refresh(){try{render(await api('/api/state'))}catch(e){document.getElementById('net').textContent='Offline'}}
async function control(q){render(await api('/api/control?'+q));}
document.getElementById('power').onclick=()=>api('/api/power?value=toggle').then(render);
document.getElementById('up').onclick=()=>api('/api/temp?delta=1').then(render);
document.getElementById('down').onclick=()=>api('/api/temp?delta=-1').then(render);
document.querySelectorAll('#modes button').forEach(b=>b.onclick=()=>control('mode='+b.dataset.v));
document.querySelectorAll('#fans button').forEach(b=>b.onclick=()=>control('fan='+b.dataset.v));
document.querySelectorAll('#swings button').forEach(b=>b.onclick=()=>control('swing='+b.dataset.v));
document.querySelectorAll('#lights button').forEach(b=>b.onclick=()=>control('light='+b.dataset.v));
document.querySelectorAll('#presetEnabled button').forEach(b=>b.onclick=()=>api('/api/preset?enabled='+b.dataset.v).then(render));
document.getElementById('protocolSelect').onchange=e=>api('/api/config?protocol='+encodeURIComponent(e.target.value)).then(render);
document.getElementById('applyIr').onclick=()=>api('/api/ir/apply').then(render);
document.getElementById('savePreset').onclick=()=>api('/api/preset?time='+encodeURIComponent(document.getElementById('presetTime').value)).then(render);
document.getElementById('runPreset').onclick=()=>api('/api/preset/run').then(render);
document.getElementById('cancelPreset').onclick=()=>api('/api/preset/cancel').then(render);
document.getElementById('saveWifi').onclick=()=>api('/api/config?ssid='+encodeURIComponent(document.getElementById('ssid').value)+'&password='+encodeURIComponent(document.getElementById('password').value)).then(render);
loadProtocols().then(refresh).catch(refresh); setInterval(refresh,5000);
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

AcCapabilities capabilitiesForProtocol(String protocolName) {
  protocolName.toUpperCase();
  for (uint8_t i = 0; i < PROTOCOL_CAPABILITY_COUNT; i++) {
    if (protocolName == PROTOCOL_CAPABILITIES[i].protocol) return PROTOCOL_CAPABILITIES[i];
  }
  return DEFAULT_CAPABILITIES;
}

AcCapabilities currentCapabilities() {
  return capabilitiesForProtocol(acProtocol);
}

String normalizeFan(String value, const AcCapabilities &capabilities) {
  value.trim();
  value.toLowerCase();
  if (value == "auto" || value == "automatic") return "auto";
  if (value == "min" || value == "minimum" || value == "lowest") return "1";
  if (value == "low" || value == "lo") return capabilities.manualFanMax >= 5 ? "2" : "1";
  if (value == "medium" || value == "med" || value == "mid") return String((capabilities.manualFanMax + 1) / 2);
  if (value == "high" || value == "hi") return capabilities.manualFanMax >= 5 ? "4" : String(capabilities.manualFanMax);
  if (value == "max" || value == "maximum" || value == "highest") return String(capabilities.manualFanMax);
  if (value.length() == 1 && value[0] >= '1' && value[0] <= '5') {
    const uint8_t level = value.toInt();
    if (level >= 1 && level <= capabilities.manualFanMax) return value;
  }
  return "";
}

String normalizeFan(String value) {
  return normalizeFan(value, currentCapabilities());
}

bool validFan(const String &value) {
  return normalizeFan(value).length() > 0;
}

stdAc::opmode_t toStdMode(const String &mode) {
  if (mode == "auto") return stdAc::opmode_t::kAuto;
  if (mode == "heat") return stdAc::opmode_t::kHeat;
  if (mode == "dry") return stdAc::opmode_t::kDry;
  if (mode == "fan") return stdAc::opmode_t::kFan;
  return stdAc::opmode_t::kCool;
}

stdAc::fanspeed_t toStdFan(const String &fan) {
  const AcCapabilities capabilities = currentCapabilities();
  const String normalized = normalizeFan(fan, capabilities);
  if (capabilities.manualFanMax <= 3) {
    if (normalized == "1") return stdAc::fanspeed_t::kMin;
    if (normalized == "2") return stdAc::fanspeed_t::kMedium;
    if (normalized == "3") return stdAc::fanspeed_t::kMax;
    return stdAc::fanspeed_t::kAuto;
  }
  if (normalized == "1") return stdAc::fanspeed_t::kMin;
  if (normalized == "2") return stdAc::fanspeed_t::kLow;
  if (normalized == "3") return stdAc::fanspeed_t::kMedium;
  if (normalized == "4") return stdAc::fanspeed_t::kHigh;
  if (normalized == "5") return stdAc::fanspeed_t::kMax;
  return stdAc::fanspeed_t::kAuto;
}

String fanDisplayLabel(const String &fan) {
  const String normalized = normalizeFan(fan);
  if (normalized == "auto") return "AUTO";
  if (normalized.length()) return "F" + normalized;
  return "AUTO";
}

const char *modeDisplayLabel(const String &mode) {
  if (mode == "auto") return "AUTO";
  if (mode == "heat") return "HEAT";
  if (mode == "dry") return "DRY";
  if (mode == "fan") return "FAN";
  return "COOL";
}

int findValueIndex(const String &value, const char *const values[], uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (value == values[i]) return i;
  }
  return -1;
}

String cycleValue(const String &value, const char *const values[], uint8_t count, int8_t direction) {
  int index = findValueIndex(value, values, count);
  if (index < 0) index = 0;
  index = (index + direction + count) % count;
  return String(values[index]);
}

SettingItem currentSettingItem() {
  return static_cast<SettingItem>(settingsIndex % SETTING_ITEM_COUNT);
}

const char *settingItemTitle(SettingItem item) {
  switch (item) {
    case SettingItem::Mode: return "MODE";
    case SettingItem::Temp: return "TEMP";
    case SettingItem::Fan: return "FAN";
    case SettingItem::Swing: return "SWING";
    case SettingItem::Light: return "LIGHT";
    default: return "SET";
  }
}

bool settingItemAvailable(SettingItem item) {
  const AcCapabilities capabilities = currentCapabilities();
  if (item == SettingItem::Swing) return capabilities.supportsSwing;
  if (item == SettingItem::Light) return capabilities.supportsLight;
  return true;
}

uint8_t availableSettingCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < SETTING_ITEM_COUNT; i++) {
    if (settingItemAvailable(static_cast<SettingItem>(i))) count++;
  }
  return count;
}

uint8_t currentSettingPosition() {
  uint8_t position = 0;
  for (uint8_t i = 0; i < SETTING_ITEM_COUNT; i++) {
    const SettingItem item = static_cast<SettingItem>(i);
    if (!settingItemAvailable(item)) continue;
    position++;
    if (i == settingsIndex) return position;
  }
  return 1;
}

bool sameAirState(const AirState &a, const AirState &b) {
  return a.power == b.power &&
         a.temp == b.temp &&
         a.mode == b.mode &&
         a.fan == b.fan &&
         a.swing == b.swing &&
         a.displayLight == b.displayLight;
}

void setBacklight(bool on) {
  backlightOn = on;
  const uint8_t onLevel = LCD_BACKLIGHT_ACTIVE_LOW ? LOW : HIGH;
  digitalWrite(PIN_LCD_BL, on ? onLevel : !onLevel);
}

void noteActivity() {
  lastWebActivityMs = millis();
  setBacklight(true);
  displayDirty = true;
}

bool lcdNoticeActive() {
  return lcdNoticeText.length() && static_cast<int32_t>(millis() - lcdNoticeUntilMs) < 0;
}

void showLcdNotice(const String &text, LcdNoticeKind kind = LcdNoticeKind::Info, uint16_t durationMs = LCD_NOTICE_MS) {
  lcdNoticeText = text;
  lcdNoticeKind = kind;
  lcdNoticeUntilMs = millis() + durationMs;
  displayDirty = true;
}

bool timeIsReady() {
  return time(nullptr) > 100000;
}

bool parsePresetTime(String value, uint8_t &hour, uint8_t &minute) {
  value.trim();
  const int colon = value.indexOf(':');
  if (colon <= 0 || colon >= static_cast<int>(value.length()) - 1) return false;
  for (int i = 0; i < static_cast<int>(value.length()); i++) {
    if (i == colon) continue;
    if (!isDigit(value[i])) return false;
  }
  const int h = value.substring(0, colon).toInt();
  const int m = value.substring(colon + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  hour = static_cast<uint8_t>(h);
  minute = static_cast<uint8_t>(m);
  return true;
}

String presetOffTimeString() {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", presetOffHour, presetOffMinute);
  return String(buf);
}

time_t nextPresetOffEpoch() {
  const time_t now = time(nullptr);
  if (now <= 100000) return 0;

  struct tm target;
  localtime_r(&now, &target);
  target.tm_hour = presetOffHour;
  target.tm_min = presetOffMinute;
  target.tm_sec = 0;
  time_t epoch = mktime(&target);
  if (epoch <= now) epoch += 24L * 60L * 60L;
  return epoch;
}

void presetTimeLabel(char *buf, size_t len) {
  snprintf(buf, len, "%02u:%02u", presetOffHour, presetOffMinute);
}

void cancelPresetSchedule(bool showNotice = false) {
  if (!presetScheduleActive) return;
  presetScheduleActive = false;
  presetOffEpoch = 0;
  displayDirty = true;
  if (showNotice) showLcdNotice("PRESET CANCEL", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
}

bool validProtocol(const String &protocolName) {
  String proto = protocolName;
  proto.toUpperCase();
  const decode_type_t protocol = strToDecodeType(proto.c_str());
  return protocol != decode_type_t::UNKNOWN && IRac::isProtocolSupported(protocol);
}

bool normalizeConfig() {
  bool changed = false;

  acProtocol.toUpperCase();
  if (!validProtocol(acProtocol)) {
    acProtocol = DEFAULT_AC_PROTOCOL;
    acProtocol.toUpperCase();
    changed = true;
  }

  const AcCapabilities capabilities = currentCapabilities();
  if (air.temp < capabilities.tempMin || air.temp > capabilities.tempMax) {
    air.temp = constrain(air.temp, capabilities.tempMin, capabilities.tempMax);
    changed = true;
  }
  if (!validMode(air.mode)) {
    air.mode = "cool";
    changed = true;
  }
  const String normalizedFan = normalizeFan(air.fan, capabilities);
  if (!normalizedFan.length()) {
    air.fan = "auto";
    changed = true;
  } else if (normalizedFan != air.fan) {
    air.fan = normalizedFan;
    changed = true;
  }

  if (acModel < 0) {
    acModel = DEFAULT_AC_MODEL;
    changed = true;
  }

  if (presetOffHour > 23) {
    presetOffHour = DEFAULT_PRESET_OFF_HOUR;
    changed = true;
  }
  if (presetOffMinute > 59) {
    presetOffMinute = DEFAULT_PRESET_OFF_MINUTE;
    changed = true;
  }

  return changed;
}

bool saveConfigFile() {
  if (!fileSystemReady) {
    configError = "LittleFS is not mounted";
    return false;
  }

  normalizeConfig();

  JsonDocument doc;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = wifiSsid;
  wifi["password"] = wifiPassword;

  JsonObject state = doc["state"].to<JsonObject>();
  state["power"] = air.power;
  state["temp"] = air.temp;
  state["mode"] = air.mode;
  state["fan"] = air.fan;
  state["swing"] = air.swing;
  state["displayLight"] = air.displayLight;

  JsonObject ir = doc["ir"].to<JsonObject>();
  ir["protocol"] = acProtocol;
  ir["model"] = acModel;

  JsonObject preset = doc["preset"].to<JsonObject>();
  preset["enabled"] = presetEnabled;
  preset["offTime"] = presetOffTimeString();

  File file = LittleFS.open(CONFIG_FILE, "w");
  if (!file) {
    configError = "Unable to write config file";
    return false;
  }

  const size_t written = serializeJsonPretty(doc, file);
  file.println();
  file.close();

  if (!written) {
    configError = "Unable to serialize config file";
    return false;
  }

  configError = "";
  configLoaded = true;
  return true;
}

void scheduleConfigSave() {
  configSavePending = true;
  configSaveDueMs = millis() + CONFIG_SAVE_DELAY_MS;
}

void flushConfigSaveIfDue() {
  if (!configSavePending) return;
  if (static_cast<int32_t>(millis() - configSaveDueMs) < 0) return;
  if (saveConfigFile()) {
    configSavePending = false;
    showLcdNotice("CONFIG SAVED", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
  } else {
    configSaveDueMs = millis() + CONFIG_SAVE_DELAY_MS;
    showLcdNotice("SAVE FAILED", LcdNoticeKind::Error);
  }
}

void loadConfigFile() {
  configLoaded = false;
  if (!fileSystemReady) fileSystemReady = LittleFS.begin(false, "/littlefs", 10, "littlefs");
  if (!fileSystemReady) {
    configError = "LittleFS mount failed";
    normalizeConfig();
    return;
  }

  if (!LittleFS.exists(CONFIG_FILE)) {
    configError = "";
    normalizeConfig();
    saveConfigFile();
    return;
  }

  File file = LittleFS.open(CONFIG_FILE, "r");
  if (!file) {
    configError = "Unable to read config file";
    normalizeConfig();
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    configError = "Invalid config JSON: " + String(error.c_str());
    normalizeConfig();
    return;
  }

  JsonVariant wifiSsidValue = doc["wifi"]["ssid"];
  if (!wifiSsidValue.isNull()) wifiSsid = wifiSsidValue.as<String>();
  JsonVariant wifiPasswordValue = doc["wifi"]["password"];
  if (!wifiPasswordValue.isNull()) wifiPassword = wifiPasswordValue.as<String>();

  JsonVariant powerValue = doc["state"]["power"];
  if (!powerValue.isNull()) air.power = powerValue.as<bool>();
  JsonVariant tempValue = doc["state"]["temp"];
  if (!tempValue.isNull()) air.temp = tempValue.as<uint8_t>();
  JsonVariant modeValue = doc["state"]["mode"];
  if (!modeValue.isNull()) air.mode = modeValue.as<String>();
  JsonVariant fanValue = doc["state"]["fan"];
  if (!fanValue.isNull()) air.fan = fanValue.as<String>();
  JsonVariant swingValue = doc["state"]["swing"];
  if (!swingValue.isNull()) air.swing = swingValue.as<bool>();
  JsonVariant displayLightValue = doc["state"]["displayLight"];
  if (displayLightValue.isNull()) displayLightValue = doc["state"]["light"];
  if (!displayLightValue.isNull()) air.displayLight = displayLightValue.as<bool>();

  JsonVariant protocolValue = doc["ir"]["protocol"];
  if (!protocolValue.isNull()) acProtocol = protocolValue.as<String>();
  JsonVariant modelValue = doc["ir"]["model"];
  if (!modelValue.isNull()) acModel = modelValue.as<int16_t>();

  JsonVariant presetEnabledValue = doc["preset"]["enabled"];
  if (!presetEnabledValue.isNull()) presetEnabled = presetEnabledValue.as<bool>();
  JsonVariant presetTimeValue = doc["preset"]["offTime"];
  if (!presetTimeValue.isNull()) {
    uint8_t hour;
    uint8_t minute;
    if (parsePresetTime(presetTimeValue.as<String>(), hour, minute)) {
      presetOffHour = hour;
      presetOffMinute = minute;
    }
  }

  configLoaded = true;
  configError = "";
  if (normalizeConfig()) saveConfigFile();
}

void saveState() {
  scheduleConfigSave();
  showLcdNotice("SAVE IN 10s", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
}

void saveConfig() {
  scheduleConfigSave();
  showLcdNotice("CONFIG IN 10s", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
}

void exitPresetModeForPowerOverride() {
  if (!presetScheduleActive && !presetEnabled) return;
  cancelPresetSchedule(false);
  presetEnabled = false;
  saveConfig();
  showLcdNotice("PRESET EXIT", LcdNoticeKind::Info);
}

bool sendCurrentAc() {
  String proto = acProtocol;
  proto.toUpperCase();
  const decode_type_t protocol = strToDecodeType(proto.c_str());
  if (protocol == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(protocol)) {
    lastIrError = "Unsupported protocol: " + acProtocol;
    showLcdNotice("IR UNSUPPORT", LcdNoticeKind::Error);
    return false;
  }

  const AcCapabilities capabilities = currentCapabilities();
  ac.next.protocol = protocol;
  ac.next.model = acModel;
  ac.next.power = air.power;
  ac.next.mode = toStdMode(air.mode);
  ac.next.celsius = true;
  ac.next.degrees = air.temp;
  ac.next.fanspeed = toStdFan(air.fan);
  ac.next.swingv = capabilities.supportsSwing && air.swing ? stdAc::swingv_t::kAuto : stdAc::swingv_t::kOff;
  ac.next.swingh = stdAc::swingh_t::kOff;
  ac.next.quiet = false;
  ac.next.turbo = false;
  ac.next.econo = false;
  ac.next.light = capabilities.supportsLight ? air.displayLight : false;
  ac.next.filter = false;
  ac.next.clean = false;
  ac.next.beep = true;
  ac.next.sleep = -1;
  ac.next.clock = -1;

  if (!ac.sendAc()) {
    lastIrError = "IR send failed for protocol: " + acProtocol;
    showLcdNotice("IR SEND FAIL", LcdNoticeKind::Error);
    return false;
  }

  lastIrError = "";
  lastIrSendMs = millis();
  showLcdNotice("IR SENT", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
  digitalWrite(PIN_LED, HIGH);
  delay(35);
  digitalWrite(PIN_LED, LOW);
  return true;
}

bool runPresetAction() {
  const time_t offEpoch = nextPresetOffEpoch();
  if (!offEpoch) {
    showLcdNotice("TIME NOT SET", LcdNoticeKind::Error);
    return false;
  }

  if (!air.power) {
    air.power = true;
    normalizeConfig();
    saveState();
    if (!sendCurrentAc()) return false;
  }

  presetScheduleActive = true;
  presetOffEpoch = offEpoch;
  showLcdNotice("OFF TIME SET", LcdNoticeKind::Success);
  return true;
}

void processPresetSchedule() {
  if (!presetScheduleActive) return;
  const time_t now = time(nullptr);
  if (now <= 100000 || now < presetOffEpoch) return;

  presetScheduleActive = false;
  presetOffEpoch = 0;
  noteActivity();

  if (!air.power) {
    showLcdNotice("OFF TIME DONE", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
    return;
  }

  air.power = false;
  if (settingsMode) editAir.power = false;
  saveState();
  if (sendCurrentAc()) showLcdNotice("TIME OFF", LcdNoticeKind::Success);
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
  const AcCapabilities capabilities = currentCapabilities();
  String json;
  json.reserve(1160);
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
  json += ",\"displayLight\":";
  json += air.displayLight ? "true" : "false";
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
  json += ",\"settingsMode\":";
  json += settingsMode ? "true" : "false";
  json += "}";
  json += ",\"capabilities\":{\"tempMin\":";
  json += capabilities.tempMin;
  json += ",\"tempMax\":";
  json += capabilities.tempMax;
  json += ",\"fanMax\":";
  json += capabilities.manualFanMax;
  json += ",\"swing\":";
  json += capabilities.supportsSwing ? "true" : "false";
  json += ",\"displayLight\":";
  json += capabilities.supportsLight ? "true" : "false";
  json += "}";
  json += ",\"preset\":{\"enabled\":";
  json += presetEnabled ? "true" : "false";
  json += ",\"offTime\":\"";
  json += presetOffTimeString();
  json += "\",\"nextOffEpoch\":";
  json += presetScheduleActive ? static_cast<long>(presetOffEpoch) : 0;
  json += ",\"scheduleActive\":";
  json += presetScheduleActive ? "true" : "false";
  json += "}";
  json += ",\"ir\":{\"protocol\":\"";
  json += jsonEscape(acProtocol);
  json += "\",\"model\":";
  json += acModel;
  json += ",\"lastSendMs\":";
  json += lastIrSendMs;
  json += ",\"lastError\":\"";
  json += jsonEscape(lastIrError);
  json += "\",\"rx\":{\"protocol\":\"";
  json += jsonEscape(lastIrRxProtocol);
  json += "\",\"model\":";
  json += lastIrRxModel;
  json += ",\"lastMs\":";
  json += lastIrRxMs;
  json += ",\"acDecoded\":";
  json += lastIrRxAcDecoded ? "true" : "false";
  json += ",\"summary\":\"";
  json += jsonEscape(lastIrRxSummary);
  json += "\",\"error\":\"";
  json += jsonEscape(lastIrRxError);
  json += "\"}";
  json += "}";
  json += ",\"config\":{\"file\":\"";
  json += CONFIG_FILE;
  json += "\",\"loaded\":";
  json += configLoaded ? "true" : "false";
  json += ",\"fsReady\":";
  json += fileSystemReady ? "true" : "false";
  json += ",\"fsTotal\":";
  json += fileSystemReady ? LittleFS.totalBytes() : 0;
  json += ",\"fsUsed\":";
  json += fileSystemReady ? LittleFS.usedBytes() : 0;
  json += ",\"wifiRestartRequired\":";
  json += wifiRestartRequired ? "true" : "false";
  json += ",\"savePending\":";
  json += configSavePending ? "true" : "false";
  json += ",\"saveDelayMs\":";
  json += CONFIG_SAVE_DELAY_MS;
  json += ",\"error\":\"";
  json += jsonEscape(configError);
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

bool rejectIfSettingsMode() {
  if (!settingsMode) return false;
  showLcdNotice("SET ACTIVE", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
  sendError(409, "LCD settings mode active");
  return true;
}

String modeFromStd(stdAc::opmode_t mode) {
  switch (mode) {
    case stdAc::opmode_t::kAuto: return "auto";
    case stdAc::opmode_t::kHeat: return "heat";
    case stdAc::opmode_t::kDry: return "dry";
    case stdAc::opmode_t::kFan: return "fan";
    case stdAc::opmode_t::kCool:
    default: return "cool";
  }
}

String fanFromStd(stdAc::fanspeed_t fan, const AcCapabilities &capabilities) {
  if (fan == stdAc::fanspeed_t::kAuto) return "auto";
  if (capabilities.manualFanMax <= 3) {
    switch (fan) {
      case stdAc::fanspeed_t::kMin:
      case stdAc::fanspeed_t::kLow: return "1";
      case stdAc::fanspeed_t::kMedium:
      case stdAc::fanspeed_t::kMediumHigh: return "2";
      case stdAc::fanspeed_t::kHigh:
      case stdAc::fanspeed_t::kMax: return "3";
      default: return "auto";
    }
  }
  switch (fan) {
    case stdAc::fanspeed_t::kMin: return "1";
    case stdAc::fanspeed_t::kLow: return "2";
    case stdAc::fanspeed_t::kMedium: return "3";
    case stdAc::fanspeed_t::kHigh:
    case stdAc::fanspeed_t::kMediumHigh: return "4";
    case stdAc::fanspeed_t::kMax: return "5";
    default: return "auto";
  }
}

uint8_t tempFromStd(const stdAc::state_t &state, const AcCapabilities &capabilities) {
  const float celsius = state.celsius ? state.degrees : (state.degrees - 32.0f) * 5.0f / 9.0f;
  const int rounded = static_cast<int>(celsius + 0.5f);
  return constrain(rounded, capabilities.tempMin, capabilities.tempMax);
}

AirState airFromStdState(const stdAc::state_t &state, const AcCapabilities &capabilities) {
  AirState next;
  next.power = state.power && state.mode != stdAc::opmode_t::kOff;
  next.temp = tempFromStd(state, capabilities);
  next.mode = modeFromStd(state.mode);
  next.fan = fanFromStd(state.fanspeed, capabilities);
  next.swing = capabilities.supportsSwing && state.swingv != stdAc::swingv_t::kOff;
  next.displayLight = capabilities.supportsLight ? state.light : air.displayLight;
  return next;
}

String irRxJson() {
  String json;
  json.reserve(640);
  json += "{\"ok\":true,\"rx\":{\"protocol\":\"";
  json += jsonEscape(lastIrRxProtocol);
  json += "\",\"model\":";
  json += lastIrRxModel;
  json += ",\"lastMs\":";
  json += lastIrRxMs;
  json += ",\"acDecoded\":";
  json += lastIrRxAcDecoded ? "true" : "false";
  json += ",\"summary\":\"";
  json += jsonEscape(lastIrRxSummary);
  json += "\",\"error\":\"";
  json += jsonEscape(lastIrRxError);
  json += "\"}";
  if (lastIrRxAcDecoded) {
    json += ",\"state\":{\"power\":";
    json += lastIrAir.power ? "true" : "false";
    json += ",\"temp\":";
    json += lastIrAir.temp;
    json += ",\"mode\":\"";
    json += lastIrAir.mode;
    json += "\",\"fan\":\"";
    json += lastIrAir.fan;
    json += "\",\"swing\":";
    json += lastIrAir.swing ? "true" : "false";
    json += ",\"displayLight\":";
    json += lastIrAir.displayLight ? "true" : "false";
    json += "}";
  }
  json += "}";
  return json;
}

String protocolsJson() {
  constexpr uint8_t MAX_PROTOCOLS = 80;
  String protocols[MAX_PROTOCOLS];
  uint8_t count = 0;

  for (int i = 0; i <= static_cast<int>(decode_type_t::kLastDecodeType); i++) {
    const decode_type_t protocol = static_cast<decode_type_t>(i);
    if (!IRac::isProtocolSupported(protocol)) continue;

    const String name = typeToString(protocol);
    if (!name.length() || name == "UNKNOWN") continue;
    if (count >= MAX_PROTOCOLS) break;

    int insertAt = count;
    while (insertAt > 0 && protocols[insertAt - 1].compareTo(name) > 0) {
      protocols[insertAt] = protocols[insertAt - 1];
      insertAt--;
    }
    protocols[insertAt] = name;
    count++;
  }

  String json;
  json.reserve(1024);
  json += "{\"ok\":true,\"current\":\"";
  json += jsonEscape(acProtocol);
  json += "\",\"protocols\":[";
  for (uint8_t i = 0; i < count; i++) {
    if (i) json += ",";
    json += "\"";
    json += jsonEscape(protocols[i]);
    json += "\"";
  }
  json += "]}";
  return json;
}

bool shouldSendIr() {
  if (!server.hasArg("send")) return true;
  String value = server.arg("send");
  value.toLowerCase();
  return !isFalsy(value);
}

bool sendIrIfRequested() {
  if (!shouldSendIr()) return true;
  if (sendCurrentAc()) return true;
  sendError(500, lastIrError.length() ? lastIrError : "IR send failed");
  return false;
}

bool applyControlArgs(AirState &target, bool &changed) {
  changed = false;
  const AcCapabilities capabilities = currentCapabilities();

  if (server.hasArg("power")) {
    bool value;
    if (!parseSwitchArg(server.arg("power"), target.power, value)) return false;
    target.power = value;
    changed = true;
  }

  if (server.hasArg("temp")) {
    int value = server.arg("temp").toInt();
    if (value < capabilities.tempMin || value > capabilities.tempMax) return false;
    target.temp = value;
    changed = true;
  }

  if (server.hasArg("delta")) {
    int value = static_cast<int>(target.temp) + server.arg("delta").toInt();
    target.temp = constrain(value, capabilities.tempMin, capabilities.tempMax);
    changed = true;
  }

  if (server.hasArg("mode")) {
    String value = server.arg("mode");
    value.toLowerCase();
    if (!validMode(value)) return false;
    target.mode = value;
    changed = true;
  }

  if (server.hasArg("fan")) {
    String value = normalizeFan(server.arg("fan"));
    if (!value.length()) return false;
    target.fan = value;
    changed = true;
  }

  if (server.hasArg("swing")) {
    bool value;
    if (!parseSwitchArg(server.arg("swing"), target.swing, value)) return false;
    target.swing = value;
    changed = true;
  }

  if (server.hasArg("light") || server.hasArg("displayLight")) {
    bool value;
    const String arg = server.hasArg("light") ? server.arg("light") : server.arg("displayLight");
    if (!capabilities.supportsLight) return false;
    if (!parseSwitchArg(arg, target.displayLight, value)) return false;
    target.displayLight = value;
    changed = true;
  }

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

void handleProtocols() {
  noteActivity();
  sendApi(protocolsJson());
}

void handleControl() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  AirState next = air;
  bool changed = false;
  if (!applyControlArgs(next, changed)) {
    sendError(400, "Invalid control argument");
    return;
  }
  if (presetScheduleActive && server.hasArg("power")) exitPresetModeForPowerOverride();
  if (changed) {
    air = next;
    normalizeConfig();
    if (!air.power) cancelPresetSchedule(false);
    saveState();
  }
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handlePower() {
  String value = server.hasArg("value") ? server.arg("value") : "toggle";
  noteActivity();
  if (rejectIfSettingsMode()) return;

  bool parsed;
  if (!parseSwitchArg(value, air.power, parsed)) {
    sendError(400, "Invalid power value");
    return;
  }
  if (presetScheduleActive) exitPresetModeForPowerOverride();
  air.power = parsed;
  if (!air.power) cancelPresetSchedule(false);
  saveState();
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleTemp() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  const AcCapabilities capabilities = currentCapabilities();
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    if (value < capabilities.tempMin || value > capabilities.tempMax) {
      sendError(400, "Temperature out of range");
      return;
    }
    air.temp = value;
  } else if (server.hasArg("delta")) {
    int value = static_cast<int>(air.temp) + server.arg("delta").toInt();
    air.temp = constrain(value, capabilities.tempMin, capabilities.tempMax);
  } else {
    sendError(400, "Missing value or delta");
    return;
  }
  saveState();
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleMode() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

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
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleFan() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  if (!server.hasArg("value")) {
    sendError(400, "Missing fan value");
    return;
  }
  String value = server.arg("value");
  value = normalizeFan(value);
  if (!value.length()) {
    sendError(400, "Invalid fan value");
    return;
  }
  air.fan = value;
  saveState();
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleSwing() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  String value = server.hasArg("value") ? server.arg("value") : "toggle";
  bool parsed;
  if (!parseSwitchArg(value, air.swing, parsed)) {
    sendError(400, "Invalid swing value");
    return;
  }
  air.swing = parsed;
  saveState();
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleLight() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  if (!currentCapabilities().supportsLight) {
    sendError(400, "Display light unsupported by current protocol");
    return;
  }
  String value = server.hasArg("value") ? server.arg("value") : "toggle";
  bool parsed;
  if (!parseSwitchArg(value, air.displayLight, parsed)) {
    sendError(400, "Invalid light value");
    return;
  }
  air.displayLight = parsed;
  saveState();
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

bool parsePresetTimeArg(uint8_t &hour, uint8_t &minute) {
  if (server.hasArg("time") || server.hasArg("offTime")) {
    const String arg = server.hasArg("time") ? server.arg("time") : server.arg("offTime");
    return parsePresetTime(arg, hour, minute);
  }
  return false;
}

void handlePreset() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  bool changed = false;
  if (server.hasArg("enabled") || server.hasArg("value")) {
    bool enabled;
    const String value = server.hasArg("enabled") ? server.arg("enabled") : server.arg("value");
    if (!parseSwitchArg(value, presetEnabled, enabled)) {
      sendError(400, "Invalid preset enabled value");
      return;
    }
    presetEnabled = enabled;
    if (!presetEnabled) cancelPresetSchedule(false);
    changed = true;
  }

  if (server.hasArg("time") || server.hasArg("offTime")) {
    uint8_t hour;
    uint8_t minute;
    if (!parsePresetTimeArg(hour, minute)) {
      sendError(400, "Invalid preset off time");
      return;
    }
    presetOffHour = hour;
    presetOffMinute = minute;
    if (presetScheduleActive) {
      cancelPresetSchedule(false);
      if (!runPresetAction()) {
        sendError(500, lastIrError.length() ? lastIrError : "Preset reschedule failed");
        return;
      }
    }
    changed = true;
  }

  if (changed) {
    normalizeConfig();
    saveConfig();
    showLcdNotice(presetEnabled ? "PRESET ON" : "PRESET OFF", LcdNoticeKind::Info);
  }

  if (server.hasArg("run") && !isFalsy(server.arg("run"))) {
    if (!runPresetAction()) {
      sendError(500, lastIrError.length() ? lastIrError : "Preset start failed");
      return;
    }
  }

  sendApi(stateJson());
}

void handlePresetRun() {
  noteActivity();
  if (rejectIfSettingsMode()) return;
  if (!runPresetAction()) {
    sendError(500, lastIrError.length() ? lastIrError : "Preset start failed");
    return;
  }
  sendApi(stateJson());
}

void handlePresetCancel() {
  noteActivity();
  if (rejectIfSettingsMode()) return;
  cancelPresetSchedule(true);
  sendApi(stateJson());
}

void handleConfig() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  bool wifiChanged = false;
  bool irChanged = false;
  if (server.hasArg("protocol")) {
    String protocol = server.arg("protocol");
    protocol.toUpperCase();
    if (!validProtocol(protocol)) {
      sendError(400, "Unsupported IR protocol");
      return;
    }
    acProtocol = protocol;
    irChanged = true;
  }
  if (server.hasArg("model")) {
    acModel = server.arg("model").toInt();
    irChanged = true;
  }
  if (server.hasArg("ssid")) {
    wifiSsid = server.arg("ssid");
    wifiChanged = true;
  }
  if (server.hasArg("password")) {
    wifiPassword = server.arg("password");
    wifiChanged = true;
  }
  if (wifiChanged) wifiRestartRequired = true;
  normalizeConfig();
  saveConfig();
  if (wifiChanged) {
    showLcdNotice("WIFI SAVE 10s", LcdNoticeKind::Info);
  } else if (irChanged) {
    showLcdNotice("BRAND SAVE 10s", LcdNoticeKind::Info);
  }
  sendApi(stateJson());
}

void handleReloadConfig() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  const String oldSsid = wifiSsid;
  const String oldPassword = wifiPassword;
  configSavePending = false;
  loadConfigFile();
  if (wifiSsid != oldSsid || wifiPassword != oldPassword) wifiRestartRequired = true;
  showLcdNotice(configError.length() ? "CONFIG ERROR" : "CONFIG LOADED",
                configError.length() ? LcdNoticeKind::Error : LcdNoticeKind::Success);
  sendApi(stateJson());
}

void handleSend() {
  noteActivity();
  if (rejectIfSettingsMode()) return;
  if (!sendIrIfRequested()) return;
  sendApi(stateJson());
}

void handleIrStatus() {
  noteActivity();
  sendApi(irRxJson());
}

void handleIrApply() {
  noteActivity();
  if (rejectIfSettingsMode()) return;
  if (!lastIrRxAcDecoded) {
    sendError(400, "No decoded A/C IR state available");
    return;
  }

  const bool updateProtocol = !server.hasArg("protocol") || !isFalsy(server.arg("protocol"));
  if (updateProtocol && lastIrRxProtocol.length() && validProtocol(lastIrRxProtocol)) {
    acProtocol = lastIrRxProtocol;
    if (lastIrRxModel >= 0) acModel = lastIrRxModel;
  }
  air = lastIrAir;
  normalizeConfig();
  if (!air.power) cancelPresetSchedule(false);
  saveState();
  showLcdNotice("IR APPLIED", LcdNoticeKind::Success);
  sendApi(stateJson());
}

void handleHelp() {
  noteActivity();
  sendApi(F("{\"ok\":true,\"configFile\":\"/config.json\",\"endpoints\":[\"/api/state\",\"/api/protocols\",\"/api/control?power=on&mode=cool&temp=26&fan=auto&swing=off&light=on\",\"/api/power?value=toggle\",\"/api/temp?delta=1\",\"/api/mode?value=heat\",\"/api/fan?value=5\",\"/api/swing?value=toggle\",\"/api/light?value=toggle\",\"/api/preset?enabled=on&time=07:30\",\"/api/preset/run\",\"/api/preset/cancel\",\"/api/send\",\"/api/ir\",\"/api/ir/apply\",\"/api/config?protocol=KELVINATOR&model=1\",\"/api/config?ssid=YOUR_WIFI&password=YOUR_PASSWORD\",\"/api/reload-config\"]}"));
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

void drawModeIcon(const AirState &state, int x, int y) {
  if (state.mode == "cool") {
    u8g2.drawLine(x - 6, y, x + 6, y);
    u8g2.drawLine(x - 4, y - 5, x + 4, y + 5);
    u8g2.drawLine(x - 4, y + 5, x + 4, y - 5);
    u8g2.drawCircle(x, y, 2);
  } else if (state.mode == "heat") {
    u8g2.drawDisc(x, y, 5);
    for (int i = 0; i < 8; i++) {
      const float a = i * 0.785398f;
      u8g2.drawLine(x + cos(a) * 8, y + sin(a) * 8, x + cos(a) * 11, y + sin(a) * 11);
    }
  } else if (state.mode == "dry") {
    u8g2.drawTriangle(x, y - 9, x - 7, y + 3, x + 7, y + 3);
    u8g2.drawCircle(x, y + 3, 7);
  } else if (state.mode == "fan") {
    u8g2.drawCircle(x, y, 2);
    u8g2.drawEllipse(x, y - 5, 3, 7);
    u8g2.drawEllipse(x - 5, y + 3, 7, 3);
    u8g2.drawEllipse(x + 5, y + 3, 7, 3);
  } else {
    u8g2.drawFrame(x - 9, y - 9, 18, 18);
    u8g2.drawStr(x - 5, y + 4, "A");
  }
}

void drawFanIcon(const AirState &state, int x, int y) {
  const String normalized = normalizeFan(state.fan);
  const uint8_t maxBars = currentCapabilities().manualFanMax;
  const uint8_t filled = normalized == "auto" ? 1 : normalized.toInt();
  for (uint8_t i = 0; i < maxBars; i++) {
    const uint8_t h = 2 + i * 2;
    if (i < filled) {
      u8g2.drawBox(x + i * 5, y - h, 3, h);
    } else {
      u8g2.drawFrame(x + i * 5, y - h, 3, h);
    }
  }
}

const char *noticeIconText() {
  switch (lcdNoticeKind) {
    case LcdNoticeKind::Success: return "OK";
    case LcdNoticeKind::Warning: return "!";
    case LcdNoticeKind::Error: return "!!";
    case LcdNoticeKind::Info:
    default: return "i";
  }
}

String clippedLcdText(String text, uint8_t maxChars) {
  if (text.length() <= maxChars) return text;
  return text.substring(0, maxChars - 1) + ".";
}

void drawNoticeOverlay() {
  if (!lcdNoticeActive()) return;

  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 50, 128, 14);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(3, 61, noticeIconText());
  const String text = clippedLcdText(lcdNoticeText, 17);
  u8g2.drawStr(22, 61, text.c_str());
  u8g2.setDrawColor(1);
}

void drawSettingProgress(uint8_t active, uint8_t total) {
  if (!total) return;
  const uint8_t startX = 128 - total * 6;
  for (uint8_t i = 0; i < total; i++) {
    const uint8_t x = startX + i * 6;
    if (i + 1 == active) {
      u8g2.drawBox(x, 15, 4, 4);
    } else {
      u8g2.drawFrame(x, 15, 4, 4);
    }
  }
}

void drawToggleIcon(int x, int y, bool on) {
  u8g2.drawFrame(x, y, 28, 14);
  u8g2.drawBox(on ? x + 15 : x + 3, y + 3, 10, 8);
}

void drawTopBar(const char *leftText) {
  char buf[24] = {0};
  time_t now = time(nullptr);
  struct tm tmNow;
  const bool hasTime = now > 100000 && localtime_r(&now, &tmNow);

  u8g2.setFont(u8g2_font_6x10_tf);
  if (leftText && leftText[0]) {
    u8g2.drawStr(0, 8, leftText);
  } else if (hasTime) {
    strftime(buf, sizeof(buf), "%m-%d %H:%M", &tmNow);
    u8g2.drawStr(0, 8, buf);
  } else {
    u8g2.drawStr(0, 8, apMode ? "AP MODE" : "TIME --:--");
  }
  u8g2.drawStr(93, 8, apMode ? "AP" : "STA");
  drawWifiBars(112, 8, WiFi.RSSI());
  u8g2.drawHLine(0, 11, 128);
}

void drawSettingsDisplay() {
  u8g2.clearBuffer();

  char buf[24] = {0};
  const uint8_t settingCount = availableSettingCount();
  const uint8_t settingPosition = currentSettingPosition();
  snprintf(buf, sizeof(buf), "SET %u/%u", settingPosition, settingCount);
  drawTopBar(buf);

  const SettingItem item = currentSettingItem();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawBox(0, 14, 42, 10);
  u8g2.setDrawColor(0);
  u8g2.drawStr(3, 22, settingItemTitle(item));
  u8g2.setDrawColor(1);
  if (!sameAirState(editAir, air)) u8g2.drawStr(48, 22, "EDIT");
  drawSettingProgress(settingPosition, settingCount);

  switch (item) {
    case SettingItem::Mode:
      drawModeIcon(editAir, 20, 39);
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(50, 42, modeDisplayLabel(editAir.mode));
      break;
    case SettingItem::Temp:
      u8g2.setFont(u8g2_font_logisoso28_tf);
      snprintf(buf, sizeof(buf), "%u", editAir.temp);
      u8g2.drawStr(38, 44, buf);
      u8g2.drawCircle(78, 20, 2);
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(82, 29, "C");
      snprintf(buf, sizeof(buf), "%u-%u", currentCapabilities().tempMin, currentCapabilities().tempMax);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(92, 42, buf);
      break;
    case SettingItem::Fan: {
      drawFanIcon(editAir, 16, 44);
      const String label = fanDisplayLabel(editAir.fan);
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(54, 42, label.c_str());
      snprintf(buf, sizeof(buf), "1-%u", currentCapabilities().manualFanMax);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.drawStr(98, 42, buf);
      break;
    }
    case SettingItem::Swing:
      drawToggleIcon(16, 29, editAir.swing);
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(54, 42, editAir.swing ? "ON" : "OFF");
      break;
    case SettingItem::Light:
      drawToggleIcon(16, 29, editAir.displayLight);
      u8g2.setFont(u8g2_font_6x12_tf);
      u8g2.drawStr(54, 42, editAir.displayLight ? "ON" : "OFF");
      break;
    default:
      break;
  }

  u8g2.drawHLine(0, 49, 128);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 61, "B1 NEXT  B2 +/-");

  drawNoticeOverlay();
  u8g2.sendBuffer();
  displayDirty = false;
}

void drawDisplay() {
  if (settingsMode) {
    drawSettingsDisplay();
    return;
  }

  u8g2.clearBuffer();
  char buf[24] = {0};
  drawTopBar(nullptr);

  drawModeIcon(air, 14, 31);
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
  drawFanIcon(air, 2, 61);
  const String fanLabel = fanDisplayLabel(air.fan);
  u8g2.drawStr(30, 61, fanLabel.c_str());
  if (presetScheduleActive) {
    presetTimeLabel(buf, sizeof(buf));
    u8g2.drawStr(64, 61, buf);
  } else {
    u8g2.drawStr(64, 61, air.swing ? "SWING" : "FIX");
  }
  const AcCapabilities capabilities = currentCapabilities();
  u8g2.drawStr(94, 61, capabilities.supportsLight ? (air.displayLight ? "LCD" : "lcd") : "--");

  const char *statusMark = "";
  if (lastIrError.length()) {
    statusMark = "!";
  } else if (lastIrSendMs && millis() - lastIrSendMs < 3000) {
    statusMark = "*";
  } else if (lastIrRxAcDecoded && lastIrRxMs && millis() - lastIrRxMs < 5000) {
    statusMark = "R";
  } else if (configSavePending) {
    statusMark = "S";
  }
  if (statusMark[0]) u8g2.drawStr(121, 61, statusMark);

  drawNoticeOverlay();
  u8g2.sendBuffer();
  displayDirty = false;
}

void refreshBacklight() {
  if (backlightOn && millis() - lastWebActivityMs > BACKLIGHT_IDLE_MS) {
    setBacklight(false);
  }
}

void stepDraftTemp(int8_t direction) {
  const AcCapabilities capabilities = currentCapabilities();
  if (direction > 0) {
    editAir.temp = editAir.temp >= capabilities.tempMax ? capabilities.tempMin : editAir.temp + 1;
  } else {
    editAir.temp = editAir.temp <= capabilities.tempMin ? capabilities.tempMax : editAir.temp - 1;
  }
}

void stepDraftSetting(int8_t direction) {
  switch (currentSettingItem()) {
    case SettingItem::Mode:
      editAir.mode = cycleValue(editAir.mode, MODE_VALUES, MODE_VALUE_COUNT, direction);
      break;
    case SettingItem::Temp:
      stepDraftTemp(direction);
      break;
    case SettingItem::Fan:
      editAir.fan = cycleValue(normalizeFan(editAir.fan), FAN_VALUES, currentCapabilities().manualFanMax + 1, direction);
      break;
    case SettingItem::Swing:
      editAir.swing = !editAir.swing;
      break;
    case SettingItem::Light:
      editAir.displayLight = !editAir.displayLight;
      break;
    default:
      break;
  }
}

void enterSettingsMode() {
  editAir = air;
  settingsIndex = 0;
  settingsMode = true;
  noteActivity();
  showLcdNotice("SETTINGS", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
  drawDisplay();
}

void commitSettingsMode() {
  air = editAir;
  normalizeConfig();
  settingsMode = false;
  saveState();
  showLcdNotice("SET SAVED", LcdNoticeKind::Success);
  noteActivity();
  drawDisplay();
}

void handleButton1Click() {
  noteActivity();
  if (settingsMode) {
    do {
      settingsIndex = (settingsIndex + 1) % SETTING_ITEM_COUNT;
    } while (!settingItemAvailable(currentSettingItem()));
    drawDisplay();
  } else {
    showLcdNotice("LCD ON", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
  }
}

void handleButton1LongPress() {
  if (settingsMode) {
    commitSettingsMode();
  } else {
    enterSettingsMode();
  }
}

void handleButton2Click() {
  noteActivity();
  if (settingsMode) {
    stepDraftSetting(1);
  } else if (presetScheduleActive) {
    exitPresetModeForPowerOverride();
    air.power = !air.power;
    saveState();
    sendCurrentAc();
  } else if (presetEnabled) {
    runPresetAction();
  } else {
    air.power = !air.power;
    if (!air.power) cancelPresetSchedule(false);
    saveState();
    sendCurrentAc();
  }
  drawDisplay();
}

void handleButton2LongPress() {
  noteActivity();
  if (settingsMode) {
    stepDraftSetting(-1);
    drawDisplay();
  }
}

void pollIrReceiver() {
  if (!irrecv.decode(&irResults)) return;

  const uint32_t now = millis();
  if (lastIrSendMs && now - lastIrSendMs < 700) return;

  lastIrRxMs = now;
  lastIrRxProtocol = typeToString(irResults.decode_type, irResults.repeat);
  lastIrRxModel = -1;
  lastIrRxAcDecoded = false;
  lastIrRxError = irResults.overflow ? "Capture buffer overflow" : "";

  String summary = IRAcUtils::resultAcToString(&irResults);
  if (!summary.length()) summary = resultToHumanReadableBasic(&irResults);
  summary.replace("\r", " ");
  summary.replace("\n", " ");
  lastIrRxSummary = summary;

  stdAc::state_t decoded = ac.next;
  if (IRAcUtils::decodeToState(&irResults, &decoded, &ac.next)) {
    const String protocolName = typeToString(decoded.protocol);
    const AcCapabilities capabilities = capabilitiesForProtocol(protocolName);
    lastIrAir = airFromStdState(decoded, capabilities);
    lastIrRxProtocol = protocolName;
    lastIrRxModel = decoded.model;
    lastIrRxAcDecoded = true;
  }

  if (!settingsMode) {
    if (lastIrRxAcDecoded) {
      noteActivity();
      showLcdNotice("IR AC RX", LcdNoticeKind::Success);
    } else if (irResults.overflow) {
      noteActivity();
      showLcdNotice("IR OVERFLOW", LcdNoticeKind::Warning);
    }
  }
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  if (wifiSsid.length()) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
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
  server.on("/api/protocols", HTTP_GET, handleProtocols);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/power", HTTP_GET, handlePower);
  server.on("/api/temp", HTTP_GET, handleTemp);
  server.on("/api/mode", HTTP_GET, handleMode);
  server.on("/api/fan", HTTP_GET, handleFan);
  server.on("/api/swing", HTTP_GET, handleSwing);
  server.on("/api/light", HTTP_GET, handleLight);
  server.on("/api/preset", HTTP_GET, handlePreset);
  server.on("/api/preset/run", HTTP_GET, handlePresetRun);
  server.on("/api/preset/cancel", HTTP_GET, handlePresetCancel);
  server.on("/api/send", HTTP_GET, handleSend);
  server.on("/api/ir", HTTP_GET, handleIrStatus);
  server.on("/api/ir/apply", HTTP_GET, handleIrApply);
  server.on("/api/config", HTTP_GET, handleConfig);
  server.on("/api/reload-config", HTTP_GET, handleReloadConfig);
  server.on("/api/help", HTTP_GET, handleHelp);
  server.onNotFound(handleNotFound);
  server.begin();
}

void setup() {
  pinMode(PIN_LCD_BL, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_IR_IN, INPUT);
  button1.setDebounceMs(BUTTON_DEBOUNCE_MS);
  button2.setDebounceMs(BUTTON_DEBOUNCE_MS);
  button1.setPressMs(BUTTON_LONG_PRESS_MS);
  button2.setPressMs(BUTTON_LONG_PRESS_MS);
  button1.attachClick(handleButton1Click);
  button1.attachLongPressStart(handleButton1LongPress);
  button2.attachClick(handleButton2Click);
  button2.attachLongPressStart(handleButton2LongPress);
  digitalWrite(PIN_LED, LOW);
  setBacklight(true);
  lastWebActivityMs = millis();

  Serial.begin(115200);
  delay(100);

  loadConfigFile();

  u8g2.begin();
  u8g2.setContrast(20);
  u8g2.setDisplayRotation(U8G2_R2);
  drawDisplay();

  irrecv.enableIRIn();
  setupWiFi();
  setupWeb();
  drawDisplay();
}

void loop() {
  server.handleClient();
  button1.tick();
  button2.tick();
  pollIrReceiver();
  refreshBacklight();
  flushConfigSaveIfDue();
  processPresetSchedule();

  if (displayDirty || millis() - lastDisplayRefreshMs > DISPLAY_REFRESH_MS) {
    lastDisplayRefreshMs = millis();
    drawDisplay();
  }
}
