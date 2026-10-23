# 15ESPHJ1602ALCD16pinTemp — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcu-32s` — the stock espressif32@6.x
  ID for the NodeMCU-32S; the sibling projects' `nodemcuv3` needs a custom
  board JSON, LED on GPIO2). `lib_deps`
  = `arduino-libraries/LiquidCrystal@^1.0.7` + `bblanchon/ArduinoJson@^7`.
  `build_flags` include `-I../_secrets`.
- Secrets: only `WIFI_SSID`, `WIFI_PASS` from `_secrets/secrets.h`
  (gitignored — copy `_secrets/secrets.example.h` and fill it in).
- Build output: `.pio/build/default/firmware.bin`.
- Firmware: WiFi weather clock for an HJ1602A 16×2 LCD (same pins as 14:
  RS=4, E=5, D4=14, D5=12, D6=13, D7=15).
  - Row 0: local time `%I:%M %p %a` (no seconds) with the leading zero
    stripped (America/Chicago, NTP `pool.ntp.org`/`time.nist.gov`/
    `time.google.com` re-synced every 30 min via
    `sntp_set_sync_interval(30*60*1000)`, `#include <esp_sntp.h>`, +
    POSIX TZ `CST6CDT,M3.2.0,M11.1.0`; if NTP is blocked the clock is
    set from the API's `current.time` − `utc_offset_seconds`), redrawn
    only when it changes (1 s tick); `--:-- -- ---` until time is set
  - Row 1: `%.0f°F %.0f°C RH %d%%` for Chesterfield, MO (38.6631,
    −90.5771), whole numbers, °C computed from °F; `°` is HD44780 code
    `0xDF` — in `snprintf` it MUST stay a separate adjacent string
    literal (`"%.0f\xDF" "F %.0f\xDF" "C RH %d%%"`) or the hex escape
    swallows the next character
  - Weather: HTTPS GET Open-Meteo `current=temperature_2m,
    relative_humidity_2m&temperature_unit=fahrenheit` (no API key),
    `WiFiClientSecure::setInsecure()` (02TelegramTX style), ArduinoJson
    v7 `JsonDocument` (~350 B response, no filter). Parse
    `http.getString()` — NOT `getStream()`, which exposes chunked-transfer
    length headers to the parser (→ InvalidInput); refresh 10 min,
    retry 30 s after failure
  - WiFi watchdog on the 1 s tick: lost link → `WiFi.disconnect()` +
    blocking `connectWiFi()` (20 s timeout), weather retried every 5 s
    while down; rows freeze at last values; `connectWiFi()` scans first
    and pins the join to the strongest BSS of `WIFI_SSID` (channel +
    BSSID) — there is a same-SSID repeater/neighbor at ~−80 dBm that
    otherwise stalls association; on connect TIMEOUT a 2.4 GHz scan
    (`SSID(rssi)` list) is dumped to serial for debugging
  - LED GPIO2 heartbeat: 90 ms flash per 1 s tick
- Rows are padded to 16 chars and diffed against `shown0/shown1` — only
  changed rows are rewritten (no flicker).
- Location/timezone are constants at the top of `main.cpp`
  (`API_PATH`, `TZ_POSIX`) — edit there to move the clock.

## Files

- `src/main.cpp` — app: WiFi connect/watchdog, NTP, Open-Meteo fetch
  (HTTPS + ArduinoJson), 1 s display tick, LCD diffing
- `platformio.ini` — env knobs, `-I../_secrets`, lib_deps
