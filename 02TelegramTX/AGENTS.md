# 02TelegramTX — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Firmware: Telegram **sender**. 01Wifi-style connect, results pushed to
  Telegram. **The only serial interaction: type a line + Enter → it is sent
  to the channel.** No other commands, no NVS.
  - `WIFI_SSID`, `WIFI_PASS`, `BOT_TOKEN`, `CHAT_ID` come from
    `_secrets/secrets.h` (gitignored — copy
    `_secrets/secrets.example.h` and fill it in; see the root `README.md`). `CHAT_ID` accepts a
    numeric id (-100... for channels), @channelusername or a user/group id)
  - Auto-connect on boot; timeout (20 s) / link loss → auto-retry (3 s pause)
  - On first WiFi connect it sends `02TelegramTX online: ssid=... ip=...
    rssi=... dBm`
  - Typed lines (max 159 chars, `TX_LINE_MAX`) are sent verbatim — there is
    no command parser; typing e.g. `help` just sends the text `help`
  - Telegram: HTTPS POST `https://api.telegram.org/bot<token>/sendMessage`
    with `WiFiClientSecure::setInsecure()` (no cert pinning), JSON payload
    `{"chat_id":"...","text":"..."}`; success = HTTP 200 + `{"ok":true...`
  - LED GPIO2: heartbeat (90 ms on / 160 ms off / 90 ms on / 1.1 s pause),
    driven by a `Ticker` (10 ms) — keeps beating during blocking sends
- Build output: `.pio/build/default/firmware.bin`

## Memory / heap management

- Every send prints `telegram send -> HTTP <code> ok|FAIL (heap free=…
  min=… maxblk=…)`; `maxblk` (`ESP.getMaxAllocHeap()`) must stay ~110 KB
  after repeated sends — dropping near 40 KB (`HEAP_WARN_BYTES`) means
  fragmentation threatens TLS (a `WARN` line is printed then).
- Error/body prints use `printf("%.Ns", body.c_str())` — never
  `body.substring(...)`, which allocates a copy.
- Transients are locals in `telegramSend()`: `url`/`payload`/`client`/
  `http`/`body` all freed on return (client before payload ✓ declaration
  order).
- Verified: 6-message stress (+2 auto sends) → all HTTP 200, free
  245.9 → 244.7 KB (Δ1.2 KB), maxblk constant 110580, 0 warnings.
  Note: `start_ssl_client: -1` / HTTP -1 has been observed occasionally on
  the weak AP — transient, resend works.
