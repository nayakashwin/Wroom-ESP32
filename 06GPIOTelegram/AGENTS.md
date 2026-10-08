# 06GPIOTelegram — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2),
  `lib_deps = bblanchon/ArduinoJson@^7`
- Build output: `.pio/build/default/firmware.bin`
- **GPIO console over Telegram** = the GPIO core of `05GPIO` + the
  transport of `04TelegramRXTX`. One parser (`execCommand()`) is fed by
  two transports: incoming Telegram messages *and* the serial console.
- Config in `_secrets/secrets.h` (gitignored — copy
  `_secrets/secrets.example.h` and fill it in; see the root `README.md`):
  `WIFI_SSID`, `WIFI_PASS`, `BOT_TOKEN`, `CHAT_ID` (identical set to 04).
  `CHAT_ID` is the owner chat, and the only one allowed to drive the GPIO.

### Commands (identical syntax on Telegram and serial; leading `/` optional)

`help`/`start`, `status`/`pins`, `stat`, `mode <p> <in|out|pu|pd>`,
`on <p>`, `off <p>`, `w <p> <0|1>`, `read <p>`, `adc <p>`,
`pwm <p> <Hz> <0-255>`, `blink <p> <on_ms> <off_ms>`, `tog <p>`,
`stop <p>`, `all <0|1>`.

Covered GPIOs (26): `0-5 12-19 21-23 25-27 32-33 34-36 39`.
Refused: `6-11` (flash), `37/38`, `EN`. Input-only (`34 35 36 39`) rejected
for `mode out` / `pwm` / `on` / `off`. Straps `0 2 5 12 15`, UART0 `1 3`,
UART2 `16/17`, DAC `25/26` print a `note:` when first driven.

### Loop order per pass (unchanged from 04)

`handleSerial()` → wifi state machine → greeting once → `pollUpdates()`
(blocks ~3.5 s: `POLL_TIMEOUT_S = 2` + TLS handshake) → `serviceBlink()`.
Send and poll TLS sessions are strictly sequential, never concurrent.

- Heartbeat on GPIO2 via `Ticker` (10 ms): **skipped while `st[2].mode != -1`**,
  so `/mode 2 out` hands the LED over to the user.
- `blink` is driven from `loop()`, so it *stalls during a poll* (up to ~3.5 s
  gaps); hardware `pwm` (LEDC) keeps running through polls.
- Updates are polled with `updateOffset = 0` on every boot, i.e. messages that
  arrived while the board was offline are executed after the next boot.

### Security

`authorized()` accepts only `CHAT_ID` — numeric id, or `chat.username` when
`CHAT_ID` starts with `@`. Anything else is logged as
`TG: ignored - chat is not the owner` and produces no reply.

### Reply path

`pollUpdates()` collects replies into `String reply` **inside** the scope that
holds `body`/`doc`, then both are destroyed before `telegramSend(reply)` runs —
never parse or reply while the poll's JSON/TLS buffers are still resident.
Serial shows `TG TX: <reply>` for every reply that was sent.

## Verification status (2026-10-07)

- `pio run -e default`: SUCCESS, **no warnings**, RAM 14.7 % / Flash 73 %.
- Flashed; serial console: 20/20 scripted checks pass
  (`/tmp/opencode/console_test.py`).
- Boot: WiFi connect → `webhook cleared` → greeting `HTTP 200 ok` to the
  owner chat from `secrets.h` (proves token + chat id + send path).
- Poll loop: 57 polls / 200 s, heap free drift **+0**, `maxblk` constant
  `110580`, no `poll failed` / `WARN` lines.
- **Not yet verified live:** the Telegram *receive* path (a real message from
  the owner must arrive while the board is up) — confirm by sending `/status`
  to your bot (the username you gave @BotFather) and watching for
  `TG RX: ... : /status` + `TG TX:` in the serial log.
