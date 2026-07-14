#include <Arduino.h>
#include <ArduinoJson.h>
#include "Checksum.h"
#include <LittleFS.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <OneButton.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

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

#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
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
constexpr uint8_t LCD_BACKLIGHT_PWM_BITS = 8;
constexpr uint16_t LCD_BACKLIGHT_PWM_HZ = 5000;
constexpr uint16_t LCD_BACKLIGHT_PWM_MAX = (1U << LCD_BACKLIGHT_PWM_BITS) - 1;
constexpr uint8_t LCD_BACKLIGHT_MIN_BRIGHTNESS = 1;
constexpr uint8_t LCD_BACKLIGHT_MAX_BRIGHTNESS = 100;
constexpr uint8_t DEFAULT_LCD_BACKLIGHT_BRIGHTNESS = 30;
constexpr uint32_t BACKLIGHT_IDLE_MS = 15UL * 1000UL;
constexpr uint32_t DISPLAY_REFRESH_MS = 1000UL;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 40UL;
constexpr uint32_t BUTTON_LONG_PRESS_MS = 800UL;
constexpr uint32_t CONFIG_SAVE_DELAY_MS = 5UL * 1000UL;
constexpr uint16_t LCD_NOTICE_MS = 2200;
constexpr uint16_t LCD_NOTICE_SHORT_MS = 1400;
constexpr uint8_t DEFAULT_WEEKDAY_SLEEP_HOUR = 7;
constexpr uint8_t DEFAULT_WEEKDAY_SLEEP_MINUTE = 50;
constexpr uint8_t DEFAULT_WEEKEND_SLEEP_HOUR = 8;
constexpr uint8_t DEFAULT_WEEKEND_SLEEP_MINUTE = 0;
constexpr uint8_t DEFAULT_SLEEP_START_TEMP = 25;
constexpr uint8_t DEFAULT_SLEEP_TARGET_TEMP = 27;
constexpr uint8_t TEMP_MIN_C = 18;
constexpr uint8_t TEMP_MAX_C = 35;
constexpr uint16_t IR_CAPTURE_BUFFER_SIZE = 1024;
constexpr uint8_t IR_CAPTURE_TIMEOUT_MS = 50;
constexpr uint8_t IR_RX_EVENT_QUEUE_SIZE = 8;
constexpr size_t IR_RX_SUMMARY_MAX_LENGTH = 320;
constexpr size_t ESPNOW_MAX_PAYLOAD_SIZE = 250;
constexpr uint8_t ESPNOW_DEBUG_MSG_COUNT = 3;
constexpr const char *CONFIG_FILE = "/config.json";
constexpr const char *DEFAULT_DEVICE_ID = "IRStation";

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

struct IrRxEvent {
  uint32_t sequence = 0;
  uint32_t receivedMs = 0;
  String protocol;
  String summary;
  String error;
  AirState state;
  int16_t model = -1;
  bool acDecoded = false;
  bool noise = false;
};

struct AcCapabilities {
  const char *protocol;
  uint8_t tempMin;
  uint8_t tempMax;
  uint8_t manualFanMax;
  bool supportsSwing;
  bool supportsLight;
};

struct AcModelOption {
  decode_type_t protocol;
  int16_t value;
  const char *name;
  bool isDefault;
};

enum class SleepPresetKind : uint8_t {
  None,
  Weekday,
  Weekend,
};

enum class SleepPresetMode : uint8_t {
  Cool,
  Heat,
};

enum class SleepPresetAction : uint8_t {
  Off,
  Temp,
};

struct SleepPresetProfile {
  uint8_t startTemp = DEFAULT_SLEEP_START_TEMP;
  uint8_t actionHour = DEFAULT_WEEKDAY_SLEEP_HOUR;
  uint8_t actionMinute = DEFAULT_WEEKDAY_SLEEP_MINUTE;
  uint8_t targetTemp = DEFAULT_SLEEP_TARGET_TEMP;
};

AirState air;
AirState editAir;
IrRxEvent irRxEvents[IR_RX_EVENT_QUEUE_SIZE];
IrRxEvent lastValidIrRx;
OneButton button1(PIN_BTN1, true, true);
OneButton button2(PIN_BTN2, true, true);

