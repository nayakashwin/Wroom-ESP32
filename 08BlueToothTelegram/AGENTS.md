# 08BlueToothTelegram — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2),
  `lib_deps = bblanchon/ArduinoJson@^7` (needed for the `getUpdates` filter)
- **`board_build.partitions = huge_app.csv`** — BLE + WiFi + TLS link to
  ~1.71 MB, over the 1310720-byte `default.csv` app slot. `huge_app` gives
  the app 3145728 bytes (54.4 % used); `nvs`/`otadata`/`app0` offsets are
  unchanged so `pio run -t upload` still works. Cost: no `app1` (no OTA) and
  `spiffs` shrinks to 896 KB.
- Build output: `.pio/build/default/firmware.bin`
- **07BlueToothTempSensor's BLE reader + 04TelegramRXTX's transport (send
  *and* receive) + an LED heartbeat.** The board scans the three LYWSD03MMC
  sensors forever and answers Telegram — **the sensor table is sent only when
  the owner types `/temp`**. There is no timer left in the code, so a silent
  bot means nobody asked.

### Sections of `src/main.cpp`

| section | what it does |
|---|---|
| config | WiFi/bot constants, `kSensors`, timing constants |
| heartbeat | `Ticker`-driven GPIO2 patterns |
| sensor state | `SensorState`, `adv/bthome/ours` counters, `mac_to_native()` |
| BLE | `handle_device()` → `bthome_index()` → `bthome::parse_service_data()` → `update_cache()` |
| report text | `take_snapshot()`, `build_report()` (the grid), `print_summary()` (serial) |
| telegram send | `jsonEscape()`, `htmlEscape()`, `telegramSend(text, preformatted)` |
| telegram receive | `telegramApi()`, `updateFilter()`, `handleUpdate()`, `handleCommand()`, `servicePoll()` |
| wifi + scheduler | `connectNow()`/`onConnected()` (clears the webhook once), `serviceWifi()`, `servicePoll()` |

Parser and sensor table are copies of 07's — `src/bthome_parser.{h,cpp}` and
`kSensors[]` must be edited in **both** projects.

### Commands (Telegram input, and the serial mirror of it)

- `/temp` — replies with the sensor grid (`/temp@<bot-username>` is
  accepted too, which is what Telegram delivers in groups). Only chat
  `CHAT_ID` (from `_secrets/secrets.h`) is obeyed; anyone else gets
  `TG: ignored - chat N is not the owner`. Other text is logged and dropped.
- Replies are rate-limited to one per `COMMAND_GAP_MS` (5 s).
- The serial console mirrors both inputs, so the receive path can be tested
  without the phone: typing `/temp` runs the **same** `handleCommand()`, any
  other line is sent to the channel as plain text.

### Polling

