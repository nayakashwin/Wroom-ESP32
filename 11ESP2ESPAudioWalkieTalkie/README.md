# 11ESP2ESPAudioWalkieTalkie — encrypted ESP-NOW walkie-talkie (design)

**Status: design only — no code yet.** This folder documents the plan to turn
the proven two-board encrypted link from
[`09ESP2ESPCommunication`](../09ESP2ESPCommunication) into a voice
walkie-talkie (PTT half-duplex radio) and later a full-duplex intercom.

Everything radio-related is already solved by the base link: pairing,
AES-CCM-128 encryption (free at MAC level), channel 6, ~0.68 Mbps sustained
per direction, RTT 25 ms avg / 85 ms max under load (2 h continuous-traffic
soak, 0 data loss). Voice needs ~1.3 kbps (Codec2) to 64 kbps (µ-law) —
**10–500× under capacity**. What remains is audio hardware, a codec, and a
best-effort frame type that skips the stream engine's reliability machinery.

Read [`09ESP2ESPCommunication/AGENTS.md`](../09ESP2ESPCommunication/AGENTS.md)
first — its gotchas (serial discipline, port resets, tuning constants) all
apply to this project too.

---

## 1. Hardware (per board, ~₹400–600)

| part | role | notes |
|---|---|---|
| **INMP441** I2S MEMS mic | audio in | digital I2S — clean audio, no ADC involved |
| **MAX98357A** I2S amp + 8 Ω speaker | audio out | I2S in → class-D amp |
| **Push button** 10 kΩ pull-down | PTT | any free GPIO, active high |
| (optional) 10 kΩ pot | volume | **ADC1 pin only** (GPIO32) — never ADC2, it kills WiFi |

### Proposed wiring (no conflicts with LED GPIO2 or each other)

| signal | GPIO | I2S role |
|---|---|---|
| INMP441 SCK | 18 | mic bit clock (I2S0) |
| INMP441 WS | 19 | mic word select |
| INMP441 SD | 21 | mic data out |
| MAX98357A BCLK | 26 | amp bit clock (I2S1) |
| MAX98357A LRC | 25 | amp word select |
| MAX98357A DIN | 22 | amp data in |
| PTT button | 27 | GPIO input, internal pull-up, active low |
| volume pot | 32 | ADC1 (optional) |
| LED | 2 | TX indicator (existing, `LED_BUILTIN`) |

Two I2S peripherals (0 for RX/mic, 1 for TX/amp) — the Arduino core (2.0.17)
supports one full-duplex-capable instance each. Avoid strapping pins
0 / 5 / 12 / 15 for I2S clocks.

---

## 2. Codec selection

| codec | bitrate | CPU / RAM | quality | effort |
|---|---|---|---|---|
| **µ-law (G.711)** 8 kHz | 64 kbps | trivial | telephone | ~30 lines, same day |
| **IMA-ADPCM** 8 kHz | 32 kbps | trivial | decent | ~60 lines |
| **Codec2 1300 bps** ⭐ | **1.3 kbps** | light (~14 KB, few MHz) | classic digital-radio "walkie" sound | ~1 day port |
| Opus NB 6–8 kbps | 8 kbps | heavy (50–80 KB, ~100 MHz) | near-cellular | 2–3 days, risky |

**Path**: build the whole pipe with **µ-law first** (mic → link → speaker
proven in one evening), then swap in **Codec2 1300** — the open ham-radio
voice codec, designed exactly for low-bitrate radio. Opus is a stretch goal
only if Codec2 quality proves insufficient and heap/CPU allow.

Frame cadence: Codec2 = 40 ms per frame (25 frames/s, ~7–9 B payload + seq
header); µ-law = 10 ms per frame (100 pkt/s) or batch 4 frames (40 ms, 25
pkt/s) to match.

---

## 3. Architecture

### 3.1 Voice must NOT use the bulk stream engine

The existing `tx` path is bitmap-ACKed, retransmitted, window-ordered —
perfect for files, wrong for voice (a retransmit adds 50–100 ms and
head-of-line-blocks; a stale voice frame is worse than an audible click).

Add to `e2e_link` a **best-effort frame type** instead:

