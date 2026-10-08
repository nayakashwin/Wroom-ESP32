# 03TelegramRX — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Firmware: Telegram **receiver**. Long-polls the Bot API `getUpdates`
  (5 s hold) and prints every incoming update on the serial port.
  **No serial CLI at all** — input is not read.
  - Dependency: `lib_deps = bblanchon/ArduinoJson@^7` (platformio.ini)
  - `WIFI_SSID`, `WIFI_PASS`, `BOT_TOKEN` come from
    `_secrets/secrets.h` (gitignored — copy
    `_secrets/secrets.example.h` and fill it in; see the root `README.md`)
  - Auto-connect on boot; timeout (20 s) / link loss → auto-retry (3 s pause)
  - After first connect it calls `/deleteWebhook` once (getUpdates 409s
    while a webhook is set), then loops
    `GET /getUpdates?timeout=5&limit=10&offset=N`
  - Incoming line format:
    `TG RX: update <id> [message] from <user> (id N) chat N : <text>`
    (handles message / edited_message / channel_post / edited_channel_post;
    other update types print `type=<kind> (not displayed)`), plus
    `poll: <n> new (total <n> seen, offset <n>) heap free=… min=… maxblk=…`
    after every poll
  - LED GPIO2: heartbeat (90 ms on / 160 ms off / 90 ms on / 1.1 s pause),
    driven by a `Ticker` (10 ms) because the loop blocks ~6 s per long-poll —
    do NOT move it back into `loop()` or it will freeze during polls
- Build output: `.pio/build/default/firmware.bin`

## Memory / heap management

- `limit=10` bounds each getUpdates body (long-poll + batch drain: verified
  10-message burst drained in 6 polls).
- JSON is parsed with a static ArduinoJson `DeserializationOption::Filter`
  (`updateFilter()`): only `ok`, `update_id`, `text`, `caption`,
  `from.id/username/first_name`, `chat.id/title` survive parsing — docs stay
  small even for long texts/entities. If you print a new field, add it to
  `addMsgFilter()` or it will be `null` after filtering.
- Transients are scoped: `WiFiClientSecure` dies inside `telegramApi()`
  before JSON parse; `body` and `doc` are locals freed at the end of
  `pollUpdates()` (body declared first → doc destroyed first ✓ keep it
  that way).
- Telemetry on every `poll:` line: `free`, `min`, `maxblk`
  (`ESP.getMaxAllocHeap()`). `maxblk` must stay ~110 KB (zero
  fragmentation); a `WARN: largest free block < 40 KB - TLS may fail` line
  means TLS is at risk (`HEAP_WARN_BYTES`).
- Verified: 10-message burst → free 245.0 → 241.7 KB, maxblk constant
  110580, min 185 KB (TLS dip), no warnings. A `poll failed: HTTP 409` line
  appears if another getUpdates consumer collides (e.g. a curl during
  testing) — self-recovers on the next poll.
