# 04TelegramRXTX — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Firmware: combined Telegram **sender + receiver** (02TelegramTX +
  03TelegramRX in one). **The only serial interaction: type a line + Enter →
  it is sent to the channel.** No other commands, no NVS.
  - Dependency: `lib_deps = bblanchon/ArduinoJson@^7` (platformio.ini)
  - `WIFI_SSID`, `WIFI_PASS`, `BOT_TOKEN`, `CHAT_ID` come from
    `_secrets/secrets.h` (gitignored — copy
    `_secrets/secrets.example.h` and fill it in; see the root `README.md`)
  - Auto-connect on boot; timeout (20 s) / link loss → auto-retry (3 s pause)
  - On first connect: `/deleteWebhook` once, then sends
    `04TelegramRXTX online: ssid=... ip=... rssi=... dBm`, then long-polls
    `GET /getUpdates?timeout=2&limit=10&offset=N` forever
  - Loop order per pass: serial lines → (state checks) → greeting once →
    poll. A typed line waits in the UART buffer while a poll blocks
    (~3.5 s worst case, `POLL_TIMEOUT_S = 2` keeps it short) and is sent
    before the next poll starts. Send and poll TLS sessions are strictly
    sequential — never concurrent.
  - Incoming line format:
    `TG RX: update <id> [message] from <user> (id N) chat N : <text>`
    plus `poll: <n> new (total <n> seen, offset <n>) heap free=… min=…
    maxblk=…` after every poll
  - LED GPIO2: heartbeat (90 ms on / 160 ms off / 90 ms on / 1.1 s pause),
    driven by a `Ticker` (10 ms) — the loop blocks seconds at a time, do
    NOT move it into `loop()`
- Build output: `.pio/build/default/firmware.bin`

## Memory / heap management

- Outflow (send): `printf("%.Ns", body.c_str())` — never `substring()`;
  result line carries heap `free/min/maxblk`. Transients (`url`/`payload`/
  `client`/`http`/`body`) freed at end of `telegramSend()`.
- Inflow (poll): `limit=10` bounds the body; JSON parsed with a static
  ArduinoJson `DeserializationOption::Filter` (`updateFilter()`) keeping
  only `ok`, `update_id`, `text`, `caption`, `from.id/username/first_name`,
  `chat.id/title` — add new printed fields to `addMsgFilter()` or they
  parse as `null`. `WiFiClientSecure` dies inside `telegramApi()` before
  JSON parse; `body` declared before `doc` → doc destroyed first ✓.
- `HEAP_WARN_BYTES = 40000`: a `WARN: largest free block < 40 KB - TLS may
  fail` line means fragmentation threatens TLS. `maxblk` should sit at
  ~110 KB; sequential (never overlapping) TLS sessions keep it there.
- Verified patterns from 02/03: 8 consecutive sends → Δ1.2 KB free,
  maxblk constant; 10-message burst → free −3.3 KB, maxblk constant.
