# 07BlueToothTempSensor — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2); no `lib_deps`
  — the BLE stack ships with the framework.
- Build output: `.pio/build/default/firmware.bin`
- **BTHome v2 sensor reader.** Continuously scans BLE advertisements, decodes
  the service-data payload of three **LYWSD03MMC** thermometers (ATC firmware,
  BTHome mode, Xiaomi OUI `A4:C1:38`), caches the newest readings and prints a
  summary every 5 s.

### Files

- `src/bthome_parser.h` / `src/bthome_parser.cpp` — pure parser, no BLE or
  Arduino types: `parse_service_data(data, len) -> Reading`. Keeps the fields
  this project uses — packet id `0x00`, battery `0x01`, temperature
  `0x02/0x45/0x57/0x58`, humidity `0x03/0x2E`, voltage `0x0C/0x4A` — parses
  and skips every other object, and sets `truncated` when the payload runs out
  of bytes, an unknown id appears, the encryption bit (`info & 1`) is set, or
  the version `(info >> 5) & 7` is not 2. `kServiceUuid = 0xFCD2`; the Arduino wrapper strips the
  2-byte UUID, so the buffer starts at the device-info byte.
- `src/main.cpp` — scanner, MAC table, cache, printer.

### Config (top of `src/main.cpp`)

```cpp
kSensors[]  = ria "A4:C1:38:3B:E2:F6" | kitchen "A4:C1:38:D9:0C:0C"
            | basement "A4:C1:38:10:49:7C"     // same as atc.sh / bthome-mon.sh
kMaxAgeSeconds[] = {600, 1800, 600}   // Kitchen is a marginal link
PRINT_INTERVAL_MS = 5000
```

### Scan / loop

`setup()` starts an **asynchronous** scan with the callback overload
`g_scan->start(0, on_scan_complete, false)`; `loop()` restarts it if
`g_scanning` went false (this library has no `isScanning()`), then prints.
`setAdvertisedDeviceCallbacks(cb, /*wantDuplicates=*/true)` reports every
advertisement so cache ages stay fresh. Active scan, interval 50 ms /
window 30 ms.

### Output (every 5 s)

```
Rias Room  24.07C    57.21%   (90%)  rssi -101  pkts 1  age 55s
Kitchen    --  never seen
Basement   24.71C    54.26%   (97%)  rssi  -71  pkts 2  age 14s
adv 1004  bthome 36  ours 36
--
```

- `never seen` / `stale <n>s` per `kMaxAgeSeconds`; `rssi`, cumulative `packets`,
  `age` of the cached packet.
- Counters: `adv` = every advertisement, `bthome` = carrying service `0xFCD2`,
  `ours` = from a configured MAC. They separate "sensor out of range" from
  "scan is receiving nothing". Remove the `printf` in `print_summary()` if the
  line is unwanted.
- One-shot diagnostics (once per MAC, printed early after boot):
  `[info] BTHome device not configured: <mac> rssi …` for unknown BTHome
  advertisers, and `[info] xiaomi device seen: <mac> rssi … bthome yes|no` for
  **any** `A4:C1:38` advertiser (any format) — use it to tell "wrong MAC in
  `kSensors`" from "device not on the air".
- `[warn] <label>: payload truncated` = encrypted / wrong BTHome version.

## Gotchas learned the hard way

- **MAC byte order.** On this core (arduino-esp32 2.0.17 / Bluedroid)
  `BLEAddress::getNative()` keeps the address in the *same* order as the
  printed MAC and `toString()` prints `m_address[0..5]` as-is. An earlier
  version reversed the octets in `mac_to_native()`, which made every sensor
  miss and appear as `[info] … not configured` with a byte-swapped MAC
  (`7C:49:10:38:C1:A4` is really `A4:C1:38:10:49:7C`).
- `BLEAdvertisedDevice::getServiceData(int)` returns **`std::string`**, not
  `String`; service data and UUIDs are *parallel arrays* — match them by index
  (`bthome_index()`), the no-arg overloads only return the first entry.
- `BLEScan::start(duration, is_continue)` blocks the Arduino task on a binary
  semaphore (duration is in **seconds**; 0 = forever) → use the 3-arg async
  overload or `setup()` never returns.
- Reading `/dev/ttyUSB0` while `apps/arduino-serial-gui` (or a stale capture
  script) holds the port raises
  `device reports readiness to read but returned no data`. `fuser -v
  /dev/ttyUSB0` before a capture.

## Verification status (2026-10-07)

- `pio run -e default`: SUCCESS, **no warnings**, Flash 85.3 % (1117669 /
  1310720 bytes). Flashed with `sg dialout -c 'pio run -e default -t upload'`.
- 150 s serial capture: `adv 1004  bthome 36  ours 36` — every BTHome
  advertisement belonged to a configured sensor, no `[warn] truncated`
  (payloads are unencrypted BTHome v2).
- **Basement** live and updating: `24.76C 54.23% (97%) rssi -68 … pkts 2`.
- **Rias Room** live but marginal: `24.07C 57.21% (90%) rssi -91 … pkts 1`
  (one packet per ~60 s).
- **Kitchen never seen** — not in 150 s, and the OUI-wide `xiaomi device seen`
  check did not report `A4:C1:38:D9:0C:0C` either, so it is genuinely not
  advertising in range (off, dead battery, or too far), not a MAC typo and not
  a different format. Re-check once the sensor is reachable.
