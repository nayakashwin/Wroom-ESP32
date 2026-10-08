# 01Wifi — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Firmware: WiFi connect + ICMP ping of google.com. **No serial CLI.**
  - `WIFI_SSID`, `WIFI_PASS` come from `_secrets/secrets.h` (gitignored —
    copy `_secrets/secrets.example.h` and fill it in; see the root
    `README.md`)
  - On boot it connects automatically; on connect timeout (20 s) or link
    loss it retries forever (3 s pause between attempts)
  - One automatic ping session right after the first connect (3 pings,
    1 s apart, hard lifetime cap `PING_TOTAL_LIMIT = 3`), then it stops
  - Serial input is ignored entirely (typed commands are not read)
  - LED GPIO2: continuous heartbeat (lub-dub: 90 ms on / 160 ms off / 90 ms
    on / 1.1 s pause), driven by a `Ticker` (10 ms) — keeps beating during
    the blocking ping session
  - Pings use the lwIP `ping/ping_sock.h` (esp_ping) API
  - Sample output:
    `connected to "<your-ssid>" ip=192.168.3.32 rssi=-78 dBm` /
    `PING google.com (142.250.x.x) 64 bytes of data` /
    `3 packets transmitted, 3 received, 0% packet loss` /
    `3 pings done - stopping, no more pings`
- Build output: `.pio/build/default/firmware.bin`
