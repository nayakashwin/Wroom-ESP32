# 09ESP2ESPCommunication — encrypted point-to-point ESP32↔ESP32 link

One firmware, two boards. They find each other over WiFi (ESP-NOW), pair with a
shared key, and then exchange **encrypted, acknowledged, lossless** data at the
fastest rate this radio path allows — no router, no WiFi credentials, no server.

Measured on this bench (two NodeMCU v3 boards, ~30 cm apart, channel 6):

| metric | value |
|---|---|
| sustained payload throughput | **≈ 1.1–1.5 Mbit/s** (512 KB delivered, 0 loss) |
| round-trip latency (`ping`) | 1–11 ms (min/avg 1–4/4–7 ms) |
| delivery | every frame ACKed at MAC *and* app level, retransmitted until acked |
| integrity | AES-CCM-128 MIC per frame + end-to-end pattern check (`verify=ok`) |

Same firmware on both boards — roles are symmetric; either side can send,
both directions can carry traffic at the same time.

---

## 1. Quick start

```bash
cd 09ESP2ESPCommunication
pio run -e default                                        # build (~749 KB flash / ~51 KB RAM)
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB0'
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB1'   # second board
sg dialout -c 'pio device monitor -b 115200 -p /dev/ttyUSB0'
```

On the monitor you will see both boards pair (`LINK up …`) and, 1.5 s later,
the board with the **higher MAC** automatically runs a 128 KB benchmark:

```
link: esp-now ch=6 frame<=250 payload<=232 psk-fp=778F547A(default (E2E_PSK)) rate=WIFI_PHY_RATE_54M rc=0/0
09ESP2ESPCommunication online: encrypted esp-now p2p link, 115200 baud
waiting for peer (both boards: same channel, same key) - type 'help'
LINK up peer=40:91:51:FC:D7:7C fp=778F547A frame<=250 payload<=232
TX s=1 bench frames=565 payload<=232
TX s=1 73% 413/565 retx=0 mac=0 q=0
TX DONE s=1 bench acked=565/565 retx=0 macfail=0 bytes=131080 ms=765 rate=1.37Mbps
```

and on the other board:

```
LINK up peer=A8:42:E3:C8:30:F0 fp=778F547A frame<=250 payload<=232
RX s=1 total=565 payload<=232
RX DONE s=1 frames=565/565 dup=0 verify=ok bytes=131080 ms=755
```

Disable the auto-benchmark with `-D E2E_AUTO_BENCH=0` (see §5).

## 2. Serial console (115200, both boards)

| command | meaning |
|---|---|
| `help` | command list |
| `status` | link state, peer MAC, key fingerprint, counters, last TX/RX results, heap |
| `tx [KB]` | encrypted throughput bench (default 128 KB, max 16384) |
| `stop` | abort the running stream |
| `ping [n]` | round-trip latency, n pings (default 5, max 50) |
| `msg <text>` | send text to the peer **reliably** (auto-chunked if > 232 bytes) |
| `rate [name]` | PHY rate at runtime (`1m`…`54m`, `mcs0`…`mcs7s`), saved to NVS; no arg = list |
| `key <psk>` | set a new pre-shared key (8–63 chars), stores it in NVS and reboots — run the *same* command on both boards |

LED (GPIO2): blink 1 Hz = waiting for peer, solid = link up,
fast blink = stream in flight.

Diagnostic output lines:

| line | meaning |
|---|---|
| `LINK up peer=<mac> fp=<fp>` | paired with an encrypted peer (fingerprints match) |
| `LINK down (no frame …)` | peer went silent for 3 s |
| `LINK down: key changed on <mac>` | peer's HELLO now carries a different key fingerprint |
| `WARN: key mismatch …` | a board is nearby but the keys differ (pairing refused) |
| `TX s=<id> … / TX DONE …` | sender progress / result (`retx` = retransmits, `mac` = MAC-level failures, `q` = queue refusals) |
| `RX s=<id> … / RX DONE …` | receiver progress / result (`dup` = duplicates received, `verify` = payload pattern check) |
| `[msg] from <mac> …` | a text message was received and reassembled |

