# IRStation

DIY ESP32-C3 air-conditioner IR remote with a 128x64 ST7567 LCD, web control, and HTTP GET APIs.

## Hardware Pins

| Function | ESP32-C3 GPIO | Schematic Net |
| --- | ---: | --- |
| IR LED TX | GPIO1 | `IR_OUT` |
| IR receiver | GPIO3 | `IR_IN` |
| LCD reset | GPIO4 | `LCD_RST` |
| LCD D/C | GPIO5 | `LCD_DC` |
| LCD SCK | GPIO6 | `LCD_SCK` |
| LCD SDA/MOSI | GPIO7 | `LCD_SDA` |
| LCD backlight | GPIO8 | `LCD_BL` |
| Button 1 | GPIO9 | `B1` |
| Button 2 | GPIO0 | `B2` |
| Status LED | GPIO10 | `LED` |

The LCD is driven as an ST7567 128x64 display with U8g2 software SPI. Backlight is PWM dimmed on `LCD_BL`, treated as active-low because the schematic uses a P-channel MOSFET high-side switch. Saved brightness is `1`-`100`, defaulting to `30`.

## LCD And Buttons

Normal mode:

- `B1` short press wakes the LCD backlight and shows a short LCD notice.
- `B1` long press enters LCD settings mode.
- `B2` short press toggles A/C power and sends IR immediately.
- `B2` long press cancels an active sleep preset in normal mode.

Sleep presets are started only from the web UI or API. `Workday` and `Weekend`
each have separate `cool` and `heat` settings selected from the current A/C
mode. Starting a preset sets the configured start temperature and sends IR; if
the A/C is off, it is turned on first. Workday presets turn the A/C off at the
configured time. Weekend presets change to the configured target temperature at
the configured time. After the scheduled action runs, the controller returns to
normal state. Any B2 or web power operation cancels an active preset.

LCD settings mode edits a draft state only. `B1` short press moves to the next
available setting, `B1` long press commits the draft and schedules a debounced
save, `B2` short press moves the selected value forward, and `B2` long press
moves it backward. Unsupported settings for the selected protocol are skipped. LCD backlight brightness is also available in settings mode and is adjusted in 5% steps.

The normal LCD page shows time/date, AP/STA Wi-Fi status, signal bars, A/C
state, fan bars, swing, and indoor display light state when supported. Short
inverted notices appear for send/save/config/IR-learning events. The bottom
right status mark is `!` for IR error, `*` for recent IR send, `R` for recent
decoded A/C IR receive, and `S` for a pending config save.

## Wi-Fi

Runtime configuration is stored in the 256K LittleFS partition as `/config.json`.
Edit `data/config.json` before uploading the filesystem, or edit `/config.json`
with a web file tool after the device is running. After an in-place file edit,
call `/api/reload-config` or reboot so the firmware refreshes its in-memory
settings before the next control command writes the file again.

Runtime changes are saved with a 5 second debounce: each operation updates RAM
immediately, then writes `/config.json` after 5 seconds with no further
operation.

```json
{
  "id": "irstation-01",
  "wifi": {
    "ssid": "your-router",
    "password": "your-password"
  },
  "state": {
    "power": false,
    "temp": 26,
    "mode": "cool",
    "fan": "auto",
    "swing": false,
    "displayLight": true
  },
  "lcd": {
    "backlightBrightness": 30
  },
  "ir": {
    "protocol": "KELVINATOR",
    "model": 1
  },
  "preset": {
    "weekday": {
      "cool": {"startTemp": 25, "time": "07:50", "action": "off"},
      "heat": {"startTemp": 22, "time": "07:50", "action": "off"}
    },
    "weekend": {
      "cool": {"startTemp": 25, "time": "08:00", "action": "temp", "targetTemp": 27},
      "heat": {"startTemp": 22, "time": "08:00", "action": "temp", "targetTemp": 24}
    }
  }
}
```

If no SSID is configured, the device starts an AP named `IRStation` with password `12345678`. `include/secrets.h` is only an optional build-time fallback when `/config.json` is missing.
When the device is in AP mode, the web UI shows a Wi-Fi setup panel. Saving
SSID/password updates `/config.json`; reboot the device to join the new network.

`id` is this receiver's ESP-NOW identity. ESP-NOW commands are JSON messages addressed with `to`, for example `{"to":"irstation-01","uid":"unique-id","cmd":"power","chk":"CRC32"}`. Use `"to":"all"` to broadcast to every receiver. `chk` is required and is the uppercase CRC32 of canonical JSON with top-level `chk` omitted. Repeated messages with the same `uid` are ignored after the first handled command.