| property | data frames (existing) | voice frames (new `kTypeVoice`) |
|---|---|---|
| ACK / retransmit | yes (bitmap, window) | **no** — send and forget |
| ordering / completeness | guaranteed | in-order preferred, loss tolerated |
| encryption | AES-CCM-128 via LMK | **same, free** (MAC-level, same peer) |
| size | up to 250 B | ~9–25 B (seq + one codec frame) |
| API | `startStream` / sessions | `sendVoice(payload, len)` |

### 3.2 Audio module (`voice.cpp`)

- I2S0 RX (mic DMA) + I2S1 TX (amp DMA); PTT GPIO interrupt sets `talking`.
- **40 ms frame pump in `loop()`**: read mic frame → encode → `sendVoice`.
  Must respect the existing WDT (`esp_task_wdt_reset()` every loop).
- RX path from `e2e_link::poll()`: push into a **jitter buffer (2–3 frames =
  80–120 ms)** → pop → decode → I2S write. Soak-measured RTT spikes (85 ms)
  are absorbed by a 3-frame buffer.
- Dropped frame → inject silence/comfort noise (don't stretch previous audio).

### 3.3 PTT half-duplex discipline

- Hold PTT → mic live, TX LED on, peer radio muted locally (no echo path).
- Release → listen (speaker live, mic ignored).
- This is authentic walkie-talkie UX **and** solves acoustic feedback for
  free — the mic is never open near the speaker.
- Later `mode duplex` option: always-on intercom (needs echo consideration —
  separate speaker/mic or AEC, out of scope for v1).

---

## 4. Latency budget

```
mic frame   40 ms
encode       <1 ms (Codec2 ~µs–ms on 240 MHz)
ESP-NOW TX   3–5 ms
jitter buf  80–120 ms   (2–3 frames; sized from soak RTT data)
decode + DAC ~20 ms
--------------------------------
end-to-end ≈ 150–220 ms
```

Same as a cell call — fine for walkie-talkie use (radio operators tolerate
up to ~500 ms). Tune the jitter buffer down (2 frames) if needed.

---

## 5. CLI additions (on top of the existing 09 CLI)

| command | action |
|---|---|
| `voice start` / `voice stop` | init/shutdown I2S + codec, begin/stop frame pump |
| `vol 0-8` | amp gain (MAX98357A SD_MODE pin or digital scale) |
| `status` line | add `voice: idle/tx/rx <codec>@<fps>` |
| `ptt` (optional software PTT) | same as holding the button, for bench tests |

LED: solid on while PTT held (TX active); brief blink on voice RX.

---

## 6. Implementation steps

1. **Bench loopback**: wire mic + amp; `voice start` → hear yourself with no
   radio (proves I2S in + out + DMA).
2. **µ-law over the air**: new `kTypeVoice` + `sendVoice` in `e2e_link`;
   µ-law encode/decode; first real walkie-talkie audio between the two
   boards. Verify with the 09 two-board workflow.
3. **PTT + LED + volume**: GPIO interrupt, half-duplex muting, gain.
4. **Codec2 port**: add the Codec2 library (fixed-point), 40 ms framing,
   swap into the same plumbing; keep µ-law as a fallback build flag.
5. **Hardening**: jitter-buffer tuning, silence fill, `status` fields; run a
   30 min–2 h soak with concurrent `tx`/`ping` to confirm voice + data
   coexist; measure clap-test latency.

Effort: steps 1–3 an evening, Codec2 the next day.

---

## 7. Test plan

| stage | pass criteria |
|---|---|
| I2S loopback | clear self-hearing, no clicks at idle |
| µ-law over air | intelligible speech both directions, PTT works |
| Codec2 over air | intelligible "radio" quality, no clicks on frame loss |
| under load | voice stays usable while peer runs `tx 256` benches (the soak profile) |
| latency | clap test ≤ 250 ms end-to-end |
| soak | 1 h voice + data: 0 reboots, 0 heap drift, 0 WDT |

---

## 8. Open questions

- [ ] Audio parts on hand? (design assumes INMP441 + MAX98357A — say if
      different, wiring table changes)
- [ ] v1 = classic PTT half-duplex only, or also always-on intercom?
- [ ] Keep µ-law build flag as fallback after Codec2 lands? (recommended)
- [ ] Channel/rate: stay on channel 6 @ 54 Mbps (current), or drop to 1 Mbps
      PHY for range at voice's tiny bitrate? (`rate 1m` already supported)
