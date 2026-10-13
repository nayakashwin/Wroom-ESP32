# 12ESP2ESPRemoteControl — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2). No `lib_deps`
  (ESP-NOW + mbedtls + NVS ship with the framework), no `_secrets/` include
  (ESP-NOW needs no WiFi credentials — PSK is a serial `key` command or
  `-D E2E_PSK`).
- Build output: `.pio/build/default/firmware.bin`.
- Firmware: **same image on both boards.** Constant one-way telemetry stream
  (default: one 64 B message every 200 ms, sent K=3 times) over the vendored
  09 encrypted ESP-NOW engine, tuned for long range: 1 Mbps PHY default,
  8 s link timeout, quiet engine prints. TX role elected by higher MAC
  (`auto`), pinned via NVS-backed `role tx|rx`, or hardware-strapped at
  boot: GPIO21→GND = tx, GPIO22→GND = rx (INPUT_PULLUP straps, grounded
  beats NVS, both grounded warns and falls back; banner shows
  `role=… (strap)`). Receiver dedups by message
  id, accounts gaps, prints **every distinct message** as `[rx] #id copyN/K
  ...` plus a `[stat]` line every 30 s. Serial CLI:
  `help | status | role | period | copies | stream on|off | msg | ping | rate
  | key | verbose | stop`.
- The vendored `src/e2e_link.{h,cpp}` must stay a *minimal documented fork*
  of ../09ESP2ESPCommunication — the diff list lives in the banners of both
  files (wire magic "1E2E", NVS namespace `e12`, 1M PHY default, 8 s link
  timeout, `setQuiet()`, 250 ms paired HELLO cadence). Sync deliberately,
  never blindly.

## Two-board workflow

Two CP210x bridges: `/dev/ttyUSB0` and `/dev/ttyUSB1`. Flash both:

```bash
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB0'
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB1'
```