enum class SettingItem : uint8_t {
  Mode,
  Temp,
  Fan,
  Swing,
  Light,
  Backlight,
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

constexpr AcModelOption AC_MODEL_OPTIONS[] = {
    {decode_type_t::ARGO, 1, "WREM2", true},
    {decode_type_t::ARGO, 2, "WREM3", false},
    {decode_type_t::FUJITSU_AC, 1, "ARRAH2E", true},
    {decode_type_t::FUJITSU_AC, 2, "ARDB1", false},
    {decode_type_t::FUJITSU_AC, 3, "ARREB1E", false},
    {decode_type_t::FUJITSU_AC, 4, "ARJW2", false},
    {decode_type_t::FUJITSU_AC, 5, "ARRY4", false},
    {decode_type_t::FUJITSU_AC, 6, "ARREW4E", false},
    {decode_type_t::GREE, 1, "YAW1F", true},
    {decode_type_t::GREE, 2, "YBOFB", false},
    {decode_type_t::GREE, 3, "YX1FSF", false},
    {decode_type_t::HAIER_AC176, 1, "V9014557-A", true},
    {decode_type_t::HAIER_AC176, 2, "V9014557-B", false},
    {decode_type_t::HITACHI_AC1, 1, "R-LT0541-HTA-A", true},
    {decode_type_t::HITACHI_AC1, 2, "R-LT0541-HTA-B", false},
    {decode_type_t::LG, 1, "GE6711AR2853M", true},
    {decode_type_t::LG, 5, "LG6711A20083V", false},
    {decode_type_t::LG2, 2, "AKB75215403", true},
    {decode_type_t::LG2, 3, "AKB74955603", false},
    {decode_type_t::LG2, 4, "AKB73757604", false},
    {decode_type_t::MIRAGE, 1, "KKG9A-C1", true},
    {decode_type_t::MIRAGE, 2, "KKG29A-C1", false},
    {decode_type_t::PANASONIC_AC, 1, "LKE", false},
    {decode_type_t::PANASONIC_AC, 2, "NKE", false},
    {decode_type_t::PANASONIC_AC, 3, "DKE/PKR", false},
    {decode_type_t::PANASONIC_AC, 4, "JKE", true},
    {decode_type_t::PANASONIC_AC, 5, "CKP", false},
    {decode_type_t::PANASONIC_AC, 6, "RKR", false},
    {decode_type_t::SHARP_AC, 1, "A907", true},
    {decode_type_t::SHARP_AC, 2, "A705", false},
    {decode_type_t::SHARP_AC, 3, "A903/820", false},
    {decode_type_t::TCL112AC, 1, "TAC09CHSD", true},
    {decode_type_t::TCL112AC, 2, "GZ055BE1", false},
    {decode_type_t::TEKNOPOINT, 2, "GZ055BE1", true},
    {decode_type_t::VOLTAS, 0, "Full Function", false},
    {decode_type_t::VOLTAS, 1, "122LZF", true},
    {decode_type_t::WHIRLPOOL_AC, 1, "DG11J13A/DG11J1-04", true},
    {decode_type_t::WHIRLPOOL_AC, 2, "DG11J191", false},
};
constexpr uint8_t AC_MODEL_OPTION_COUNT = sizeof(AC_MODEL_OPTIONS) / sizeof(AC_MODEL_OPTIONS[0]);

String acProtocol = DEFAULT_AC_PROTOCOL;
int16_t acModel = DEFAULT_AC_MODEL;
String deviceId = DEFAULT_DEVICE_ID;
String wifiSsid = WIFI_SSID;
String wifiPassword = WIFI_PASSWORD;
String lastIrError;
String lastEspNowUid;
String lastEspNowCmd;
String lastEspNowSender;
String lastEspNowError;
String espNowDebugMessages[ESPNOW_DEBUG_MSG_COUNT];
String lcdNoticeText;
String configError;
uint32_t lastIrSendMs = 0;
uint32_t lastEspNowRxMs = 0;
uint32_t lastEspNowIgnoredMs = 0;
uint32_t lastWebActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t lcdNoticeUntilMs = 0;
bool backlightOn = false;
bool backlightPwmReady = false;
bool apMode = false;
bool fileSystemReady = false;
bool configLoaded = false;
bool wifiRestartRequired = false;
bool configSavePending = false;
bool settingsMode = false;
bool hasLastValidIrRx = false;
bool displayDirty = true;
bool presetScheduleActive = false;
bool espNowReady = false;
uint8_t settingsIndex = 0;
uint8_t backlightBrightness = DEFAULT_LCD_BACKLIGHT_BRIGHTNESS;
uint8_t editBacklightBrightness = DEFAULT_LCD_BACKLIGHT_BRIGHTNESS;
uint8_t espNowDebugNextIndex = 0;
uint8_t espNowDebugCount = 0;
uint8_t irRxEventNextIndex = 0;
uint8_t irRxEventCount = 0;
uint32_t irRxNextSequence = 1;
uint32_t configSaveDueMs = 0;
time_t presetActionEpoch = 0;
LcdNoticeKind lcdNoticeKind = LcdNoticeKind::Info;
SleepPresetProfile weekdayCoolPreset = {DEFAULT_SLEEP_START_TEMP, DEFAULT_WEEKDAY_SLEEP_HOUR, DEFAULT_WEEKDAY_SLEEP_MINUTE, DEFAULT_SLEEP_TARGET_TEMP};
SleepPresetProfile weekdayHeatPreset = {DEFAULT_SLEEP_START_TEMP, DEFAULT_WEEKDAY_SLEEP_HOUR, DEFAULT_WEEKDAY_SLEEP_MINUTE, DEFAULT_SLEEP_TARGET_TEMP};
SleepPresetProfile weekendCoolPreset = {DEFAULT_SLEEP_START_TEMP, DEFAULT_WEEKEND_SLEEP_HOUR, DEFAULT_WEEKEND_SLEEP_MINUTE, DEFAULT_SLEEP_TARGET_TEMP};
SleepPresetProfile weekendHeatPreset = {DEFAULT_SLEEP_START_TEMP, DEFAULT_WEEKEND_SLEEP_HOUR, DEFAULT_WEEKEND_SLEEP_MINUTE, DEFAULT_SLEEP_TARGET_TEMP};
SleepPresetKind activePresetKind = SleepPresetKind::None;
SleepPresetMode activePresetMode = SleepPresetMode::Cool;
SleepPresetAction activePresetAction = SleepPresetAction::Off;
uint8_t activePresetTargetTemp = DEFAULT_SLEEP_TARGET_TEMP;
portMUX_TYPE espNowMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool espNowPayloadPending = false;
volatile size_t espNowPayloadLen = 0;
char espNowPayload[ESPNOW_MAX_PAYLOAD_SIZE + 1] = {0};

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
.tabs{display:grid;grid-template-columns:1fr 1fr;gap:6px;background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:5px;margin-bottom:14px}.tabs button{border:0;min-height:40px;color:var(--muted)}.tabs button.active{color:white}.page{display:none}.page.active{display:block}.pageHead{display:flex;align-items:end;justify-content:space-between;gap:12px;margin:2px 2px 12px}.pageTitle{font-size:20px;font-weight:800}.pageHint{color:var(--muted);font-size:13px;text-align:right}
.status{display:grid;grid-template-columns:1fr auto;gap:14px;align-items:center;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px;margin-bottom:14px}
.temp{font-size:70px;font-weight:800;letter-spacing:0;line-height:.92}.temp small{font-size:28px}.meta{color:var(--muted);font-size:15px;margin-top:8px}
.power{width:92px;height:92px;border:0;border-radius:50%;background:var(--accent);color:white;font-size:38px;box-shadow:0 8px 24px #0002}.power.off{background:#6b7280}
.grid,.advancedGrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px}.wide{grid-column:1/-1}
.label{font-size:13px;color:var(--muted);margin-bottom:9px}.seg{display:grid;grid-template-columns:repeat(auto-fit,minmax(70px,1fr));gap:7px}
button{min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;font-weight:650}button.active{background:var(--accent);border-color:var(--accent);color:white}
select{width:100%;min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;font-weight:650;padding:0 10px}
input{width:100%;min-height:44px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--ink);font:inherit;padding:0 10px}
.step{display:grid;grid-template-columns:54px 1fr 54px;gap:8px;align-items:center}.step button{font-size:26px}.value{text-align:center;font-size:32px;font-weight:800}
.form{display:grid;grid-template-columns:1fr 1fr auto;gap:8px}.presetForm{display:grid;grid-template-columns:1fr auto auto auto;gap:8px;align-items:center}.presetConfig{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:10px}.presetProfile{border-top:1px solid var(--line);padding-top:8px}.presetProfile .inputs{display:grid;grid-template-columns:1fr 1fr;gap:7px}.presetProfile.weekend .inputs{grid-template-columns:1fr 1fr 1fr}.muted{color:var(--muted);font-size:13px;margin-top:8px}
.debugMsgs{margin:0;white-space:pre-wrap;word-break:break-word;color:var(--muted);font:12px/1.45 ui-monospace,SFMono-Regular,Consolas,monospace}
.foot{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px;color:var(--muted);font-size:13px}.foot span{border:1px solid var(--line);border-radius:999px;padding:5px 9px}
@media(pointer:coarse){#backlightSlider{display:none}}
@media(max-width:520px){main{padding:12px}.grid,.advancedGrid{grid-template-columns:1fr}.wide{grid-column:auto}.form,.presetForm,.presetConfig{grid-template-columns:1fr}.temp{font-size:58px}.power{width:78px;height:78px}.status{padding:14px}.pageHead{display:block}.pageHint{text-align:left;margin-top:3px}}
</style>
</head>
<body>
<main>
  <div class="top"><div class="brand">IRStation</div><div class="chip" id="net">Connecting</div></div>
  <nav class="tabs" aria-label="Page navigation">
    <button class="active" data-page="daily" aria-controls="dailyPage" aria-selected="true">Daily</button>
    <button data-page="advanced" aria-controls="advancedPage" aria-selected="false">Advanced</button>
  </nav>

  <section class="page active" id="dailyPage" data-view="daily">
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
      <div class="panel" id="lightPanel"><div class="label">Indoor display light</div><div class="seg" id="lights"><button data-v="off">Off</button><button data-v="on">On</button></div></div>
    </section>
    <div class="panel" id="presetPanel" style="margin-top:12px"><div class="label">Sleep preset</div><div class="seg"><button id="runWeekday">Workday</button><button id="runWeekend">Weekend</button><button id="cancelPreset">Cancel</button></div><div class="muted" id="presetNote"></div><div class="presetConfig">
      <div class="presetProfile"><div class="label">Workday Cool</div><div class="inputs"><input id="wdCoolTemp" type="number" min="16" max="30" placeholder="Temp"><input id="wdCoolTime" type="time"></div></div>
      <div class="presetProfile"><div class="label">Workday Heat</div><div class="inputs"><input id="wdHeatTemp" type="number" min="16" max="30" placeholder="Temp"><input id="wdHeatTime" type="time"></div></div>
      <div class="presetProfile weekend"><div class="label">Weekend Cool</div><div class="inputs"><input id="weCoolTemp" type="number" min="16" max="30" placeholder="Start"><input id="weCoolTime" type="time"><input id="weCoolTarget" type="number" min="16" max="30" placeholder="Target"></div></div>
      <div class="presetProfile weekend"><div class="label">Weekend Heat</div><div class="inputs"><input id="weHeatTemp" type="number" min="16" max="30" placeholder="Start"><input id="weHeatTime" type="time"><input id="weHeatTarget" type="number" min="16" max="30" placeholder="Target"></div></div>
    </div><button id="savePreset" style="margin-top:10px;width:100%">Save sleep settings</button></div>
  </section>

  <section class="page" id="advancedPage" data-view="advanced">
    <div class="pageHead"><div class="pageTitle">Advanced</div><div class="pageHint">Device, infrared and diagnostics</div></div>
    <div class="advancedGrid">
      <div class="panel wide" id="protocolPanel"><div class="label">AC brand</div><select id="protocolSelect"></select></div>
      <div class="panel" id="modelPanel" style="display:none"><div class="label">Remote profile</div><select id="modelSelect"></select><div class="muted">Remote model, not the indoor-unit product model.</div></div>
      <div class="panel wide" id="backlightPanel"><div class="label">LCD backlight</div><div class="step"><button id="backlightDown">-</button><div class="value" id="backlightValue">--%</div><button id="backlightUp">+</button></div><input id="backlightSlider" type="range" min="1" max="100" step="1" style="margin-top:8px"><div class="seg" id="backlightQuick" style="margin-top:8px"><button data-v="10">10%</button><button data-v="30">30%</button><button data-v="60">60%</button><button data-v="100">100%</button></div></div>
      <div class="panel wide" id="irPanel"><div class="label">IR receiver</div><div class="foot"><span id="irrx">No signal</span></div><button id="applyIr" style="margin-top:8px">Apply learned state</button></div>
      <div class="panel wide" id="wifiPanel"><div class="label">Wi-Fi setup</div><div class="form"><input id="ssid" placeholder="SSID"><input id="password" type="password" placeholder="Password"><button id="saveWifi">Save</button></div><div class="muted" id="wifiNote"></div></div>
      <div class="panel wide" id="debugPanel"><div class="label">Debug Msg</div><pre class="debugMsgs" id="debugMsg">No ESP-NOW messages</pre></div>
    </div>
  </section>

  <div class="foot"><span id="proto">Protocol --</span><span id="ip">IP --</span><span id="channel">CH --</span><span id="rssi">RSSI --</span></div>
</main>
<script>
const labels={mode:{auto:'Auto',cool:'Cool',heat:'Heat',dry:'Dry',fan:'Fan'},fan:{auto:'Auto','1':'1','2':'2','3':'3','4':'4','5':'5'},swing:{true:'On',false:'Off'}};
let state=null;
let protocolModels={};
const PAGE_STORAGE_KEY='irstation.activePage';
function savedPage(){
 try{const page=localStorage.getItem(PAGE_STORAGE_KEY); return page==='advanced'?'advanced':'daily';}catch(e){return 'daily';}
}
function selectPage(page,persist=true){
 const selected=page==='advanced'?'advanced':'daily';
 document.querySelectorAll('[data-view]').forEach(el=>{const active=el.dataset.view===selected; el.classList.toggle('active',active); el.setAttribute('aria-hidden',active?'false':'true');});
 document.querySelectorAll('[data-page]').forEach(el=>{const active=el.dataset.page===selected; el.classList.toggle('active',active); el.setAttribute('aria-selected',active?'true':'false');});
 if(persist){try{localStorage.setItem(PAGE_STORAGE_KEY,selected);}catch(e){} window.scrollTo(0,0);}
}
async function api(path){const r=await fetch(path,{cache:'no-store'}); if(!r.ok)throw new Error(await r.text()); return r.json();}
function mark(group,value){document.querySelectorAll(group+' button').forEach(b=>b.classList.toggle('active',b.dataset.v==value));}
async function loadProtocols(){
 const s=await api('/api/protocols'); protocolModels=s.models||{};
 const el=document.getElementById('protocolSelect'); el.innerHTML='';
 s.protocols.forEach(p=>{const o=document.createElement('option'); o.value=p; o.textContent=p; el.appendChild(o);});
}
function renderModelSelect(protocol,model){
 const options=protocolModels[protocol]||[]; const panel=document.getElementById('modelPanel'); const el=document.getElementById('modelSelect');
 panel.style.display=options.length>1?'block':'none';
 document.getElementById('protocolPanel').classList.toggle('wide',options.length<=1);
 if(el.dataset.protocol!==protocol){
  el.dataset.protocol=protocol; el.innerHTML='';
  options.forEach(m=>{const o=document.createElement('option'); o.value=String(m.value); o.textContent=m.name+' ('+m.value+')'; el.appendChild(o);});
 }
 if(options.length)el.value=String(model);
}
function renderCaps(s){
 const c=s.capabilities||{fanMax:5,displayLight:true}; document.querySelectorAll('#fans button').forEach(b=>{const v=b.dataset.v; b.style.display=(v=='auto'||Number(v)<=c.fanMax)?'':'none';});
 document.getElementById('lightPanel').style.display=c.displayLight?'block':'none';
}
function clampBacklight(v){return Math.max(1,Math.min(100,Number(v)||30));}
function updateBacklightUi(v){
 const b=clampBacklight(v); if(state)state.backlightBrightness=b;
 document.getElementById('backlightValue').textContent=b+'%';
 const slider=document.getElementById('backlightSlider'); slider.value=b;
 document.querySelectorAll('#backlightQuick button').forEach(btn=>btn.classList.toggle('active',Number(btn.dataset.v)==b));
 return b;
}
function setValue(id,v){const el=document.getElementById(id); if(el&&document.activeElement!==el)el.value=v==null?'':v;}
function profileTime(p){return p&&p.time?p.time:'07:50';}
function fillPresetProfile(prefix,p,withTarget){
 setValue(prefix+'Temp',p&&p.startTemp); setValue(prefix+'Time',profileTime(p));
 if(withTarget)setValue(prefix+'Target',p&&p.targetTemp);
}
function presetLabel(p){
 if(!p||!p.active)return 'Tap Workday or Weekend; mode follows current Cool/Heat.';
 const kind=p.kind=='weekend'?'Weekend':'Workday';
 const mode=(p.mode||'').toUpperCase();
 const action=p.action=='temp'?('-> '+p.targetTemp+'C'):'OFF';
 return kind+' '+mode+' '+(p.time||'--:--')+' '+action;
}
function renderDebugMsg(s){
 const msgs=(s.espnow&&s.espnow.debugMsg)||[];
 document.getElementById('debugMsg').textContent=msgs.length?msgs.join('\n'):'No ESP-NOW messages';
}
function render(s){
 state=s.state; state.backlightBrightness=s.device.backlightBrightness||30; document.getElementById('temp').textContent=state.temp; document.getElementById('temp2').textContent=state.temp+'C';
 document.getElementById('mode').textContent=labels.mode[state.mode]||state.mode; document.getElementById('fan').textContent=labels.fan[state.fan]||state.fan;
 document.getElementById('swing').textContent=state.swing?'On':'Off'; document.getElementById('power').classList.toggle('off',!state.power);
 document.getElementById('net').textContent=s.device.wifi+' / '+s.device.ip; document.getElementById('ip').textContent='IP '+s.device.ip;
 document.getElementById('rssi').textContent='RSSI '+s.device.rssi; document.getElementById('channel').textContent='CH '+((s.espnow&&s.espnow.channel)||'--'); document.getElementById('proto').textContent='Protocol '+s.ir.protocol+'/'+s.ir.model;
 const protocolSelect=document.getElementById('protocolSelect'); if(protocolSelect.options.length) protocolSelect.value=s.ir.protocol;
 renderModelSelect(s.ir.protocol,s.ir.model);
 updateBacklightUi(state.backlightBrightness);
 renderCaps(s);
 mark('#modes',state.mode); mark('#fans',state.fan); mark('#swings',state.swing?'on':'off'); mark('#lights',state.displayLight?'on':'off');
 const preset=s.preset||{}; const profiles=preset.profiles||{}; const weekday=profiles.weekday||{}; const weekend=profiles.weekend||{};
 fillPresetProfile('wdCool',weekday.cool,false); fillPresetProfile('wdHeat',weekday.heat,false); fillPresetProfile('weCool',weekend.cool,true); fillPresetProfile('weHeat',weekend.heat,true);
 document.getElementById('presetNote').textContent=presetLabel(preset); document.getElementById('cancelPreset').disabled=!preset.active;
 const rx=s.ir.rx||{}; document.getElementById('irrx').textContent=rx.lastMs?(rx.protocol+' '+(rx.acDecoded?'AC':'raw')+' '+(rx.summary||'')):'No signal'; document.getElementById('applyIr').disabled=!rx.acDecoded;
 document.getElementById('wifiPanel').style.display=(s.device.wifi=='ap'||s.config.wifiRestartRequired)?'block':'none'; document.getElementById('wifiNote').textContent=s.config.wifiRestartRequired?'Saved. Wait 10s, then reboot to use the new Wi-Fi.':'AP mode setup';
 renderDebugMsg(s);
}
async function refresh(){try{render(await api('/api/state'))}catch(e){document.getElementById('net').textContent='Offline'}}
async function control(q){render(await api('/api/control?'+q));}
function setBacklightBrightness(v){const b=updateBacklightUi(v); api('/api/config?brightness='+b).then(render);}
function presetConfigQuery(){
 const ids=['wdCoolTemp','wdCoolTime','wdHeatTemp','wdHeatTime','weCoolTemp','weCoolTime','weCoolTarget','weHeatTemp','weHeatTime','weHeatTarget'];
 return ids.map(id=>id+'='+encodeURIComponent(document.getElementById(id).value)).join('&');
}
document.getElementById('power').onclick=()=>api('/api/power?value=toggle').then(render);
document.getElementById('up').onclick=()=>api('/api/temp?delta=1').then(render);
document.getElementById('down').onclick=()=>api('/api/temp?delta=-1').then(render);
document.querySelectorAll('#modes button').forEach(b=>b.onclick=()=>control('mode='+b.dataset.v));
document.querySelectorAll('#fans button').forEach(b=>b.onclick=()=>control('fan='+b.dataset.v));
document.querySelectorAll('#swings button').forEach(b=>b.onclick=()=>control('swing='+b.dataset.v));
document.querySelectorAll('#lights button').forEach(b=>b.onclick=()=>control('light='+b.dataset.v));
document.getElementById('backlightDown').onclick=()=>setBacklightBrightness((state&&state.backlightBrightness||30)-5);
document.getElementById('backlightUp').onclick=()=>setBacklightBrightness((state&&state.backlightBrightness||30)+5);
document.getElementById('backlightSlider').oninput=e=>updateBacklightUi(e.target.value);
document.getElementById('backlightSlider').onchange=e=>setBacklightBrightness(e.target.value);
document.querySelectorAll('#backlightQuick button').forEach(b=>b.onclick=()=>setBacklightBrightness(b.dataset.v));
document.getElementById('protocolSelect').onchange=e=>api('/api/config?protocol='+encodeURIComponent(e.target.value)).then(render);
document.getElementById('modelSelect').onchange=e=>api('/api/config?model='+encodeURIComponent(e.target.value)).then(render);
document.getElementById('applyIr').onclick=()=>api('/api/ir/apply').then(render);
document.getElementById('runWeekday').onclick=()=>api('/api/preset/run?kind=weekday').then(render);
document.getElementById('runWeekend').onclick=()=>api('/api/preset/run?kind=weekend').then(render);
document.getElementById('savePreset').onclick=()=>api('/api/preset?'+presetConfigQuery()).then(render);
document.getElementById('cancelPreset').onclick=()=>api('/api/preset/cancel').then(render);
document.getElementById('saveWifi').onclick=()=>api('/api/config?ssid='+encodeURIComponent(document.getElementById('ssid').value)+'&password='+encodeURIComponent(document.getElementById('password').value)).then(render);
document.querySelectorAll('[data-page]').forEach(el=>el.onclick=()=>selectPage(el.dataset.page));
selectPage(savedPage(),false);
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

void enqueueIrRxEvent(const IrRxEvent &event) {
  irRxEvents[irRxEventNextIndex] = event;
  irRxEventNextIndex = (irRxEventNextIndex + 1) % IR_RX_EVENT_QUEUE_SIZE;
  if (irRxEventCount < IR_RX_EVENT_QUEUE_SIZE) irRxEventCount++;
}

uint8_t irRxEventIndexNewestFirst(uint8_t offset) {
  return (irRxEventNextIndex + IR_RX_EVENT_QUEUE_SIZE - 1 - offset) % IR_RX_EVENT_QUEUE_SIZE;
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

uint8_t normalizedBacklightBrightness(uint8_t value) {
  return constrain(value, LCD_BACKLIGHT_MIN_BRIGHTNESS, LCD_BACKLIGHT_MAX_BRIGHTNESS);
}

uint16_t backlightDuty(uint8_t brightness, bool on) {
  if (!on) return LCD_BACKLIGHT_ACTIVE_LOW ? LCD_BACKLIGHT_PWM_MAX : 0;
  const uint16_t onDuty = (static_cast<uint32_t>(normalizedBacklightBrightness(brightness)) * LCD_BACKLIGHT_PWM_MAX + 50) / 100;
  return LCD_BACKLIGHT_ACTIVE_LOW ? LCD_BACKLIGHT_PWM_MAX - onDuty : onDuty;
}

void writeBacklightOutput(uint8_t brightness, bool on) {
  const uint16_t duty = backlightDuty(brightness, on);
  if (backlightPwmReady) {
    ledcWrite(PIN_LCD_BL, duty);
  } else {
    const uint8_t onLevel = LCD_BACKLIGHT_ACTIVE_LOW ? LOW : HIGH;
    digitalWrite(PIN_LCD_BL, on && brightness ? onLevel : !onLevel);
  }
}

void applyBacklightOutput() {
  writeBacklightOutput(backlightBrightness, backlightOn);
}

void setBacklight(bool on) {
  backlightOn = on;
  applyBacklightOutput();
}

void previewBacklightBrightness(uint8_t brightness) {
  writeBacklightOutput(normalizedBacklightBrightness(brightness), true);
}

void setBacklightBrightness(uint8_t brightness) {
  backlightBrightness = normalizedBacklightBrightness(brightness);
  applyBacklightOutput();
  displayDirty = true;
}

bool parseBacklightBrightness(String value, uint8_t &brightness) {
  value.trim();
  if (!value.length()) return false;
  for (uint8_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) return false;
  }
  const int parsed = value.toInt();
  if (parsed < LCD_BACKLIGHT_MIN_BRIGHTNESS || parsed > LCD_BACKLIGHT_MAX_BRIGHTNESS) return false;
  brightness = static_cast<uint8_t>(parsed);
  return true;
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

String presetTimeString(const SleepPresetProfile &profile) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02u:%02u", profile.actionHour, profile.actionMinute);
  return String(buf);
}

bool sleepPresetModeFromAir(SleepPresetMode &mode) {
  if (air.mode == "cool") {
    mode = SleepPresetMode::Cool;
    return true;
  }
  if (air.mode == "heat") {
    mode = SleepPresetMode::Heat;
    return true;
  }
  return false;
}

SleepPresetProfile &sleepPresetProfile(SleepPresetKind kind, SleepPresetMode mode) {
  if (kind == SleepPresetKind::Weekend) return mode == SleepPresetMode::Heat ? weekendHeatPreset : weekendCoolPreset;
  return mode == SleepPresetMode::Heat ? weekdayHeatPreset : weekdayCoolPreset;
}

SleepPresetAction sleepPresetAction(SleepPresetKind kind) {
  return kind == SleepPresetKind::Weekend ? SleepPresetAction::Temp : SleepPresetAction::Off;
}

const char *sleepPresetKindName(SleepPresetKind kind) {
  if (kind == SleepPresetKind::Weekday) return "weekday";
  if (kind == SleepPresetKind::Weekend) return "weekend";
  return "none";
}

const char *sleepPresetModeName(SleepPresetMode mode) {
  return mode == SleepPresetMode::Heat ? "heat" : "cool";
}

const char *sleepPresetActionName(SleepPresetAction action) {
  return action == SleepPresetAction::Temp ? "temp" : "off";
}

const char *sleepPresetLcdLabel() {
  if (activePresetKind == SleepPresetKind::Weekday) return activePresetMode == SleepPresetMode::Heat ? "WD HEAT" : "WD COOL";
  if (activePresetKind == SleepPresetKind::Weekend) return activePresetMode == SleepPresetMode::Heat ? "WE HEAT" : "WE COOL";
  return "PRESET";
}

const char *sleepPresetLcdShortLabel() {
  if (activePresetKind == SleepPresetKind::Weekday) return activePresetMode == SleepPresetMode::Heat ? "WDH" : "WDC";
  if (activePresetKind == SleepPresetKind::Weekend) return activePresetMode == SleepPresetMode::Heat ? "WEH" : "WEC";
  return "PRE";
}

time_t nextPresetEpoch(const SleepPresetProfile &profile) {
  const time_t now = time(nullptr);
  if (now <= 100000) return 0;

  struct tm target;
  localtime_r(&now, &target);
  target.tm_hour = profile.actionHour;
  target.tm_min = profile.actionMinute;
  target.tm_sec = 0;
  time_t epoch = mktime(&target);
  if (epoch <= now) epoch += 24L * 60L * 60L;
  return epoch;
}

void presetTimeLabel(char *buf, size_t len) {
  if (!presetScheduleActive) {
    snprintf(buf, len, "--:--");
    return;
  }
  struct tm target;
  localtime_r(&presetActionEpoch, &target);
  snprintf(buf, len, "%02u:%02u", target.tm_hour, target.tm_min);
}

void cancelPresetSchedule(bool showNotice = false) {
  if (!presetScheduleActive) return;
  presetScheduleActive = false;
  presetActionEpoch = 0;
  activePresetKind = SleepPresetKind::None;
  activePresetAction = SleepPresetAction::Off;
  activePresetTargetTemp = DEFAULT_SLEEP_TARGET_TEMP;
  displayDirty = true;
  if (showNotice) showLcdNotice("PRESET CANCEL", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
}

bool normalizeSleepPresetProfile(SleepPresetProfile &profile, const AcCapabilities &capabilities) {
  bool changed = false;
  const uint8_t startTemp = constrain(profile.startTemp, capabilities.tempMin, capabilities.tempMax);
  if (startTemp != profile.startTemp) {
    profile.startTemp = startTemp;
    changed = true;
  }
  const uint8_t targetTemp = constrain(profile.targetTemp, capabilities.tempMin, capabilities.tempMax);
  if (targetTemp != profile.targetTemp) {
    profile.targetTemp = targetTemp;
    changed = true;
  }
  if (profile.actionHour > 23) {
    profile.actionHour = DEFAULT_WEEKDAY_SLEEP_HOUR;
    changed = true;
  }
  if (profile.actionMinute > 59) {
    profile.actionMinute = DEFAULT_WEEKDAY_SLEEP_MINUTE;
    changed = true;
  }
  return changed;
}

bool validProtocol(const String &protocolName) {
  String proto = protocolName;
  proto.toUpperCase();
  const decode_type_t protocol = strToDecodeType(proto.c_str());
  return protocol != decode_type_t::UNKNOWN && IRac::isProtocolSupported(protocol);
}

uint8_t modelOptionCount(decode_type_t protocol) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < AC_MODEL_OPTION_COUNT; i++) {
    if (AC_MODEL_OPTIONS[i].protocol == protocol) count++;
  }
  return count;
}

