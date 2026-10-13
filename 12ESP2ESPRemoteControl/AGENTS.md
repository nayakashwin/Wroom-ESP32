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
  (`auto`) or pinned via NVS-backed `role tx|rx`. Receiver dedups by message
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

## Verification status (2026-10-12, boards on ttyUSB0/1)

- `pio run -e default`: SUCCESS, no warnings, 57.4 % flash (757 KB) /
  15.3 % RAM (50 KB).
- Flashed to both boards; pairing in ~0.6 s, `LINK up` both sides.
- Steady stream (200 ms period, K=3): RX prints every message
  (`[rx] #N copy0/3 ... crc=ok`), ids strictly sequential, **gaps=0, late=0,
  crcErr=0, msgDrop=0, rxOverflow=0** over all runs; ~0.5 % airtime duty.
- `ping 5` under stream load: 5/5, rtt 3–9 ms. `msg` reverse direction
  (rx→tx control channel) delivered (`[msg] from peer ...`).
- Mid-stream RX reboot (EN pulse while streaming): TX session stalls, then
  **completes acked=3/3 via ACK-rewind** (e.g. `#78 slow path: acked=3/3
  retx=33 macfail=23 ms=716`), zero failed sessions, stream resumes at the
  current message ~0.8 s after the receiver boots; messages the radio could
  not deliver while the receiver was down are counted in TX `drop`
  (freshness semantics — no stale backlog replay).
- Two iterations of the reboot test drove two app fixes: the receiver now
  prints a message exactly once via a mod-64 printed-id map (so a session
  completing around our own reboot still prints, marked `(late)`), and the
  paired HELLO cadence dropped 1000→250 ms (faster re-pair, and it removed
  the first-session retransmit transient at initial pairing).
- Known micro-edge (documented, accepted): during the receiver's own reboot
  window the TX-side bitmap accounting of the one in-flight message can
  complete without a live RX print of that message — the receiver was down;
  no *live* message was ever lost in any run (sequential-id coverage 100 %).
- Heap flat at ~253 KB free across runs; WDT armed, no resets beyond the
  deliberate EN pulses.

## Files

- `src/e2e_link.h` / `src/e2e_link.cpp` — vendored 09 engine (banner lists diffs)
- `src/main.cpp` — app: roles/election, telemetry gen, TX pacing, RX dedup +
  gap accounting + `[rx]` serial output, CLI, WDT, LED
