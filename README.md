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

The LCD is driven as an ST7567 128x64 display with U8g2 software SPI. Backlight is treated as active-low because the schematic uses a P-channel MOSFET high-side switch.

## LCD And Buttons

Normal mode:

- `B1` short press wakes the LCD backlight and shows a short LCD notice.
- `B1` long press enters LCD settings mode.
- `B2` short press toggles A/C power and sends IR immediately, unless the
  one-key off-time preset is enabled.

When the one-key off-time preset is enabled, `B2` starts the preset instead of
toggling power. If the A/C is off, the firmware turns it on and schedules power
off at the next configured local clock time. If the A/C is already on, it only
schedules or refreshes that off time. During an active preset, any B2 or web
power operation cancels the preset mode and returns B2 to normal power toggle
behavior.

LCD settings mode edits a draft state only. `B1` short press moves to the next
available setting, `B1` long press commits the draft and schedules a debounced
save, `B2` short press moves the selected value forward, and `B2` long press
moves it backward. Unsupported settings for the selected protocol are skipped.

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

Runtime changes are saved with a 10 second debounce: each operation updates RAM
immediately, then writes `/config.json` after 10 seconds with no further
operation.

```json
{
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
  "ir": {
    "protocol": "KELVINATOR",
    "model": 1
  },
  "preset": {
    "enabled": false,
    "offTime": "07:30"
  }
}
```

If no SSID is configured, the device starts an AP named `IRStation` with password `12345678`. `include/secrets.h` is only an optional build-time fallback when `/config.json` is missing.
When the device is in AP mode, the web UI shows a Wi-Fi setup panel. Saving
SSID/password updates `/config.json`; reboot the device to join the new network.

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
| One-key off-time preset | `/api/preset?enabled=on&time=07:30` |
| Start preset now | `/api/preset/run` |
| Cancel active preset | `/api/preset/cancel` |
| Resend current state | `/api/send` |
| Last received IR | `/api/ir` |
| Apply learned A/C state | `/api/ir/apply` |
| IR protocol config | `/api/config?protocol=KELVINATOR&model=1` |
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
- `preset.enabled`: `on`, `off`, `toggle`
- `preset.offTime` / `time`: `HH:MM`, local 24-hour time

`/api/state` includes a `capabilities` object for the selected protocol. The
firmware currently has explicit capability profiles for `KELVINATOR` and `GREE`;
other protocols use a conservative generic profile. Unsupported display-light
controls are hidden in the web UI and are not sent in IR commands.

The preset schedule is implemented locally by the controller. Starting the preset
does not rely on brand-specific A/C clock IR support: the controller sends power
on if needed, schedules the next matching local clock time such as `07:30`, then
sends power off when that time arrives. The active schedule is runtime state;
the configured clock time is saved in LittleFS.

The IR receiver listens continuously. `/api/ir` returns the latest decoded
signal. If it can be converted to a common A/C state, `/api/ir/apply` updates
the saved remote state and learned protocol/model without sending IR.

The default IR protocol is `KELVINATOR`, configured in `platformio.ini` and mirrored in `data/config.json`. Change `/config.json` or call `/api/config?protocol=...&model=...` after flashing. The exact protocol must match your air conditioner for IR sending to work.

## Build

The project uses a custom 4MB partition table: a 3MB factory app partition plus a 256K LittleFS partition named `littlefs`. OTA update partitions are not reserved in this first firmware.

```powershell
pio run
pio run --target uploadfs
```