int16_t defaultModelForProtocol(decode_type_t protocol) {
  const AcModelOption *first = nullptr;
  for (uint8_t i = 0; i < AC_MODEL_OPTION_COUNT; i++) {
    const AcModelOption &option = AC_MODEL_OPTIONS[i];
    if (option.protocol != protocol) continue;
    if (!first) first = &option;
    if (option.isDefault) return option.value;
  }
  return first ? first->value : DEFAULT_AC_MODEL;
}

bool validModelForProtocol(decode_type_t protocol, int16_t model) {
  bool hasOptions = false;
  for (uint8_t i = 0; i < AC_MODEL_OPTION_COUNT; i++) {
    const AcModelOption &option = AC_MODEL_OPTIONS[i];
    if (option.protocol != protocol) continue;
    hasOptions = true;
    if (option.value == model) return true;
  }
  return !hasOptions && model == DEFAULT_AC_MODEL;
}

bool parseModelValue(const String &text, int16_t &model) {
  if (!text.length()) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < text.length(); i++) {
    if (!isDigit(text[i])) return false;
    value = value * 10 + static_cast<uint8_t>(text[i] - '0');
    if (value > INT16_MAX) return false;
  }
  model = static_cast<int16_t>(value);
  return true;
}

bool normalizeConfig() {
  bool changed = false;

  deviceId.trim();
  if (!deviceId.length()) {
    deviceId = DEFAULT_DEVICE_ID;
    changed = true;
  }

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

  const decode_type_t configuredProtocol = strToDecodeType(acProtocol.c_str());
  if (!validModelForProtocol(configuredProtocol, acModel)) {
    acModel = defaultModelForProtocol(configuredProtocol);
    changed = true;
  }

  changed = normalizeSleepPresetProfile(weekdayCoolPreset, capabilities) || changed;
  changed = normalizeSleepPresetProfile(weekdayHeatPreset, capabilities) || changed;
  changed = normalizeSleepPresetProfile(weekendCoolPreset, capabilities) || changed;
  changed = normalizeSleepPresetProfile(weekendHeatPreset, capabilities) || changed;

  const uint8_t normalizedBrightness = normalizedBacklightBrightness(backlightBrightness);
  if (normalizedBrightness != backlightBrightness) {
    backlightBrightness = normalizedBrightness;
    changed = true;
  }

  return changed;
}

