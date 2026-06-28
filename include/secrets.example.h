#pragma once

// Optional build-time defaults. Runtime settings are stored in LittleFS:
// /config.json. If WIFI_SSID is empty and /config.json has no SSID, IRStation
// starts a fallback AP instead.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Optional: override the default fallback AP.
#define IRSTATION_AP_SSID "IRStation"
#define IRSTATION_AP_PASSWORD "12345678"
