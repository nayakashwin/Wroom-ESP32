# 09ESP2ESPCommunication — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2). No `lib_deps`
  (ESP-NOW + mbedtls + NVS ship with the framework), no `_secrets/` include
  (ESP-NOW needs no WiFi credentials — the pre-shared key is a serial `key`
  command or `-D E2E_PSK`, see README §5).
- Build output: `.pio/build/default/firmware.bin` (~749 KB flash / ~51 KB RAM).
- Firmware: **same image on both boards.** Encrypted (AES-CCM-128 via ESP-NOW
  LMK) point-to-point link with a 64-frame bitmap-ARQ reliable-transfer engine
  on top. Serial CLI: `help | status | tx [KB] | stop | ping [n] | msg <text>
  | key <psk>`. Auto-runs a 128 KB benchmark on first pairing (higher-MAC
  board sends). Full docs: `README.md` in this folder.

### Two-board workflow

Two CP210x bridges on this machine: `/dev/ttyUSB0` and `/dev/ttyUSB1`
(`/dev/serial/by-path/` tells them apart if needed). Flash both:

```bash
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB0'
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB1'
```

Capture both at once (pyserial lives in `~/.platformio/penv/bin/python`):
open both ports, DTR/RTS-toggle reset, read ~15 s — expect `LINK up` on both,
then `TX DONE … acked=n/n rate≈1.0Mbps` on one and `RX DONE … verify=ok` on
the other.

### Files

- `src/e2e_link.h` — public API (config knobs documented there).
- `src/e2e_link.cpp` — wifi/esp-now bring-up, SHA-256 key derivation,
  HELLO discovery + fingerprint pairing, wire format (18 B header),
  rx sliding window + bitmap ACKs, tx window + paced retransmit, ping/pong.
- `src/main.cpp` — CLI, auto-bench (MAC-election), message reassembly, LED.

### Config

`E2E_CHANNEL=6`, `E2E_FRAME_MAX=250` (ESP-NOW v1 cap), `E2E_PSK`,
`E2E_PHY_RATE=WIFI_PHY_RATE_54M`, `E2E_AUTO_BENCH=1/KB` — all overridable via
`build_flags` (platformio.ini). Runtime key: `key <psk>` → NVS namespace `e2e`
(read back with plain `nvs_open`, **not** `Preferences::begin` — see gotchas).

## Gotchas learned the hard way

- **Don't queue deep into `esp_now_send`.** First version allowed 64 in-flight
  attempts: per-attempt cost is ~1.7 ms (driver, not airtime), so the queue
  added ~100 ms latency, the 25 ms retransmit timer fired while frames were
  still queued → duplicate storm (839 dups for 591 frames, 0.05 Mbps). Cap is
  now `kInFlightMax=8` with `kRetxMinMs=50`: 0 dups, 1.1–1.5 Mbps.
- **Encryption is free** — an experiment with `encrypt=false` measured *slower*
  (0.74 vs 1.04 Mbps, within noise). Never trade it away "for speed".
- **PHY rate barely matters here** (11M ≈ 54M ≈ MCS7 within ±3%): the ESP-NOW
  driver path is the bottleneck. Lower the rate only for range.
- **`Preferences::begin("e2e", true)` on a fresh flash logs**
  `[E][Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND` — read the
  optional PSK with `nvs_open(NVS_READONLY)` instead and the boot log stays
  clean.
- **Plaintext HELLOs must not count as link supervision once the peer's key
  changed** — the frames "flow" but encrypted data is silently dropped. The
  recv callback checks the HELLO fingerprint even when paired and forces
  `LINK down: key changed` (verified with the `key test-key-mismatch-123`
  dance on one board).
- `memcmp(own, peer, 6)` — the 3-arg form; forgetting `size_t` is a compile
  error, not a warning.
- RX window is safe by construction: the sender never transmits past
  `ackBase+63`, so the receiver can never see a frame ≥ `base+64` (that path
  only increments `rxOverflowCount()`, which should stay 0).

## Verification status (2026-10-09)

- `pio run -e default`: SUCCESS, no warnings, 57.1 % flash / 15.5 % RAM.
- Flashed to both boards; auto-bench on pairing: `TX DONE s=1 bench
  acked=565/565 retx=0 macfail=0 bytes=131080 ms=765 rate=1.37Mbps` /
  `RX DONE s=1 frames=565/565 dup=0 verify=ok` (128 KB, channel 6, 54M).
- Manual `tx 512`: `acked=2260/2260 … rate=1.47Mbps`, 0 dups, verify=ok.
- `ping 10` idle: min/avg/max = 1–4/4–7 ms; `msg` delivered both directions
  (`[msg] from <mac> … "…"`), multi-KB single-frame-per-chunk path exercised
  via `tx` (2260 frames) and `msg` (1 frame).
- Key-mismatch: `WARN: key mismatch` both sides, `LINK down: key changed` on
  the peer, auto `LINK up` after restoring the same key (fp `778F547A`).
- Both boards currently run the default PSK (`status` shows
  `psk source=nvs` on ttyUSB1's board with the default string stored —
  functionally identical; erase NVS or re-`key` to change).

### 2-hour soak (2026-10-09, harness `/tmp/opencode/soak.py`, logs
`/tmp/opencode/soak/`)

60 cycles × 120 s: alternating-direction 64 KB benches, full-duplex benches
every 6th cycle, `msg` both ways + `ping 5` every cycle; reboots B @ 10/30/50,
A @ 20/40, and B killed 3 s into a 4 MB stream @ 45. Results:

- **0 unexpected reboots, 0 watchdog/panic/backtrace lines, 0 ERR/WARN** —
  boots were exactly the 8 planned ones (3 A + 5 B).
- **66/66 bench sessions fully acked**, 184/184 RX sessions complete with
  `verify=ok`; **108/108 msgs delivered**, **270/270 pings answered**
  (RTT 1–16 ms).
- Throughput avg 1.18/1.28 Mbps (A/B), peaks 1.61/1.90 Mbps; min 0.56–0.65 =
  full-duplex cycles where both boards share the air (expected).
- The only abort is the planned mid-stream kill: sender aborted cleanly after
  5 s ("no ack progress"), both boards fully operational in the next cycle.
- Heap drift over 2 h: A +12 B, B 0 bytes — no leak. `rxOverflow=0`,
  `msgDrop=0`, retx ≈ 2.4 per session.
- Peer reboot recovery: typically **no LINK down at all** (reboot faster than
  the 3 s supervision window); traffic resumes within ~2–4 s of reset.

Soak harness note: its "re-pair TIMEOUT" labels are a checker artifact — it
waits for a new `LINK up` on *both* boards, but the surviving board keeps the
link up when the peer returns within 3 s, so it never re-prints one. Trust the
per-cycle traffic deltas (and this note) over that flag.