## 3. How it works

```
T_HELLO ── broadcast, plaintext ──> discovery: "I am <mac>, my key fingerprint is <fp>"
   both sides hear each other, fingerprint matches
   └─ each registers the other as an ESP-NOW peer with encrypt=true (LMK)
T_DATA / T_TEXT ── encrypted unicast ──> payload frames (seq/total inside)
T_ACK   ── encrypted unicast ──> bitmap of received frames (base + 64-bit map)
T_PING / T_PONG ── encrypted unicast ──> RTT measurement
```

- **Keys.** A passphrase (NVS `key` command > `-D E2E_PSK` build flag > built-in
  default) is expanded with SHA-256 into a 16-byte PMK and a 16-byte LMK. The
  LMK is what ESP-NOW's **AES-CCM-128** hardware-assisted encryption uses; both
  boards must derive the same LMK or the radio frames fail their MIC and are
  dropped silently. A 32-bit FNV fingerprint of the LMK travels in the HELLO so
  mismatched keys are *reported* ("WARN: key mismatch") instead of looking like
  radio silence.
- **Reliable transfer.** Every stream is numbered (`stream id`, `seq`, `total`).
  The receiver keeps a 64-frame sliding window and answers bitmap ACKs every
  ≤ 8 ms / 32 frames while a stream is active (and for 2 s after completion, so
  a lost final ACK still gets re-answered). The sender never transmits past
  `ackBase+63`, retransmits gaps after 50 ms, and only keeps 8 attempts in the
  MAC queue (deeper queues measured as pure duplicate storms). Every ACK is
  taken as the receiver's *current* truth — a peer that rebooted mid-stream
  reports an empty window, the sender rewinds and retransmits, and the stream
  completes without restarting (verified: 4 MB stream survived a receiver
  reboot, ~4 100 retransmits, `verify=ok`). Delivery is therefore
  **at-least-once**. A stream ends when every frame is ACKed (`TX DONE …
  acked=n/n`) or aborts after 5 s without progress.

### Ruggedization (all verified on the bench)

| mechanism | behaviour |
|---|---|
| task watchdog | `loop()` subscribed with an 8 s timeout; a hang panics and reboots the board (`wdt: armed` at boot, proven with a deliberate hang test → `task_wdt: loopTask` → `abort()` → clean reboot) |
| mid-stream peer reboot | sender rewinds its window to the receiver's state and completes the stream (see above) |
| session auto-retry | if a stream still dies (link lost / no ACK progress), it is restarted automatically up to 2× (1 s, 2 s backoff), waiting up to 60 s for the peer to return; then `TX ABORT … retries exhausted` |
| wrap-safe timers | all intervals use unsigned difference arithmetic; the 49.7-day `millis()` rollover is logged and counted (`millis-wraps` in `status`) |
| runtime rate adaptation | `rate <name>` changes the PHY rate without reflashing and persists it in NVS — drop to `11m`/`2m` when interference or distance degrades the link |
- **Framing.** 18-byte header + payload, ≤ 250 bytes per ESP-NOW frame
  (`payload ≤ 232`). Multi-frame streams are how bigger data is carried.
- **Supervision.** Any frame from the peer refreshes a 3 s timer; HELLOs
  continue at 1 s. A fingerprint change or radio silence takes the link down
  and it re-pairs automatically when the peer returns.

## 4. Sending and receiving custom data

The link layer (`src/e2e_link.h`) is data-agnostic — payloads are bytes it
neither parses nor stores. Three ready-made paths, from simplest to most
flexible:

### 4.1 Small messages — `sendBlob` / `onMessage` (what `msg` uses)

```cpp
#include "e2e_link.h"

// send (≤ 1024 bytes, chunked over the air if > payloadMax(), reliable):
e2e::sendBlob((const uint8_t*)"open valve 5", 12);

// receive, in loop() context:
e2e::onMessage([](const uint8_t src[6], uint32_t seq, uint32_t total,
                  const uint8_t* data, uint16_t len) {
  // seq/total identify the chunk; reassemble by offset seq * e2e::payloadMax()
});
```