`servicePoll()` long-polls `GET /getUpdates?timeout=2&limit=10&offset=N`
every loop pass (04TelegramRXTX's proven pattern; a cycle is ~3 s).
`updateOffset` advances past every handled update, so nothing is replayed.
A heap sample is printed for every update and for every 20th poll.

### Report format (Telegram)

A fixed grid, sent as `parse_mode: HTML` inside `<pre>...</pre>` so the
columns line up — Telegram has no monospace without it. The identical plain
text is echoed to the serial console as `TG TX:`.

```
BTHome sensors  uptime 174s  ip 192.168.3.32  wifi -76dBm
sensor     temp     hum     bat    rssi  age
Rias Room  23.78C   58.40%  91%     -98  23s
Kitchen    --       --      --       --  never seen
Basement   24.40C   57.72%  97%     -60  5s
adv 982  bthome 38  ours 38
```

Header and rows share one `snprintf` format
(`%-10s %-8s %-7s %-4s %6s  %s`) so columns cannot drift; `age` is the
free-width last column (`5s`, `never seen`, `stale 720s`). The greeting
(`08BlueToothTelegram online: ssid=… ip=… rssi=…`) still goes out once on
first WiFi connect — it is not a table.

## Heartbeat (GPIO2)

Driven by a `Ticker` (10 ms) because `loop()` blocks for seconds inside TLS.
Each pattern change is logged as `[hb] …`:

| pattern | log line | meaning |
|---|---|---|
| lub-dub, 1.1 s pause | `[hb] healthy beat` | normal — **including while getUpdates polls run** |
| lub-dub, 0.25 s pause | `[hb] fast beat (busy)` | WiFi associating or a Telegram send running |
| 100 ms blink | `[hb] ALARM: loop stuck …` or `… busy call over limit …` | the loop is hung |

**A dead LED means the firmware is hung, and the board recovers on its own.**
Three layers make that true:

- `setBusy()` (`g_busy` + `g_busy_since`) — sends/connects park the loop in
  TLS for seconds, so they switch the beat instead of alarming. A busy call
  outliving `BUSY_MAX_MS` (25 s — longer than any real send 15 s / connect
  20 s) *is* an alarm, so a hang mid-TLS cannot masquerade as "busy" forever.
  `setBusy(true)` writes the start stamp **before** the flag, so the Ticker
  never pairs "busy" with a stale time.
- `g_quiet_until` — set to `start+8000` by `telegramApi()` and **left there**
  (not cleared on return): the loop only re-stamps `g_loop_ms` on its next
  pass, so clearing the guard the instant a call returns made the Ticker see
  the whole call duration as a hang. `telegramSend()` stamps `g_loop_ms`
  immediately before `setBusy(false)` for the same reason. If the loop truly
  hangs, no further poll extends the horizon, so the alarm still fires
  ~8 s later.
- **task watchdog**, `WDT_TIMEOUT_S = 30` — `esp_task_wdt_init(30, true)` +
  `esp_task_wdt_add(nullptr)` in `setup()`, fed at the top of `loop()`. A loop
  that stops for good panics (`E (…) task_wdt: Task watchdog got triggered …
  Aborting.`) and reboots (`rst:0xc`): the LED starts beating again and the
  boot banner reappears on serial. 30 s > every legitimate block (send 15 s,
  wifi connect 20 s, poll 10 s), so it never fires on healthy work.

### Hang test (verification only)

`loop()` ends with `#ifdef HANG_TEST … if (wifiState == WIFI_CONNECTED)
delay(35000); … #endif`. Build with
`PLATFORMIO_BUILD_FLAGS="-DHANG_TEST" pio run -e default -t upload` to watch
the whole story on the board: `[hb] ALARM: loop stuck (loop last tick 5008 ms
ago …)` → 100 ms blink → `task_wdt: … loopTask … Aborting.` → `rst:0xc` →
boot banner → healthy beat again. A hang taken *during* association instead
logs `ALARM: busy call over limit`. **Re-flash without the flag afterwards**
(the board otherwise reboots every ~45 s); the plain build is
`.pio/build/default/firmware.bin` at 1710429 bytes.

## Memory

- Idle, BLE+WiFi+polling running: free ~74.5 KB, `maxblk` 34804 (Bluedroid
  holds what 04/06 had free).
- During a send: free ~27 KB, `min` ~18.7 KB, `maxblk` 13300. TLS still
  succeeds, so `HEAP_WARN_BYTES = 12000` (calibrated here, not 04's 40000) —
  `WARN: send FAILED…` means it really went wrong.
- TLS sessions stay strictly sequential: `servicePoll()` finishes (parse
  included) before anything else can start a session, and `http.end()` runs
  inside `telegramApi()`/`telegramSend()` before JSON/body work.

## Gotchas (same as 07, repeated because they bite here too)

- **MAC byte order**: on this core `BLEAddress::getNative()` keeps the MAC in
  normal printed order — `mac_to_native()` copies straight through, no
  reversal (reversing makes every sensor "never seen" and prints its MAC
  byte-swapped in the `[info]` line).
- `getServiceData(int)` returns **`std::string`**; service data and UUIDs are
  parallel arrays, matched by index in `bthome_index()`.
- `BLEScan::start(duration, is_continue)` **blocks on an internal semaphore**
  (`m_semaphoreScanEnd.take()`, `BLEScan.cpp:387`) for the whole scan — always
  use the 3-arg callback overload, otherwise `setup()` never returns.
- Reading `/dev/ttyUSB0` while `apps/arduino-serial-gui` (or a stale capture
  script) holds the port raises
  `device reports readiness to read but returned no data`. Run
  `fuser -v /dev/ttyUSB0` before a capture.

## Verification status (2026-10-08)

- `pio run -e default`: SUCCESS, **no warnings**, RAM 18.4 % / Flash 54.4 %
  (1710429 / 3145728). Flashed with
  `sg dialout -c 'pio run -e default -t upload'`.
- **Heartbeat hang paths, both exercised on hardware** (`-DHANG_TEST` build,
  3 cycles captured): idle hang → `[hb] ALARM: loop stuck (loop last tick
  5008 ms ago, busy for 6827 ms)` at +5 s → `E (45804) task_wdt: Task watchdog
  got triggered … - loopTask (CPU 1) … Aborting.` → `rst:0xc (SW_CPU_RESET)`
  → boot banner → `[hb] healthy beat`. Hang taken during association →
  `[hb] ALARM: busy call over limit (loop last tick 24954 ms ago, busy for
  25005 ms)` at +25 s, same reboot. Real firmware then re-flashed.
- **Normal firmware after the restore**: 50 s capture, **0 `[hb] ALARM`**;
  `[hb] healthy beat` → `fast beat (busy)` during connect and during the
  greeting send → `healthy beat`; serial `/temp` → `local: /temp` → grid →
  `telegram send -> HTTP 200 ok (heap free=28644 min=18716 maxblk=13300)` →
  `healthy beat` again. Sensors alive in that window: Basement
  `24.51C 56.86% (97%) rssi -64 age 0s`, Rias Room `23.93C 58.23% (91%)
  rssi -88 age 8s`, Kitchen `never seen`.
- **No automatic table**: 60 s (and a 400 s soak) with nobody typing produced
  `TG TX:` only in answer to `/temp`.
- **Live Telegram input**: user sent `/temp` to the bot twice —
  both were consumed:
  `TG RX: update 36845613 [message] from <name> (id <chat>) chat <chat> : /temp`
  → grid → `telegram send -> HTTP 200 ok` (message delivered to the chat),
  `offset` advanced to 36845615 so nothing is replayed.
- **Serial mirror**: typing `/temp` in the console logs `local: /temp` and
  produces the same grid + `HTTP 200 ok`.
- **Polling soak**: 140 polls / ~7 min, `poll failed` 0 times, heap free
  74556 → 74564 (no drift), `maxblk` 34804 constant, **0 `[hb] ALARM`**
  after the guard fix (the pre-fix build fired a bogus ALARM after every
  slow poll/send).
- **Sensors in the last report**: Basement `24.40C 57.72% 97% rssi -60 age 5s`,
  Rias Room `23.78C 58.40% 91% rssi -98 age 23s`, Kitchen `never seen`
  (it did advertise once in an earlier soak at `23.77C 55.37% 68% rssi -96`
  — it is simply too marginal to show up in most reports).
