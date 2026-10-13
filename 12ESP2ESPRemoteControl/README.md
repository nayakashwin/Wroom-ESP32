# 12ESP2ESPRemoteControl — long-range reliable ESP-NOW message stream

One ESP32 **constantly streams messages** to the other over encrypted
ESP-NOW; **every distinct message received is printed to the serial
monitor**. Built for **reliability at long distances, explicitly over
speed**: the link budget, the redundancy and the retransmission engine are
all biased toward "the message arrives, eventually, intact".

Same firmware on both boards. The transmitter is elected automatically
(higher MAC address sends), or pinned with `role tx` / `role rx`.

## The reliability stack (why it is built this way)

Five independent layers; each one can fail without losing the message:

| Layer | Mechanism | What it buys at long range |
|---|---|---|
| PHY | **1 Mbps DSSS** (`WIFI_PHY_RATE_1M_L`, the fork's default) | Receiver sensitivity ~-98 dBm vs ~-74 dBm at 54M — **~24 dB more link budget**, several times the range. 802.11 drops any frame that fails FCS, so bit-level payload FEC cannot recover anything ESP-NOW never delivers; spending bits on the slowest, most robust modulation instead is the honest "error correction" available to us. |
| Link crypto/integrity | AES-CCM-128 (ESP-NOW LMK, SHA-256-derived from PSK) + 802.11 FCS | Any corrupted frame is dropped by the MIC long before the app sees it — corruption can never be *silently* delivered. |
| Redundancy | Every message is sent **K = 3 times** as distinct frames (id + copy index) | Any single copy delivers the message — no waiting for a retransmit round trip. Copies ride different moments in time (fades, interference bursts). `copies 1..4` at runtime. |
| ARQ | The soak-proven 09 engine: 64-frame sliding window, **bitmap ACKs every ≤8 ms**, paced retransmission (≥50 ms), ACK-rewind when the peer reboots mid-stream, session auto-retry (2×, 1 s/2 s backoff, 60 s give-up) | At-least-once delivery even when *all* copies of a message are lost: the sender retransmits until the receiver's bitmap confirms. 2 h full-duplex soak: 4587/4587 sessions acked, 0 aborts (see ../09ESP2ESPCommunication/AGENTS.md). |
| Supervision | HELLO beacons (250 ms seek / 1 s paired), 8 s link timeout, task watchdog (8 s → reboot), millis-wrap-safe timers | A deaf or hung board recovers by itself; pairing survives long-range fades without churn. |

On top sits application-level verification: per-message **CRC32**, an id
window with dedup and **gap accounting** — so delivery is not just claimed,
it is *measured* (`gaps=` should stay 0; a gap means all K copies plus all
retransmissions failed over seconds).

Airtime budget: 200 ms period × 3 copies × 82 B frames at 1 Mbps ≈ **0.5 %
duty cycle** — enormous headroom for retransmissions, which is exactly the
point. Goodput needed is ~2.6 kbit/s; the 1M link sustains ~700 kbit/s
worst-case and ~1 Mbps typical.

## Wiring

Two NodeMCU v3 (ESP-WROOM-32) boards, no external components. USB-UART
bridges on `/dev/ttyUSB0` and `/dev/ttyUSB1`. For actual long-range use:
the on-board PCB antenna is the limiting factor — a board with a proper
external antenna on the same modulation goes much further.

## Build / flash / watch

```bash
cd 12ESP2ESPRemoteControl
pio run -e default                                        # build
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB0'
sg dialout -c 'pio run -e default -t upload --upload-port /dev/ttyUSB1'
```

Open both monitors at 115200. On pairing:

- both print `LINK up peer=... fp=...`
- the elected TX board silently streams (LED solid; anomalies print)
- the RX board prints **one line per message** and pulses the LED:

```
[rx] #1 copy0/3 up=0s heap=212980B "remote-control telemetry" crc=ok
[rx] #2 copy0/3 up=0s heap=211234B "remote-control telemetry" crc=ok
...
[stat] up=30s role=rx link=up | rx msgs=149 copies=447 dup=0 late=0 gaps=0 crcErr=0 | ...
```

`copy0/3` = delivered by the first of three copies; if you ever see
`copy1/3` or `copy2/3` first, the redundancy just saved a retransmission
(`firstNotCopy0` in `status` counts it).

## Serial commands

```
help | status | role [auto|tx|rx] | period [100..60000 ms] | copies [1..4] |
stream on|off | msg <text> | ping [n] | rate [name] | key <psk> | verbose | stop
```

- `role` — show/pin the role; `auto` (default) = higher MAC transmits. Both
  boards pinned `tx` is legal: they stream to each other simultaneously.
- `period`, `copies` — stream cadence and redundancy factor (NVS-persisted).
- `msg` — reliable text the *other* way (the control channel of the remote
  control): lands on the peer's serial as `[msg] from peer ...`.
- `ping` — RTT diagnostics under stream load (never blocks on the stream).
- `rate` — PHY rate ladder; `1m` is the long-range default, stepping up buys
  throughput at the cost of sensitivity.
- `key` — change the PSK on both boards (NVS namespace `e12`, then reboots).
- `verbose` — also print `[rx+]` lines for duplicate copies.

## Message format

64-byte packed struct per frame, K frames per message:

```
id:u32 | copy:u8 | copies:u8 | rsv:u8 | uptimeMs:u32 | heapFree:u32 |
crc32:u32 (over the preceding 19 bytes) | text:char[45]
```

The engine retransmission regenerates frames deterministically from the
snapshot captured at message creation, so retries are byte-identical.

## Configuration knobs

`platformio.ini` build flags (both boards must match): `E2E_CHANNEL=6`,
`E2E_FRAME_MAX=250`, `E2E_PSK="..."`, `E2E_PHY_RATE=WIFI_PHY_RATE_1M_L`.
Runtime overrides (NVS `e12`): `psk`, `rate`, `role`, `period`, `copies`.

The vendored engine deliberately differs from 09's in five documented ways
(see the banner in `src/e2e_link.h`): different wire magic (09 and 12 boards
ignore each other), own NVS namespace, 1M default PHY rate, 8 s link
timeout, and `setQuiet()` so the continuous stream does not spam per-session
engine prints.

## Long-distance tuning guide

1. Start at defaults (1M, copies=3). Check `status`: `gaps=0` and
   `firstNotCopy0` tell you how hard the link is working.
2. If `retx` climbs and `[tx] slow path` lines appear, you are at the edge —
   good: that is the stack doing its job. Raise `copies 4` before anything
   else.
3. If gaps appear, slow the stream (`period 500`) — airtime is not the
   issue, but a quieter channel gives retransmits room to land.
4. Interference: try another `E2E_CHANNEL` (1..13); 1/6/11 are the classic
   non-overlapping picks, but 12/13 see less WiFi in the 2.4 GHz band.
5. Only if you must have throughput back: `rate 2m` / `5m5` / `11m`.

## Verification

See AGENTS.md in this folder for the current build/flash/soak results.

## Files

```
platformio.ini     env knobs (channel, PSK, PHY rate)
src/e2e_link.h/.cpp vendored 09 engine (diffs documented in the banner)
src/main.cpp       roles + election, telemetry generator, TX pacing,
                   RX dedup/gap accounting + serial output, CLI, WDT, LED
```