### 4.2 Periodic telemetry — your own generator (what `tx` uses)

`startStream` sends `frames` frames whose payloads your callback builds on
demand (so nothing is buffered — a 10 KB/sensor stream costs no RAM):

```cpp
struct __attribute__((packed)) Telemetry {   // fixed-size records travel best
  uint32_t seq; float temp; uint16_t hum; uint16_t mv;
};

static Telemetry g_tel;

static uint16_t telGen(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen) {
  g_tel.seq   = seq;
  g_tel.temp  = readTemperature();
  g_tel.hum   = readHumidity();
  g_tel.mv    = analogReadMilliVolts(34);
  memcpy(out, &g_tel, sizeof g_tel);        // must stay ≤ e2e::payloadMax() (232)
  return sizeof g_tel;
}

// one record per frame, forever* — call again when the previous finished:
if (!e2e::streamActive() && every_250ms())
  e2e::startStream(e2e::T_TEXT, telGen, nullptr, 1, "telemetry");
```

On the receiver install `onMessage` and cast the payload back to `Telemetry`
(check `len == sizeof(Telemetry)`). Because delivery is reliable you can treat
it as a pipe: nothing is silently lost, and duplicates are already filtered.

### 4.3 Bulk data / files — stream from a buffer

```cpp
static uint16_t bufGen(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen) {
  const uint8_t* blob = (const uint8_t*)ctx;          // e.g. 100 KB in flash
  uint32_t off = seq * maxLen;
  uint32_t n = kBLOB_LEN - off;
  if (n > maxLen) n = maxLen;
  memcpy(out, blob + off, n);
  return n;
}
e2e::startStream(e2e::T_TEXT, bufGen, blob, (kBLOB_LEN + e2e::payloadMax() - 1)
                                          / e2e::payloadMax(), "blob");
```

Reassemble on the peer exactly like `main.cpp`'s `onMsg()` does
(`memcpy(rx + seq*e2e::payloadMax(), data, len)` until `seq == total-1`).

### Rules of the road for custom data

| constraint | value / reason |
|---|---|
| payload per frame | `e2e::payloadMax()` = 232 B (250 B ESP-NOW cap − 18 B header) |
| one TX stream at a time per board | check `e2e::streamActive()`; queue or skip |
| both directions simultaneously | supported (each board has its own TX and RX state) |
| `PayloadGen` must be deterministic | retransmissions regenerate the payload from `seq` |
| receiver gets each frame once | bitmap dedup; your handler sees no duplicates |
| delivery semantics | **at-least-once**: a mid-stream rewind or an auto-retried stream (new stream id) can redeliver frames the app already saw — dedup by content/seq in the handler if that matters |
| frame type | `T_TEXT` for app data (no built-in verification); `T_DATA` only if you reuse the bench pattern |
| TEXT handler throughput | chunks are handed to `onMessage` via a 16-slot FIFO; if your handler is slow, surplus chunks are counted in `msgDroppedCount()` |

## 5. Configuration knobs (build flags in `platformio.ini`)

| flag | default | meaning |
|---|---|---|
| `E2E_CHANNEL` | `6` | WiFi channel 1–13; **must match on both boards**. Pick a quiet one. |
| `E2E_FRAME_MAX` | `250` | frame size incl. header, ≤ 250 (ESP-NOW v1 hard limit) |
| `E2E_PSK` | `"esp32-ptp-default-key-CHANGE-ME"` | passphrase; prefer the `key` command (NVS) so it never exists in source |
| `E2E_PHY_RATE` | `WIFI_PHY_RATE_54M` | PHY rate token — or skip it and use the runtime `rate` command (NVS-persisted, wins over the build flag after first use) |
| `E2E_AUTO_BENCH` | `1` | run the 128 KB demo benchmark once after pairing |
| `E2E_AUTO_BENCH_KB` | `128` | size of that benchmark |

