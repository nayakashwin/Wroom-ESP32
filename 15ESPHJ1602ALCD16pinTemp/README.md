# 15ESPHJ1602ALCD16pinTemp — Chesterfield, MO weather clock on a 16×2 LCD

WiFi weather station for the HJ1602A 16×2 character LCD: joins WiFi, syncs
the clock over NTP (America/Chicago), and queries **Open-Meteo** for the
current temperature and relative humidity of **Chesterfield, Missouri**
(38.6631 N, 90.5771 W) every 10 minutes.  No API key required.

```
┌────────────────┐
│ 7:45 AM Wed    │   row 0: local time (no seconds)
│45°F 7°C RH 89% │   row 1: rounded temp in °F and °C + humidity
└────────────────┘
```

## Wiring

Identical to [`14ESPHJ1602ALCD16pin`](../14ESPHJ1602ALCD16pin/README.md):

| LCD pin | Function | ESP32 (NodeMCU-32S) |
|:-------:|----------|---------------------|
| 1 (VSS) | GND      | GND                 |
| 2 (VDD) | 3.3 V    | 3.3 V               |
| 3 (V0)  | Contrast | 10 kΩ pot (VDD↔GND, wiper → V0) |
| 4 (RS)  | Register | GPIO4               |
| 5 (RW)  | Read/Write | GND (hard-wired)  |
| 6 (E)   | Enable   | GPIO5               |
| 7–10    | D0–D3    | NC (4-bit mode)     |
| 11 (D4) | Data     | GPIO14              |
| 12 (D5) | Data     | GPIO12              |
| 13 (D6) | Data     | GPIO13              |
| 14 (D7) | Data     | GPIO15              |
| 15 (A)  | LED +    | 3.3 V (some modules need ~220 Ω series R) |
| 16 (K)  | LED −    | GND                 |

**Contrast pot is required** — without it the display is blank or all blocks.

## Secrets

Needs `WIFI_SSID` and `WIFI_PASS` from `_secrets/secrets.h` (gitignored):

```bash
cp _secrets/secrets.example.h _secrets/secrets.h
$EDITOR _secrets/secrets.h
```

## Data sources

| What | Source | Notes |
|---|---|---|
| Weather | `GET https://api.open-meteo.com/v1/forecast?latitude=38.6631&longitude=-90.5771&current=temperature_2m,relative_humidity_2m&temperature_unit=fahrenheit&timezone=America%2FChicago` | free, no API key; refreshed every 10 min (retry after 30 s on failure) |
| Time | NTP `pool.ntp.org` / `time.nist.gov` / `time.google.com`, re-synced every 30 min, POSIX TZ `CST6CDT,M3.2.0,M11.1.0` | America/Chicago with automatic CST/CDT; if NTP is blocked (guest VLANs often filter UDP/123) the clock falls back to the API's `current.time` + `utc_offset_seconds` (15-min resolution, re-corrected each fetch) |

HTTPS uses `WiFiClientSecure::setInsecure()` (no cert pinning), the same
approach as `02TelegramTX`.

## Display behaviour

| State | Row 0 | Row 1 |
|---|---|---|
| NTP not yet synced | `--:-- -- ---` | weather as usual |
| No weather yet | time as usual | `--°F --°C RH ---%` |
| WiFi lost | rows freeze at last values | auto-reconnect every 5 s, then refetch |

The `°` character is HD44780 font code `0xDF` (present on HJ1602A /
A00-font modules).  LED GPIO2 flashes 90 ms once per second as a heartbeat;
status lines (WiFi, HTTP code, heap) go to the serial console.

## Usage

```bash
pio run -e default -t upload     # build & flash
pio device monitor -b 115200     # serial monitor
```

## Hardware notes

- Any HD44780-compatible 1602 works, not just the HJ1602A.
- Requires `LiquidCrystal` and `ArduinoJson` — both in `lib_deps`.
- Change the location by editing `API_PATH` (latitude/longitude) and
  `TZ_POSIX` at the top of `src/main.cpp`.
