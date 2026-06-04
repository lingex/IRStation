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

## Wi-Fi

Copy `include/secrets.example.h` to `include/secrets.h`, then set:

```cpp
#define WIFI_SSID "your-router"
#define WIFI_PASSWORD "your-password"
```

If no SSID is configured, the device starts an AP named `IRStation` with password `12345678`.

## HTTP GET APIs

All control endpoints update the persisted state and send an IR command unless `send=0` is provided.

| Endpoint | Example |
| --- | --- |
| Current state | `/api/state` |
| Partial control | `/api/control?power=on&mode=cool&temp=26&fan=auto&swing=off` |
| Power | `/api/power?value=toggle` |
| Temperature | `/api/temp?delta=1` or `/api/temp?value=25` |
| Mode | `/api/mode?value=heat` |
| Fan | `/api/fan?value=high` |
| Swing | `/api/swing?value=toggle` |
| Resend current state | `/api/send` |
| IR protocol config | `/api/config?protocol=GREE&model=1` |
| API examples | `/api/help` |

Supported UI values:

- `power`: `on`, `off`, `toggle`
- `mode`: `auto`, `cool`, `heat`, `dry`, `fan`
- `fan`: `auto`, `low`, `medium`, `high`
- `swing`: `on`, `off`, `toggle`

The default IR protocol is `GREE`, configured in `platformio.ini`. Change it there or call `/api/config?protocol=...&model=...` after flashing. The exact protocol must match your air conditioner for IR sending to work.

## Build

The project uses PlatformIO's `huge_app.csv` partition layout so the 4MB flash has enough room for the web UI, U8g2, and air-conditioner IR protocol support. OTA update partitions are not reserved in this first firmware.

```powershell
pio run
```