void writeSleepPresetProfile(JsonObject parent, const char *key, const SleepPresetProfile &profile, SleepPresetAction action) {
  JsonObject item = parent[key].to<JsonObject>();
  item["startTemp"] = profile.startTemp;
  item["time"] = presetTimeString(profile);
  item["action"] = sleepPresetActionName(action);
  if (action == SleepPresetAction::Temp) item["targetTemp"] = profile.targetTemp;
}

bool saveConfigFile() {
  if (!fileSystemReady) {
    configError = "LittleFS is not mounted";
    return false;
  }

  normalizeConfig();

  JsonDocument doc;
  doc["id"] = deviceId;

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

  JsonObject lcd = doc["lcd"].to<JsonObject>();
  lcd["backlightBrightness"] = backlightBrightness;

  JsonObject preset = doc["preset"].to<JsonObject>();
  JsonObject weekday = preset["weekday"].to<JsonObject>();
  writeSleepPresetProfile(weekday, "cool", weekdayCoolPreset, SleepPresetAction::Off);
  writeSleepPresetProfile(weekday, "heat", weekdayHeatPreset, SleepPresetAction::Off);
  JsonObject weekend = preset["weekend"].to<JsonObject>();
  writeSleepPresetProfile(weekend, "cool", weekendCoolPreset, SleepPresetAction::Temp);
  writeSleepPresetProfile(weekend, "heat", weekendHeatPreset, SleepPresetAction::Temp);

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

void loadSleepPresetProfile(JsonVariant value, SleepPresetProfile &profile) {
  if (value.isNull()) return;
  JsonVariant startTempValue = value["startTemp"];
  if (startTempValue.isNull()) startTempValue = value["temp"];
  if (!startTempValue.isNull()) profile.startTemp = startTempValue.as<uint8_t>();

  JsonVariant targetTempValue = value["targetTemp"];
  if (!targetTempValue.isNull()) profile.targetTemp = targetTempValue.as<uint8_t>();

  JsonVariant timeValue = value["time"];
  if (timeValue.isNull()) timeValue = value["actionTime"];
  if (!timeValue.isNull()) {
    uint8_t hour;
    uint8_t minute;
    if (parsePresetTime(timeValue.as<String>(), hour, minute)) {
      profile.actionHour = hour;
      profile.actionMinute = minute;
    }
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

  JsonVariant deviceIdValue = doc["id"];
  if (!deviceIdValue.isNull()) deviceId = deviceIdValue.as<String>();

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

  JsonVariant backlightBrightnessValue = doc["lcd"]["backlightBrightness"];
  if (backlightBrightnessValue.isNull()) backlightBrightnessValue = doc["lcd"]["brightness"];
  if (backlightBrightnessValue.isNull()) backlightBrightnessValue = doc["device"]["backlightBrightness"];
  if (backlightBrightnessValue.isNull()) backlightBrightnessValue = doc["backlightBrightness"];
  if (!backlightBrightnessValue.isNull()) {
    uint8_t parsedBrightness;
    if (parseBacklightBrightness(backlightBrightnessValue.as<String>(), parsedBrightness)) {
      setBacklightBrightness(parsedBrightness);
    }
  }

  JsonVariant protocolValue = doc["ir"]["protocol"];
  if (!protocolValue.isNull()) acProtocol = protocolValue.as<String>();
  JsonVariant modelValue = doc["ir"]["model"];
  if (!modelValue.isNull()) acModel = modelValue.as<int16_t>();

  loadSleepPresetProfile(doc["preset"]["weekday"]["cool"], weekdayCoolPreset);
  loadSleepPresetProfile(doc["preset"]["weekday"]["heat"], weekdayHeatPreset);
  loadSleepPresetProfile(doc["preset"]["weekend"]["cool"], weekendCoolPreset);
  loadSleepPresetProfile(doc["preset"]["weekend"]["heat"], weekendHeatPreset);

  JsonVariant legacyPresetTimeValue = doc["preset"]["offTime"];
  if (!legacyPresetTimeValue.isNull()) {
    uint8_t hour;
    uint8_t minute;
    if (parsePresetTime(legacyPresetTimeValue.as<String>(), hour, minute)) {
      weekdayCoolPreset.actionHour = hour;
      weekdayCoolPreset.actionMinute = minute;
      weekdayHeatPreset.actionHour = hour;
      weekdayHeatPreset.actionMinute = minute;
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
  if (!presetScheduleActive) return;
  cancelPresetSchedule(false);
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

String currentIsoTimestamp() {
  time_t now = time(nullptr);
  if (now < 100000) return "";
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmNow);
  return String(buf);
}

void recordEspNowDebugMessage(const char *payload, size_t payloadLen) {
  String timestamp = currentIsoTimestamp();
  if (!timestamp.length()) timestamp = "0000-00-00T00:00:00";

  String entry;
  entry.reserve(timestamp.length() + 1 + payloadLen);
  entry += timestamp;
  entry += ' ';
  for (size_t i = 0; i < payloadLen; i++) entry += payload[i];

  espNowDebugMessages[espNowDebugNextIndex] = entry;
  espNowDebugNextIndex = (espNowDebugNextIndex + 1) % ESPNOW_DEBUG_MSG_COUNT;
  if (espNowDebugCount < ESPNOW_DEBUG_MSG_COUNT) espNowDebugCount++;
}

uint8_t currentWiFiChannel() {
  uint8_t primary = 0;
  wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &secondary) == ESP_OK && primary) return primary;
  return ESPNOW_CHANNEL;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowReceived(const esp_now_recv_info_t *, const uint8_t *data, int len) {
#else
void onEspNowReceived(const uint8_t *, const uint8_t *data, int len) {
#endif
  if (!data || len <= 0) return;
  const size_t copyLen = min(static_cast<size_t>(len), ESPNOW_MAX_PAYLOAD_SIZE);
  portENTER_CRITICAL_ISR(&espNowMux);
  memcpy(espNowPayload, data, copyLen);
  espNowPayload[copyLen] = '\0';
  espNowPayloadLen = copyLen;
  espNowPayloadPending = true;
  portEXIT_CRITICAL_ISR(&espNowMux);
}

bool copyPendingEspNowPayload(char *payload, size_t payloadSize, size_t &payloadLen) {
  if (!payload || payloadSize == 0) return false;
  portENTER_CRITICAL(&espNowMux);
  const bool pending = espNowPayloadPending;
  if (pending) {
    const size_t pendingLen = espNowPayloadLen;
    payloadLen = min(pendingLen, payloadSize - 1);
    memcpy(payload, espNowPayload, payloadLen);
    payload[payloadLen] = '\0';
    espNowPayloadPending = false;
  }
  portEXIT_CRITICAL(&espNowMux);
  return pending;
}

bool validateEspNowChecksum(JsonDocument &doc) {
  String received = doc["chk"] | "";
  received.trim();
  received.toUpperCase();
  if (!received.length()) {
    lastEspNowError = "Missing chk";
    return false;
  }
  const String expected = commandChecksum(doc);

#if 0
  Serial.print("payload: ");
  serializeJson(doc, Serial);
  Serial.println();
  Serial.print("Received chk: ");
  Serial.println(received);
  Serial.print("Expected chk: ");
  Serial.println(expected);
#endif

  if (received != expected) {
    lastEspNowError = "Invalid chk";
    return false;
  }
  return true;
}

using EspNowCommandHandler = void (*)(const String &uid, const String &sender);

struct EspNowCommandRoute {
  const char *cmd;
  EspNowCommandHandler handler;
};

void executeEspNowPowerCommand(const String &uid, const String &sender) {
  lastEspNowUid = uid;
  lastEspNowCmd = "power";
  lastEspNowSender = sender;
  lastEspNowRxMs = millis();
  lastEspNowError = "";
  noteActivity();

  if (settingsMode) {
    lastEspNowError = "LCD settings mode active";
    showLcdNotice("ESPNOW BUSY", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
    return;
  }

  if (presetScheduleActive) exitPresetModeForPowerOverride();
  air.power = !air.power;
  if (!air.power) cancelPresetSchedule(false);
  normalizeConfig();
  saveState();
  if (!sendCurrentAc()) {
    lastEspNowError = lastIrError.length() ? lastIrError : "IR send failed";
    return;
  }
  showLcdNotice("ESPNOW POWER", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
}

constexpr EspNowCommandRoute ESPNOW_COMMAND_ROUTES[] = {
    {"power", executeEspNowPowerCommand},
};
constexpr uint8_t ESPNOW_COMMAND_ROUTE_COUNT = sizeof(ESPNOW_COMMAND_ROUTES) / sizeof(ESPNOW_COMMAND_ROUTES[0]);

bool dispatchEspNowCommand(const String &cmd, const String &uid, const String &sender) {
  for (uint8_t i = 0; i < ESPNOW_COMMAND_ROUTE_COUNT; i++) {
    if (cmd == ESPNOW_COMMAND_ROUTES[i].cmd) {
      ESPNOW_COMMAND_ROUTES[i].handler(uid, sender);
      return true;
    }
  }

  lastEspNowUid = uid;
  lastEspNowCmd = cmd;
  lastEspNowSender = sender;
  lastEspNowRxMs = millis();
  lastEspNowError = "Unsupported command: " + cmd;
  showLcdNotice("ESPNOW CMD?", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
  return false;
}

void processEspNowCommand() {
  char payload[ESPNOW_MAX_PAYLOAD_SIZE + 1] = {0};
  size_t payloadLen = 0;
  if (!copyPendingEspNowPayload(payload, sizeof(payload), payloadLen)) return;
  recordEspNowDebugMessage(payload, payloadLen);

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload, payloadLen);
  if (error) {
    lastEspNowError = "Invalid JSON: " + String(error.c_str());
    lastEspNowRxMs = millis();
    showLcdNotice("ESPNOW JSON", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
    return;
  }

  String to = doc["to"] | "";
  to.trim();
  if (to != deviceId && to != "all") {
    lastEspNowIgnoredMs = millis();
    return;
  }

  if (!validateEspNowChecksum(doc)) {
    lastEspNowRxMs = millis();
    showLcdNotice("ESPNOW CHK", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
    return;
  }

  String uid = doc["uid"] | "";
  uid.trim();
  String cmd = doc["cmd"] | "";
  cmd.trim();
  cmd.toLowerCase();
  String sender = doc["id"] | "";

  if (!uid.length() || !cmd.length()) {
    lastEspNowError = "Missing uid or cmd";
    lastEspNowRxMs = millis();
    showLcdNotice("ESPNOW BAD", LcdNoticeKind::Warning, LCD_NOTICE_SHORT_MS);
    return;
  }

  if (uid == lastEspNowUid) {
    lastEspNowIgnoredMs = millis();
    return;
  }

  dispatchEspNowCommand(cmd, uid, sender);
}

void setupEspNow() {
  espNowReady = false;
  const esp_err_t initErr = esp_now_init();
  if (initErr != ESP_OK) {
    lastEspNowError = "ESP-NOW init failed: " + String(static_cast<int>(initErr));
    showLcdNotice("ESPNOW FAIL", LcdNoticeKind::Error);
    return;
  }
  if (esp_now_register_recv_cb(onEspNowReceived) != ESP_OK) {
    lastEspNowError = "ESP-NOW callback failed";
    showLcdNotice("ESPNOW CB", LcdNoticeKind::Error);
    return;
  }
  espNowReady = true;
  lastEspNowError = "";
}

bool runPresetAction(SleepPresetKind kind) {
  SleepPresetMode mode;
  if (!sleepPresetModeFromAir(mode)) {
    lastIrError = "Sleep preset requires current mode cool or heat";
    showLcdNotice("COOL/HEAT ONLY", LcdNoticeKind::Error);
    return false;
  }

  SleepPresetProfile &profile = sleepPresetProfile(kind, mode);
  const time_t actionEpoch = nextPresetEpoch(profile);
  if (!actionEpoch) {
    showLcdNotice("TIME NOT SET", LcdNoticeKind::Error);
    return false;
  }

  cancelPresetSchedule(false);
  air.power = true;
  air.temp = profile.startTemp;
  normalizeConfig();
  saveState();
  if (!sendCurrentAc()) return false;

  presetScheduleActive = true;
  presetActionEpoch = actionEpoch;
  activePresetKind = kind;
  activePresetMode = mode;
  activePresetAction = sleepPresetAction(kind);
  activePresetTargetTemp = profile.targetTemp;
  showLcdNotice(kind == SleepPresetKind::Weekend ? "WEEKEND SET" : "WORKDAY SET", LcdNoticeKind::Success);
  return true;
}

void processPresetSchedule() {
  if (!presetScheduleActive) return;
  const time_t now = time(nullptr);
  if (now <= 100000 || now < presetActionEpoch) return;

  const SleepPresetAction action = activePresetAction;
  const uint8_t targetTemp = activePresetTargetTemp;
  presetScheduleActive = false;
  presetActionEpoch = 0;
  activePresetKind = SleepPresetKind::None;
  noteActivity();

  if (action == SleepPresetAction::Off) {
    if (!air.power) {
      showLcdNotice("PRESET DONE", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
      return;
    }

    air.power = false;
    if (settingsMode) editAir.power = false;
    saveState();
    if (sendCurrentAc()) showLcdNotice("PRESET OFF", LcdNoticeKind::Success);
    return;
  }

  if (!air.power) {
    showLcdNotice("PRESET DONE", LcdNoticeKind::Success, LCD_NOTICE_SHORT_MS);
    return;
  }

  air.temp = targetTemp;
  if (settingsMode) editAir.temp = targetTemp;
  normalizeConfig();
  saveState();
  if (sendCurrentAc()) showLcdNotice("PRESET TEMP", LcdNoticeKind::Success);
}

String isoTime() {
  return currentIsoTimestamp();
}

String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void appendEspNowDebugMessagesJson(String &json) {
  json += ",\"debugMsg\":[";
  for (uint8_t i = 0; i < espNowDebugCount; i++) {
    const uint8_t index = (espNowDebugNextIndex + ESPNOW_DEBUG_MSG_COUNT - espNowDebugCount + i) % ESPNOW_DEBUG_MSG_COUNT;
    if (i) json += ",";
    json += "\"";
    json += jsonEscape(espNowDebugMessages[index]);
    json += "\"";
  }
  json += "]";
}

void appendSleepPresetProfileJson(String &json, const SleepPresetProfile &profile, SleepPresetAction action) {
  json += "{\"startTemp\":";
  json += profile.startTemp;
  json += ",\"time\":\"";
  json += presetTimeString(profile);
  json += "\",\"action\":\"";
  json += sleepPresetActionName(action);
  json += "\"";
  if (action == SleepPresetAction::Temp) {
    json += ",\"targetTemp\":";
    json += profile.targetTemp;
  }
  json += "}";
}

void appendAirStateJson(String &json, const AirState &state) {
  json += "{\"power\":";
  json += state.power ? "true" : "false";
  json += ",\"temp\":";
  json += state.temp;
  json += ",\"mode\":\"";
  json += state.mode;
  json += "\",\"fan\":\"";
  json += state.fan;
  json += "\",\"swing\":";
  json += state.swing ? "true" : "false";
  json += ",\"displayLight\":";
  json += state.displayLight ? "true" : "false";
  json += "}";
}

void appendIrRxEventJson(String &json, const IrRxEvent &event, bool includeState = false) {
  json += "{\"sequence\":";
  json += event.sequence;
  json += ",\"protocol\":\"";
  json += jsonEscape(event.protocol);
  json += "\",\"model\":";
  json += event.model;
  json += ",\"lastMs\":";
  json += event.receivedMs;
  json += ",\"acDecoded\":";
  json += event.acDecoded ? "true" : "false";
  json += ",\"confirmed\":";
  json += event.acDecoded ? "true" : "false";
  json += ",\"noise\":";
  json += event.noise ? "true" : "false";
  json += ",\"summary\":\"";
  json += jsonEscape(event.summary);
  json += "\",\"error\":\"";
  json += jsonEscape(event.error);
  json += "\"";
  if (includeState && event.acDecoded) {
    json += ",\"state\":";
    appendAirStateJson(json, event.state);
  }
  json += "}";
}

String stateJson() {
  const AcCapabilities capabilities = currentCapabilities();
  String json;
  json.reserve(2200);
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
  json += ",\"device\":{\"id\":\"";
  json += jsonEscape(deviceId);
  json += "\",\"wifi\":\"";
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
  json += ",\"backlightBrightness\":";
  json += backlightBrightness;
  json += ",\"settingsMode\":";
  json += settingsMode ? "true" : "false";
  json += "}";
  json += ",\"espnow\":{\"ready\":";
  json += espNowReady ? "true" : "false";
  json += ",\"channel\":";
  json += currentWiFiChannel();
  json += ",\"targetChannel\":";
  json += ESPNOW_CHANNEL;
  json += ",\"lastUid\":\"";
  json += jsonEscape(lastEspNowUid);
  json += "\",\"lastCmd\":\"";
  json += jsonEscape(lastEspNowCmd);
  json += "\",\"lastSender\":\"";
  json += jsonEscape(lastEspNowSender);
  json += "\",\"lastMs\":";
  json += lastEspNowRxMs;
  json += ",\"lastIgnoredMs\":";
  json += lastEspNowIgnoredMs;
  json += ",\"error\":\"";
  json += jsonEscape(lastEspNowError);
  json += "\"";
  appendEspNowDebugMessagesJson(json);
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
  json += ",\"preset\":{\"active\":";
  json += presetScheduleActive ? "true" : "false";
  json += ",\"scheduleActive\":";
  json += presetScheduleActive ? "true" : "false";
  json += ",\"kind\":\"";
  json += sleepPresetKindName(activePresetKind);
  json += "\",\"mode\":\"";
  json += sleepPresetModeName(activePresetMode);
  json += "\",\"action\":\"";
  json += sleepPresetActionName(activePresetAction);
  json += "\",\"time\":\"";
  if (presetScheduleActive) {
    char presetBuf[6];
    presetTimeLabel(presetBuf, sizeof(presetBuf));
    json += presetBuf;
  }
  json += "\",\"nextEpoch\":";
  json += presetScheduleActive ? static_cast<long>(presetActionEpoch) : 0;
  json += ",\"targetTemp\":";
  json += activePresetTargetTemp;
  json += ",\"profiles\":{\"weekday\":{\"cool\":";
  appendSleepPresetProfileJson(json, weekdayCoolPreset, SleepPresetAction::Off);
  json += ",\"heat\":";
  appendSleepPresetProfileJson(json, weekdayHeatPreset, SleepPresetAction::Off);
  json += "},\"weekend\":{\"cool\":";
  appendSleepPresetProfileJson(json, weekendCoolPreset, SleepPresetAction::Temp);
  json += ",\"heat\":";
  appendSleepPresetProfileJson(json, weekendHeatPreset, SleepPresetAction::Temp);
  json += "}}";
  json += "}";
  json += ",\"ir\":{\"protocol\":\"";
  json += jsonEscape(acProtocol);
  json += "\",\"model\":";
  json += acModel;
  json += ",\"lastSendMs\":";
  json += lastIrSendMs;
  json += ",\"lastError\":\"";
  json += jsonEscape(lastIrError);
  json += "\",\"rx\":";
  appendIrRxEventJson(json, lastValidIrRx);
  json += ",\"rxQueueCount\":";
  json += irRxEventCount;
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
  json.reserve(4096);
  json += "{\"ok\":true,\"rx\":";
  appendIrRxEventJson(json, lastValidIrRx);
  if (hasLastValidIrRx) {
    json += ",\"state\":";
    appendAirStateJson(json, lastValidIrRx.state);
  }
  json += ",\"queue\":{\"capacity\":";
  json += IR_RX_EVENT_QUEUE_SIZE;
  json += ",\"count\":";
  json += irRxEventCount;
  json += ",\"events\":[";
  for (uint8_t i = 0; i < irRxEventCount; i++) {
    if (i) json += ",";
    appendIrRxEventJson(json, irRxEvents[irRxEventIndexNewestFirst(i)], true);
  }
  json += "]}";
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
  json.reserve(4096);
  json += "{\"ok\":true,\"current\":\"";
  json += jsonEscape(acProtocol);
  json += "\",\"currentModel\":";
  json += acModel;
  json += ",\"protocols\":[";
  for (uint8_t i = 0; i < count; i++) {
    if (i) json += ",";
    json += "\"";
    json += jsonEscape(protocols[i]);
    json += "\"";
  }
  json += "],\"models\":{";
  bool firstProtocol = true;
  for (uint8_t i = 0; i < count; i++) {
    const decode_type_t protocol = strToDecodeType(protocols[i].c_str());
    if (!modelOptionCount(protocol)) continue;
    if (!firstProtocol) json += ",";
    firstProtocol = false;
    json += "\"";
    json += jsonEscape(protocols[i]);
    json += "\":[";
    bool firstModel = true;
    for (uint8_t j = 0; j < AC_MODEL_OPTION_COUNT; j++) {
      const AcModelOption &option = AC_MODEL_OPTIONS[j];
      if (option.protocol != protocol) continue;
      if (!firstModel) json += ",";
      firstModel = false;
      json += "{\"value\":";
      json += option.value;
      json += ",\"name\":\"";
      json += jsonEscape(option.name);
      json += "\",\"default\":";
      json += option.isDefault ? "true" : "false";
      json += "}";
    }
    json += "]";
  }
  json += "}}";
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

bool parseSleepPresetKindArg(SleepPresetKind &kind) {
  String value = server.hasArg("kind") ? server.arg("kind") : (server.hasArg("type") ? server.arg("type") : "weekday");
  value.toLowerCase();
  if (value == "weekday" || value == "workday" || value == "wd") {
    kind = SleepPresetKind::Weekday;
    return true;
  }
  if (value == "weekend" || value == "we") {
    kind = SleepPresetKind::Weekend;
    return true;
  }
  return false;
}

bool parsePresetTempArg(const String &value, uint8_t &temp) {
  if (!value.length()) return false;
  for (uint8_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) return false;
  }
  const AcCapabilities capabilities = currentCapabilities();
  const int parsed = value.toInt();
  if (parsed < capabilities.tempMin || parsed > capabilities.tempMax) return false;
  temp = static_cast<uint8_t>(parsed);
  return true;
}

bool updatePresetTempArg(const char *argName, uint8_t &target, bool &changed) {
  if (!server.hasArg(argName)) return true;
  uint8_t value;
  if (!parsePresetTempArg(server.arg(argName), value)) return false;
  target = value;
  changed = true;
  return true;
}

bool updatePresetTimeArg(const char *argName, SleepPresetProfile &profile, bool &changed) {
  if (!server.hasArg(argName)) return true;
  uint8_t hour;
  uint8_t minute;
  if (!parsePresetTime(server.arg(argName), hour, minute)) return false;
  profile.actionHour = hour;
  profile.actionMinute = minute;
  changed = true;
  return true;
}

bool applySleepPresetConfigArgs(bool &changed) {
  if (!updatePresetTempArg("wdCoolTemp", weekdayCoolPreset.startTemp, changed)) return false;
  if (!updatePresetTimeArg("wdCoolTime", weekdayCoolPreset, changed)) return false;
  if (!updatePresetTempArg("wdHeatTemp", weekdayHeatPreset.startTemp, changed)) return false;
  if (!updatePresetTimeArg("wdHeatTime", weekdayHeatPreset, changed)) return false;
  if (!updatePresetTempArg("weCoolTemp", weekendCoolPreset.startTemp, changed)) return false;
  if (!updatePresetTimeArg("weCoolTime", weekendCoolPreset, changed)) return false;
  if (!updatePresetTempArg("weCoolTarget", weekendCoolPreset.targetTemp, changed)) return false;
  if (!updatePresetTempArg("weHeatTemp", weekendHeatPreset.startTemp, changed)) return false;
  if (!updatePresetTimeArg("weHeatTime", weekendHeatPreset, changed)) return false;
  if (!updatePresetTempArg("weHeatTarget", weekendHeatPreset.targetTemp, changed)) return false;
  return true;
}

void handlePreset() {
  noteActivity();
  if (rejectIfSettingsMode()) return;

  bool changed = false;
  if (!applySleepPresetConfigArgs(changed)) {
    sendError(400, "Invalid preset config argument");
    return;
  }

  if (changed) {
    normalizeConfig();
    saveConfig();
    showLcdNotice("PRESET SAVED", LcdNoticeKind::Info);
  }

  if (server.hasArg("run") || server.hasArg("kind") || server.hasArg("type")) {
    SleepPresetKind kind;
    if (!parseSleepPresetKindArg(kind)) {
      sendError(400, "Invalid preset kind");
      return;
    }
    if (!runPresetAction(kind)) {
      sendError(500, lastIrError.length() ? lastIrError : "Preset start failed");
      return;
    }
  }

  sendApi(stateJson());
}

void handlePresetRun() {
  noteActivity();
  if (rejectIfSettingsMode()) return;
  SleepPresetKind kind;
  if (!parseSleepPresetKindArg(kind)) {
    sendError(400, "Invalid preset kind");
    return;
  }
  if (!runPresetAction(kind)) {
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
  bool backlightChanged = false;
  String nextProtocol = acProtocol;
  int16_t nextModel = acModel;
  if (server.hasArg("protocol")) {
    nextProtocol = server.arg("protocol");
    nextProtocol.toUpperCase();
    if (!validProtocol(nextProtocol)) {
      sendError(400, "Unsupported IR protocol");
      return;
    }
    if (!server.hasArg("model")) {
      nextModel = defaultModelForProtocol(strToDecodeType(nextProtocol.c_str()));
    }
  }
  if (server.hasArg("model")) {
    if (!parseModelValue(server.arg("model"), nextModel)) {
      sendError(400, "Invalid IR model");
      return;
    }
  }
  if (server.hasArg("protocol") || server.hasArg("model")) {
    const decode_type_t protocol = strToDecodeType(nextProtocol.c_str());
    if (!validModelForProtocol(protocol, nextModel)) {
      sendError(400, "IR model is not valid for selected protocol");
      return;
    }
    acProtocol = nextProtocol;
    acModel = nextModel;
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
  if (server.hasArg("brightness") || server.hasArg("backlightBrightness") || server.hasArg("lcdBrightness")) {
    const String arg = server.hasArg("brightness") ? server.arg("brightness") : (server.hasArg("backlightBrightness") ? server.arg("backlightBrightness") : server.arg("lcdBrightness"));
    uint8_t brightness;
    if (!parseBacklightBrightness(arg, brightness)) {
      sendError(400, "Backlight brightness out of range");
      return;
    }
    setBacklightBrightness(brightness);
    backlightChanged = true;
  }
  if (wifiChanged) wifiRestartRequired = true;
  normalizeConfig();
  saveConfig();
  if (wifiChanged) {
    showLcdNotice("WIFI SAVE 10s", LcdNoticeKind::Info);
  } else if (irChanged) {
    showLcdNotice("BRAND SAVE 10s", LcdNoticeKind::Info);
  } else if (backlightChanged) {
    showLcdNotice("BRIGHT SAVE", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
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
  if (!hasLastValidIrRx) {
    sendError(400, "No decoded A/C IR state available");
    return;
  }

  const bool updateProtocol = !server.hasArg("protocol") || !isFalsy(server.arg("protocol"));
  if (updateProtocol && lastValidIrRx.protocol.length() && validProtocol(lastValidIrRx.protocol)) {
    acProtocol = lastValidIrRx.protocol;
    const decode_type_t protocol = strToDecodeType(acProtocol.c_str());
    acModel = validModelForProtocol(protocol, lastValidIrRx.model)
                  ? lastValidIrRx.model
                  : defaultModelForProtocol(protocol);
  }
  air = lastValidIrRx.state;
  normalizeConfig();
  if (!air.power) cancelPresetSchedule(false);
  saveState();
  showLcdNotice("IR APPLIED", LcdNoticeKind::Success);
  sendApi(stateJson());
}

void handleHelp() {
  noteActivity();
  sendApi(F("{\"ok\":true,\"configFile\":\"/config.json\",\"endpoints\":[\"/api/state\",\"/api/protocols\",\"/api/control?power=on&mode=cool&temp=26&fan=auto&swing=off&light=on\",\"/api/power?value=toggle\",\"/api/temp?delta=1\",\"/api/mode?value=heat\",\"/api/fan?value=5\",\"/api/swing?value=toggle\",\"/api/light?value=toggle\",\"/api/preset?wdCoolTemp=25&wdCoolTime=07:50&weCoolTarget=27\",\"/api/preset/run?kind=weekday\",\"/api/preset/run?kind=weekend\",\"/api/preset/cancel\",\"/api/send\",\"/api/ir\",\"/api/ir/apply\",\"/api/config?protocol=KELVINATOR&model=1\",\"/api/config?brightness=30\",\"/api/config?ssid=YOUR_WIFI&password=YOUR_PASSWORD\",\"/api/reload-config\"]}"));
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
  if (!sameAirState(editAir, air) || editBacklightBrightness != backlightBrightness) u8g2.drawStr(48, 22, "EDIT");
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
    case SettingItem::Backlight:
      u8g2.drawFrame(13, 29, 46, 10);
      u8g2.drawBox(15, 31, map(editBacklightBrightness, LCD_BACKLIGHT_MIN_BRIGHTNESS, LCD_BACKLIGHT_MAX_BRIGHTNESS, 1, 42), 6);
      u8g2.setFont(u8g2_font_6x12_tf);
      snprintf(buf, sizeof(buf), "%u%%", editBacklightBrightness);
      u8g2.drawStr(70, 42, buf);
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
    char presetTimeBuf[6];
    presetTimeLabel(presetTimeBuf, sizeof(presetTimeBuf));
    snprintf(buf, sizeof(buf), "%s %s", sleepPresetLcdShortLabel(), presetTimeBuf);
    u8g2.drawStr(58, 61, buf);
  } else {
    u8g2.drawStr(64, 61, air.swing ? "SWING" : "FIX");
  }
  const AcCapabilities capabilities = currentCapabilities();
  if (!presetScheduleActive) u8g2.drawStr(100, 61, capabilities.supportsLight ? (air.displayLight ? "LCD" : "lcd") : "--");

  const char *statusMark = "";
  if (lastIrError.length()) {
    statusMark = "!";
  } else if (lastIrSendMs && millis() - lastIrSendMs < 3000) {
    statusMark = "*";
  } else if (lastEspNowRxMs && millis() - lastEspNowRxMs < 5000) {
    statusMark = "E";
  } else if (hasLastValidIrRx && lastValidIrRx.receivedMs && millis() - lastValidIrRx.receivedMs < 5000) {
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

void stepDraftBacklight(int8_t direction) {
  int value = editBacklightBrightness + (direction > 0 ? 5 : -5);
  if (value > LCD_BACKLIGHT_MAX_BRIGHTNESS) value = LCD_BACKLIGHT_MIN_BRIGHTNESS;
  if (value < LCD_BACKLIGHT_MIN_BRIGHTNESS) value = LCD_BACKLIGHT_MAX_BRIGHTNESS;
  editBacklightBrightness = static_cast<uint8_t>(value);
  previewBacklightBrightness(editBacklightBrightness);
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
    case SettingItem::Backlight:
      stepDraftBacklight(direction);
      break;
    default:
      break;
  }
}

void enterSettingsMode() {
  editAir = air;
  editBacklightBrightness = backlightBrightness;
  settingsIndex = 0;
  settingsMode = true;
  noteActivity();
  showLcdNotice("SETTINGS", LcdNoticeKind::Info, LCD_NOTICE_SHORT_MS);
  drawDisplay();
}

void commitSettingsMode() {
  air = editAir;
  setBacklightBrightness(editBacklightBrightness);
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
  } else if (presetScheduleActive) {
    cancelPresetSchedule(true);
    drawDisplay();
  }
}

void pollIrReceiver() {
  if (!irrecv.decode(&irResults)) return;

  const uint32_t now = millis();
  if (lastIrSendMs && now - lastIrSendMs < 700) return;

  IrRxEvent event;
  event.sequence = irRxNextSequence++;
  event.receivedMs = now;
  event.protocol = typeToString(irResults.decode_type, irResults.repeat);
  event.error = irResults.overflow ? "Capture buffer overflow" : "";
  event.noise = irResults.overflow || irResults.decode_type == decode_type_t::UNKNOWN;

  String summary = IRAcUtils::resultAcToString(&irResults);
  if (!summary.length()) summary = resultToHumanReadableBasic(&irResults);
  summary.replace("\r", " ");
  summary.replace("\n", " ");
  if (summary.length() > IR_RX_SUMMARY_MAX_LENGTH) {
    summary.remove(IR_RX_SUMMARY_MAX_LENGTH);
    summary += "...";
  }
  event.summary = summary;

  stdAc::state_t decoded = ac.next;
  if (!irResults.overflow && IRAcUtils::decodeToState(&irResults, &decoded, &ac.next)) {
    const String protocolName = typeToString(decoded.protocol);
    const AcCapabilities capabilities = capabilitiesForProtocol(protocolName);
    event.state = airFromStdState(decoded, capabilities);
    event.protocol = protocolName;
    event.model = decoded.model;
    event.acDecoded = true;
    event.noise = false;
    lastValidIrRx = event;
    hasLastValidIrRx = true;
  }

  enqueueIrRxEvent(event);

  if (!settingsMode) {
    if (event.acDecoded) {
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
  WiFi.softAP(IRSTATION_AP_SSID, IRSTATION_AP_PASSWORD, ESPNOW_CHANNEL);
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
  backlightPwmReady = ledcAttach(PIN_LCD_BL, LCD_BACKLIGHT_PWM_HZ, LCD_BACKLIGHT_PWM_BITS);
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
  applyBacklightOutput();

  u8g2.begin();
  u8g2.setContrast(20);
  u8g2.setDisplayRotation(U8G2_R2);
  drawDisplay();

  irrecv.enableIRIn();
  setupWiFi();
  setupEspNow();
  setupWeb();
  drawDisplay();
}

void loop() {
  server.handleClient();
  button1.tick();
  button2.tick();
  processEspNowCommand();
  pollIrReceiver();
  refreshBacklight();
  flushConfigSaveIfDue();
  processPresetSchedule();

  if (displayDirty || millis() - lastDisplayRefreshMs > DISPLAY_REFRESH_MS) {
    lastDisplayRefreshMs = millis();
    drawDisplay();
  }
}