Reliability/latency tuning lives in `src/e2e_link.cpp` (`kAckPeriodMs`,
`kAckFrames`, `kRetxMinMs`, `kWindow`, `kInFlightMax`) with measured comments —
the current values are the sweet spot on this hardware; change one at a time
and re-run `tx 512` on both boards to compare.

## 6. Security model (and its limits)

- Payload confidentiality + integrity: **AES-CCM-128** keyed by the LMK,
  enforced by the WiFi MAC/ESP-NOW stack — not by application code.
- Key material never leaves the boards except as the 32-bit fingerprint in the
  plaintext HELLO (so mismatches are diagnosable). That fingerprint *does* let
  an off-air observer test passphrase guesses offline — use a long random
  passphrase for real deployments (`key` command).
- Stale/lost frames: per-stream `seq` numbering + bitmap windows mean replays
  of old frames land in already-acked territory and are counted as `dup`,
  never delivered twice.
- No forward secrecy, no per-frame signatures, exactly **one peer** (first
  fingerprint-matching board wins). If you need multi-party, pair each peer
  with its own LMK via `esp_now_mod_peer` — the engine stays the same.
- The discovery HELLO is plaintext by ESP-NOW design (broadcast cannot be
  encrypted); it leaks only MACs + fingerprint.

## 7. Performance notes (what limits it, what to tune)

- The ~1.7 ms/frame cost is the **ESP-NOW driver path**, not airtime: at
  54 Mbit/s a 250 B frame is ~40 µs on air. Measured: PHY rate 11M ≈ 54M ≈
  MCS7 (±3%), and **encryption is free** (an unencrypted test build was *not*
  faster). So: don't chase rate flags for speed; chase them for range.
- Bigger frames = more payload per driver round trip: 240→250 B frames bought
  +4.5%. 250 is the v1 ceiling (ESP-NOW v2, 1.4 KB frames, needs
  arduino-esp32 3.x / IDF 5 — a different platform package than this repo's).
- ACK overhead at the default cadence is ~5%: raise `kAckFrames` if your link
  is clean, lower `kAckPeriodMs` on lossy links for faster gap recovery.
- Latency of a single small message ≈ one driver round trip ≈ 2–9 ms; the
  64-frame window keeps full throughput over that RTT.
- Boards far apart / marginal RSSI: lower the PHY rate — at runtime with
  `rate 11m` (or `2m`), no reflash needed — and expect `retx`/`mac` to climb:
  throughput degrades gracefully, delivery does not.

## 8. Troubleshooting

| symptom | cause / fix |
|---|---|
| `WARN: key mismatch` on both boards | keys differ — `key <same-psk>` on both |
| no `LINK up` at all | channels differ (`E2E_CHANNEL`), boards out of range, or a third ESP-NOW device on the channel — lower the PHY rate, check `status` |
| `TX ABORT … no ack progress` | peer rebooted mid-stream or link dropped; the next stream starts clean |
| `q` counter > 0 in `TX` lines | `esp_now_send` queue refusals — harmless unless huge (means the stream outpaces the MAC; the engine backs off automatically) |
| `msgDrop` in `status` > 0 | your `onMessage` handler is too slow for the chunk rate — buffer or streamline it |
| `verify=FAIL` in `RX DONE` | payload corruption got past the MIC — should never happen; report it |
| one board silent after `key` on the other | expected: `LINK down: key changed` appears once, then re-pair when you set the same key |

## 9. Files

| file | role |
|---|---|
| `src/e2e_link.h` | public link API: types, callbacks, config knobs (documented) |
| `src/e2e_link.cpp` | WiFi/ESP-NOW bring-up, keys, discovery/pairing, ARQ engine, ACKs, ping/pong |
| `src/main.cpp` | demo application: serial CLI, auto-bench, message reassembly, LED |
| `platformio.ini` | env `default` + documented build flags (no secrets needed) |