Serial discipline that avoids the ROM-bootloader trap (from 09's AGENTS):
after `open()`, immediately `setDTR(False); setRTS(False)`; to reset a board
pulse **EN only** (`setRTS(True)` 0.12 s → `False`, DTR released throughout).

### Hardening (2026-10-12)

- **Compile-time payload guard**: `static_assert(sizeof(Tele) <= 250 - HDR_SIZE)`
  — the build breaks if the telemetry message ever outgrows a frame.
- **Stale-silence hint**: a paired receiver that hears no telemetry for 10 s
  prints a hint every 30 s (`sender down, 'stream off' on the sender, or both
  boards pinned rx?`). Reference = later of (re)pairing time and last message,
  so it also fires when no message has *ever* arrived (the both-rx trap).
- **Honest slow-path labels**: the `[tx] ... slow path` line now says `#id` for
  telemetry sessions and `msg` for `msg`-command sessions (previously a stale
  telemetry id could label a msg session).
- **Sender-reboot resync threshold 64**: `g_rxMax - id > 64` (was `100000`,
  which never fired in realistic runs). Copies can be at most 64 ids late
  (window size), so any bigger backward jump is an id-space restart; it now
  prints `sender rebooted - resync window` and stops miscounting every
  subsequent message as `late`.
- **Retry freshness semantics documented**: an engine auto-retry regenerates
  from the *current* snapshot (newest message), not the stalled one — stale
  messages stay counted in `drop`. Deliberate, commented in `txMachine()`.
- Inherited from 09's engine: task watchdog (8 s → reboot), millis-wrap-safe
  timers (wrap logged), session auto-retry with backoff, ACK-rewind on peer
  reboot, key-mismatch downgrade protection, kInFlightMax=8 pacing.

## Verification status (2026-10-12, boards on ttyUSB0/1)

- `pio run -e default`: SUCCESS, no warnings, 57.4 % flash (757 KB) /
  15.3 % RAM (50 KB).
- Flashed to both boards; pairing in ~0.6 s, `LINK up` both sides.
- Steady stream (200 ms period, K=3): RX prints every message
  (`[rx] #N copy0/3 ... crc=ok`), ids strictly sequential, **gaps=0, late=0,
  crcErr=0, msgDrop=0, rxOverflow=0** over all runs; ~0.5 % airtime duty.
- Pin telemetry (2026-10-12): every message carries a 40-bit GPIO bitmask
  (24 header GPIOs; pulled-down inputs read 0, GPIO 2 = LED state, 34-39
  input-only/float). Verified live: bit 0 reads the boot-strap pull-up, and
  the RX `pin change: 2:0->1` line caught the TX LED going solid exactly
  when streaming started.
- `pins` CLI command (2026-10-13): one-shot local snapshot
  (`[local] pins=0x... up=Ns`), polled by the web monitor. Verified on both
  boards: TX reads `0x5` (GPIO0 pull-up + GPIO2 LED solid while streaming),
  RX reads `0x1`.
- `ping 5` under stream load: 5/5, rtt 3–9 ms. `msg` reverse direction
  (rx→tx control channel) delivered (`[msg] from peer ...`).
- Mid-stream RX reboot (EN pulse while streaming): TX session stalls, then
  **completes acked=3/3 via ACK-rewind** (e.g. `#78 slow path: acked=3/3
  retx=33 macfail=23 ms=716`), zero failed sessions, stream resumes at the
  current message ~0.8 s after the receiver boots; messages the radio could
  not deliver while the receiver was down are counted in TX `drop`
  (freshness semantics — no stale backlog replay).
- Two iterations of the reboot test drove two app fixes: the receiver now
  prints a message exactly once via a **sliding printed-id window**
  (`g_pBase`/`g_pMap`: a 64-id window whose base moves with the stream — a
  bare `id % 64` map collides, #65 with #1, which the 35-min soak caught:
  prints froze at id 64 while the radio stayed healthy), and the paired HELLO
  cadence dropped 1000→250 ms (faster re-pair, and it removed the
  first-session retransmit transient at initial pairing). The RX LED pulse
  now keys off reception, not the print gate.
- Known micro-edge (documented, accepted): during the receiver's own reboot
  window the TX-side bitmap accounting of the one in-flight message can
  complete without a live RX print of that message — the receiver was down;
  no *live* message was ever lost in any run (sequential-id coverage 100 %).
- Heap flat at ~253 KB free across runs; WDT armed, no resets beyond the
  deliberate EN pulses.

### Soak tests (2026-10-13)

Automated harness (python + pyserial, both ports, DTR/RTS discipline,
EN-only pulses) with a scheduled event sequence (ping, msg both ways,
copies/period/rate churn, stream off/on, verbose window, periodic status,
EN reboots of each board mid-stream).

- **Run 1 (35 min)**: engine itself flawless - 0 TX ABORT/RETRY, 0 LINK
  down, 0 WDT/bootloader traps, heap drift 0, ping 20/20, both `msg`
  directions delivered, ACK-rewind rescued a session stalled by an RX EN
  reboot (`acked=3/3 retx=29`). It caught a real app bug: the print-once
  map keyed on `id % 64` collided (#65 with #1, ...), so prints froze at
  id 64 while the radio kept running (`rx msgs=64` forever, RX LED dark,
  67 spurious silence hints). Fixed with the sliding printed-id window.
- **Run 2 (20 min, print-fix validation)**: 6114 distinct messages
  printed, max 3066 in one RX epoch (was frozen at 64). Firmware-side
  loss counters all zero: `gaps=0 crcErr=0 drop=0 fail=0`, 0 aborts,
  0 link flaps, 0 traps, heap drift 0/0, ping 10/11, msgs 4/4, silence
  hint fired only during the deliberate `stream off`. The harness
  reported "51 unexplained holes" - that was a *harness* line-reassembly
  bug (reader chunks split mid-line without a carry-over buffer); the
  receiver's own jump detector reported 0. Run 2 also exposed the
  `late=2446` artifact of the dead 100000 threshold (fixed, see
  Hardening).
- **Focused resync check (post-fix)**: EN-pulse the TX mid-stream ->
  `id went 146 -> 1, sender rebooted - resync window`, then 211
  strictly sequential ids, `late=0 gaps=0 crcErr=0`, TX booted exactly
  twice (port open + EN). PASS.
- Harness pitfalls (for future runs): keep read chunks in a line buffer
  before `splitlines()`; `pgrep -f <name>` inside a shell whose own
  cmdline contains `<name>` matches itself (poll by pid instead);
  `/tmp` harness scripts die with a reboot - the summary must be copied
  into this file, not assumed durable.

## Files

- `src/e2e_link.h` / `src/e2e_link.cpp` — vendored 09 engine (banner lists diffs)
- `src/main.cpp` — app: roles/election, telemetry gen, TX pacing, RX dedup +
  gap accounting + `[rx]` serial output, CLI (`pins` included), WDT, LED
- `web/pinmon.py` — stdlib HTTP + pyserial bridge: DTR/RTS discipline,
  line-buffered readers (carry-over buffer — mid-chunk splits are a real
  hazard), `pins` + `adc` poller (0.5 s), `[rx]` OTA parser (pins + the
  6-channel ADC1 list, 12-firmware lines without `adc` tolerated),
  analog-jump events at a 128-count threshold (13's radio-coupled ±110
  bounce would otherwise flood the log — the firmware's own deadband-32
  `adc change` lines are deliberately ignored), `/api/state` JSON
- `web/index.html` — dashboard: two NodeMCU-32S (38-pin) SVGs with live
  per-GPIO pills/pads (40-bit-safe bit test: JS `>>` wraps at 32, so
  IO32+ need `bit()`, not `>>`), RX-card pills split in two halves (left =
  receiver's read, right = transmitted value as received — orange ring on
  mismatch, independent of the local port), header labels + colored
  function badges transcribed from Waveshare's NodeMCU-32S pinout image
  (OD/IE state badges omitted), OTA-vs-local comparison, analog chip strips
  (TX live reads; received values + δ badges on the RX card), event log;
  polls 150 ms