## HTTP GET APIs

All control endpoints update the persisted state and send an IR command unless `send=0` is provided.
Control/config/send endpoints return HTTP 409 while the LCD is in settings mode,
so browser or automation commands cannot overwrite an in-progress button edit.

| Endpoint | Example |
| --- | --- |
| Current state | `/api/state` |
| Supported AC brands/protocols | `/api/protocols` |
| Partial control | `/api/control?power=on&mode=cool&temp=26&fan=auto&swing=off&light=on` |
| Power | `/api/power?value=toggle` |
| Temperature | `/api/temp?delta=1` or `/api/temp?value=25` |
| Mode | `/api/mode?value=heat` |
| Fan | `/api/fan?value=5` |
| Swing | `/api/swing?value=toggle` |
| Indoor display light | `/api/light?value=toggle` |
| Save sleep preset config | `/api/preset?wdCoolTemp=25&wdCoolTime=07:50&weCoolTarget=27` |
| Start sleep preset | `/api/preset/run?kind=weekday` or `/api/preset/run?kind=weekend` |
| Cancel active preset | `/api/preset/cancel` |
| Resend current state | `/api/send` |
| Confirmed IR and receive event queue | `/api/ir` |
| Apply learned A/C state | `/api/ir/apply` |
| IR protocol config | `/api/config?protocol=KELVINATOR&model=1` |
| LCD backlight brightness | `/api/config?brightness=30` |
| Wi-Fi config | `/api/config?ssid=YOUR_WIFI&password=YOUR_PASSWORD` |
| Reload JSON config | `/api/reload-config` |
| API examples | `/api/help` |

Supported UI values:

- `power`: `on`, `off`, `toggle`
- `mode`: `auto`, `cool`, `heat`, `dry`, `fan`
- `fan`: `auto`, `1`, `2`, `3`, `4`, `5`
- legacy fan aliases are accepted: `low` -> `2`, `medium` -> `3`, `high` -> `4`
- `swing`: `on`, `off`, `toggle`
- `light` / `displayLight`: `on`, `off`, `toggle`
- `brightness` / `backlightBrightness` / `lcdBrightness`: `1`-`100`, saved as LCD backlight percentage
- `kind`: `weekday`, `weekend`
- sleep preset config args: `wdCoolTemp`, `wdCoolTime`, `wdHeatTemp`,
  `wdHeatTime`, `weCoolTemp`, `weCoolTime`, `weCoolTarget`, `weHeatTemp`,
  `weHeatTime`, `weHeatTarget`

`/api/protocols` keeps the `protocols` string array and also returns a `models`
map for protocols with remote-model variants. The web UI shows a **Remote
profile** selector only when the selected protocol has multiple variants. This
is the remote-control profile, not the indoor-unit product model. Changing the
protocol without supplying `model` selects that protocol's default model;
invalid protocol/model combinations return HTTP 400. For example, GREE YBOFB
can be selected with `/api/config?protocol=GREE&model=2`.

`/api/state` includes a `capabilities` object for the selected protocol. The
firmware currently has explicit capability profiles for `KELVINATOR` and `GREE`;
other protocols use a conservative generic profile. Unsupported display-light
controls are hidden in the web UI and are not sent in IR commands.

The sleep preset schedule is implemented locally by the controller. It does not
rely on brand-specific A/C clock IR support. The active schedule is runtime
state; the four profile settings are saved in LittleFS. The LCD bottom row shows
short active labels such as `WDC 07:50` or `WEH 08:00`.

The IR receiver listens continuously. It keeps the eight newest captures in a
bounded, newest-first event queue returned by `/api/ir`. `UNKNOWN` signals and
capture overflows are marked as noise and remain available for diagnostics, but
they do not replace the last confirmed A/C state. `/api/ir/apply` always uses
that last confirmed state to update the saved remote state and learned
protocol/model without sending IR.

The default IR protocol is `KELVINATOR`, configured in `platformio.ini` and mirrored in `data/config.json`. Change `/config.json` or call `/api/config?protocol=...&model=...` after flashing. The exact protocol must match your air conditioner for IR sending to work.

## Build

The project uses a custom 4MB partition table: a 3MB factory app partition plus a 256K LittleFS partition named `littlefs`. OTA update partitions are not reserved in this first firmware.

```powershell
pio run
pio run --target uploadfs
```
