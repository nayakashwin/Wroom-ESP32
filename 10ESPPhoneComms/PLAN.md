# 10ESPPhoneComms — phone ↔ ESP32 ↔ ESP32 ↔ phone comms

Two Android phones talk through two ESP32s:

```
Phone A (app) ⇄ Classic BT SPP ⇄ ESP32 A ⇄ ESP-NOW (09 e2e_link, AES-CCM-128) ⇄ ESP32 B ⇄ Classic BT SPP ⇄ Phone B (app)
```

- **Text chat** — reliable, ACKed, encrypted over the air.
- **Voice PTT** — hold-to-talk; phones are mic + speaker, ESP32s are the
  encrypted radio link. v1 codec: G.711 µ-law 8 kHz mono (64 kbps), phones
  transcode — ESP32 never touches audio samples.
- Same firmware on both boards (roles symmetric, like 09). No router, no
  WiFi credentials, no secrets.

Reuse: the proven link engine from
[`09ESP2ESPCommunication`](../09ESP2ESPCommunication) (≈0.68 Mbps sustained
full-duplex, RTT 1–25/85 ms, 2 h soak with 0 loss) vendored into this project
and extended with a **best-effort voice frame type** as designed in
[`11ESP2ESPAudioWalkieTalkie`](../11ESP2ESPAudioWalkieTalkie) §3.1 — here the
phones replace the I2S mic/amp hardware that design assumed.

Hardware on this bench: the two NodeMCU v3 boards at `/dev/ttyUSB0` and
`/dev/ttyUSB1`. **No host Bluetooth adapter** — every BT data test goes through
a phone (that is why the Android toolchain arrives mid-plan).

---

## Wire protocol (end-to-end, phones are the endpoints)

Frame format on the SPP byte stream *and* inside ESP-NOW payloads:

```
u8  magic1 = 0xA5
u8  magic2 = 0x5A
u8  type
u16 len    (little-endian, payload bytes)
u8  payload[len]
```

| type | name | payload | carriage on the ESP32↔ESP32 link |
|---|---|---|---|
| 0x01 | TEXT | UTF-8 text | reliable ARQ blob (`sendBlob`) |
| 0x02 | VOICE | µ-law chunk, 160 B = 20 ms @ 8 kHz | **best-effort** (`sendVoice`, no ACK/retry) |
| 0x03 | PTT_BEGIN | u8 reserved | best-effort, seq-numbered |
| 0x04 | PTT_END | u8 reserved | best-effort, seq-numbered |
| 0x05 | PING | u32 timestamp | best-effort |
| 0x06 | PONG | u32 timestamp echo | best-effort |
| 0x07 | INFO | key=value lines (link state, name, RSSI) | reliable |

Lost/duplicate best-effort frames: VOICE → jitter buffer covers gaps with
silence; control frames carry a u32 seq so the app can ignore stale dups.

ESP32 forwarding rules: SPP bytes → frame parser → route by type (table
above) → peer ESP32 → frame re-serializer → SPP → phone. The ESP32 adds
nothing to the audio path — it is a pipe.

Voice latency budget (target ≤ 300 ms, walkie-talkie class):

```
mic chunk      20 ms   (phone, AudioRecord)
SPP tx         ~20–60 ms (Android → ESP32, 20 ms write cadence)
ESP-NOW hop    1–25 ms  (measured; 85 ms worst under saturation)
jitter buffer  60–100 ms (3–5 × 20 ms chunks on the receiving phone)
decode + play  ~10 ms   (µ-law is trivial)
```

---

## Phases

Exit criteria are what "done" means; each phase ends with `pio run -e default`
green (rule 5) and, when it touches firmware, a serial-monitor verification.

### Phase 0 — repo + scaffold ✅ (2026-10-10)
- [x] Rename `10ESP2ESPAudioWalkieTalkie` → `11ESP2ESPAudioWalkieTalkie`, fix root README references.
- [x] Scaffold `10ESPPhoneComms` from `_project` (env `default`, no secrets include).
- [x] Banner + LED heartbeat + `help`/`status` CLI builds and flashes.
- Exit: `pio run -e default` SUCCESS; monitor shows the banner and heartbeat.

### Phase 1 — vendor the 09 link, link up on both boards ✅ (2026-10-10)
- [x] Copy `e2e_link.{h,cpp}` from 09 into `src/` (note provenance in the header).
- [x] Both boards pair: `LINK up` on ttyUSB0 and ttyUSB1, auto-bench `verify=ok`.
- Exit: identical link behaviour to 09 on both boards from this project's build.
  **Verified:** LINK up both boards; auto-bench `TX DONE acked=565/565 retx=0
  rate=1.24Mbps` / `RX DONE frames=565/565 dup=0 verify=ok`; `msg` delivered
  both directions; `ping 5` → 5/5, rtt min/avg/max = 2/4/8 ms.

### Phase 2 — Bluetooth SPP bring-up ✅ (2026-10-10)
- [x] `BluetoothSerial` server, device name `ESPPhone-XXXX` (MAC suffix), SPP UUID `00001101-0000-1000-8000-00805F9B34FB`.
- [x] Init-order experiment: **BT controller must enable before `esp_wifi_start()`** (WiFi-first aborts in `coex_core_enable`), and the vendored link had to drop its `esp_wifi_set_ps(WIFI_PS_NONE)` (IDF requires WiFi modem sleep when BT coexists). Order: `SerialBT.begin` → `e2e::begin`.
- [x] SPP → serial line mirror; CLI `btsend <text>` writes to SPP; connect/disconnect events printed.
- [x] Heap/RAM budget: **default.csv app slot overflowed (115.9%) → `huge_app.csv`** (48% of 3 MB used, 65/320 KB RAM); free heap 68–69 KB with both stacks (floor 60 KB met).
- Exit: monitor proves SPP server start + LINK up + bench with BT active. Client
  connect/disconnect byte-flow proof lands with the app (no host BT adapter
  on this bench) — the `hasClient()` transition hook is in and printing.

### Phase 3 — coexistence smoke test ✅ (2026-10-10)
- [x] `tx 512` both directions with the BT stack active (idle): **0.75 / 0.77 Mbps, 2260/2260 frames, verify=ok, 0 aborts** — vs 1.24–1.47 Mbps without the BT stack in 09 (~40% coexistence cost: shared radio + forced modem sleep).
- [x] `ping 20`: **20/20 ok, rtt min/avg/max = 3/6/22 ms** (voice budget 150 ms).
- [x] Decision: **keep ESP-NOW.** 0.75 Mbps = ~12× the 64 kbps voice rate per direction; headroom even for 128 kbps PCM.
- [ ] SPP connection *held open* during bench — deferred to Phase 9 (needs the
  app; no host BT adapter here). Numbers above are BT-stack-idle.
- Exit: voice-sized traffic sustains with BT active (≥10× headroom confirmed); documented in AGENTS.md.

### Phase 4 — frame layer ✅ (2026-10-10)
- [x] `frame.{h,cpp}`: parser (resync on magic) + serializer, shared by the SPP path.
- [x] CLI `txt <text>` → TEXT frame hexdump + send; SPP input now feeds the frame parser (`[btframe]` log).
- Exit: `txt hello` dumps a valid frame (`A5 5A 01 1F 00 …`).

### Phase 5 — text end-to-end (phone-shaped CLI) ✅ (2026-10-10)
- [x] TEXT frames arriving over the link → re-serialized → SPP toward the phone (`onMsg` → `btSendFrame`); reverse path via the SPP frame parser → `sendBlob`.
- [x] Chunking for TEXT > 232 B via existing blob machinery (unchanged from 09).
- Exit: **100/100 msgs (50 each direction, 160 ms cadence) delivered, 0 loss, 0 busy-drops.**
  Harness gotcha: the serial ports must be *drained while writing* — a
  non-reading harness back-pressures the boards' console prints until they
  block mid-printf and stop reading commands (looked like total loss; was a
  test artifact, recorded in AGENTS.md).

### Phase 6 — voice frames (best-effort) ✅ (2026-10-10)
- [x] Vendored `e2e_link` extended: `T_VOICE` wire type + `sendVoice()` (fire-and-forget, drop-when-busy, seq-gap accounting, 8-slot RX FIFO drained in `poll()`), encrypted like all unicasts.
- [x] CLI `voicebench [s] [hz]` emits real end-to-end VOICE frames (160 B = 20 ms µ-law); receiver prints loss/jitter.
- [x] Bench under load (voice 50 Hz + 256 KB ARQ concurrently).
- Exit: **idle loss 0.2–0.6 % (1–3 of 500), under load ~0.2 %, inter-arrival avg 20 ms idle / 24 ms loaded, max spike 56 ms; ARQ unaffected (1130/1130, verify=ok, 0.66 Mbps).** Jitter buffer sizing: 80–120 ms absorbs comfortably.

### Phase 7 — PTT + LED + status ✅ (2026-10-10)
- [x] Full bridge routing: TEXT → reliable blob; every other app frame (VOICE/PTT/PING/PONG/INFO) → best-effort path with the inner frame intact; peer forwards raw to SPP.
- [x] PTT control frames: CLI `ptt on|off` injects exactly as the phone would; peer sees `[vframe] PTT_BEGIN` + `remoteTalk` state.
- [x] LED: solid = local PTT, 5 Hz = remote talking (2 s hold), fast blink = ARQ stream, 1 Hz = unpaired, 0.25 Hz = paired idle.
- [x] `fping [n]` + `pecho` (app-frame PING/PONG over the voice path): **5/5 ok, rtt 5/7/11 ms.** First attempt lost frames to transient radio loss — best-effort frames are allowed to be lost; senders retry (app will).
- [x] `status`: bridge counters (`btIn/textOut/voiceOut/ptt/remoteTalk/pecho`) + voice engine line.
- Exit: signalling works end-to-end from CLI on both boards.

### Phase 8 — Android toolchain on this box ✅ (2026-10-10)
- [x] JDK 21 (`openjdk-21-jdk-headless`, Debian 13; AGP 8.4 + Gradle 8.7 are JDK-21-clean), cmdline-tools 12.0 → `~/Android/Sdk`, platform-tools + platforms;android-34 + build-tools;34.0.0, licenses accepted.
- [x] Gradle 8.7 at `~/gradle-8.7/bin` + wrapper generated in `android/`.
- [x] APK builds: `cd android && ./gradlew assembleDebug` → `app-debug.apk` (833 KB).
- [x] `adb devices` sees a phone over USB — Pixel 7 Pro paired to both boards.

### Phase 9 — app: connect + text chat ✅ (2026-10-10)
- [x] Kotlin app (`android/`, minSdk 24, zero androidx deps, programmatic UI):
  BT permissions (runtime `BLUETOOTH_CONNECT` on 31+), bonded-device picker,
  SPP connect on a reader thread, frame parser port, chat send/receive, INFO
  status line, PTT button wired to PTT_BEGIN/END.
- [x] Firmware side: INFO frame pushed on BT connect + link state changes +
  every 10 s (link/peer/heap).
- [x] Install on phone; chat verified live: phone→A 3/3 + 5/5 bursts,
  A→phone 3/3; INFO frame displayed in-app (`link=up peer=… heap=…`).
- [x] Auto-reconnect in `EspLink` (generation-tracked retry loop, 2 s backoff,
  silent retries after first posted failure) — survives board reboot (RTS
  reset → reconnect in <14 s) and BT off/on cycle (drop logged "retrying
  every 2 s" → reconnected → message flow restored).
- Exit met: 2-way chat over full chain; reconnect survives BT toggle + reboot.
- Gotchas: `cancelDiscovery()` needs BLUETOOTH_SCAN on 12+ (removed — app is
  bonded-only); uiautomator taps must re-resolve bounds each dump (keyboard
  resize + list growth move controls); dialer in foreground hijacks dumps
  (pause tests during calls — calls also drop SPP mid-test).

### Phase 10 — app: voice PTT ✅ single-phone scope (2026-10-10)
- [x] `G711.kt` (canonical µ-law, jshell round-trip verified: ±0 → 0xFF/0x7F,
  max quantization error 511 at full scale = textbook).
- [x] `Voice.kt`: Tx = AudioRecord 8 kHz mono → 160-sample (20 ms) chunks →
  µ-law → SPP while PTT held; Rx = 4-chunk (80 ms) jitter prebuffer + paced
  AudioTrack playback, silence concealment on gaps, oldest-drop on overflow.
- [x] RECORD_AUDIO runtime permission on first PTT press.
- [x] 10-A phone TX: PTT hold 3.5 s → A: PTT_BEGIN/END + VOICE RX 173/175,
  jitter 0/21/101 ms.
- [x] 10-B phone RX: B `voicebench 3 50` → A: VOICE RX 150/150 rxLost=0,
  bridge voiceOut=150 → SPP → phone AudioTrack state idle→started (0→1,
  fresh-process baseline; `dumpsys audio` AudioPlaybackConfiguration proof).
- [x] 10-C reverse: phone on A, PTT 3 s → B: PTT_BEGIN/END + VOICE RX 150,
  jitter 0/20/75 ms. Both boards' SPP bridges verified.
- [x] 10-D two-phone voice↔voice (2026-10-10): second phone = OnePlus 6
  (`62f7924a`, Android 10) — adb-driven Settings paired it with both
  boards ("Pair new device" → entry → PAIR, uiautomator), APK installed,
  RECORD_AUDIO granted. Ran live alongside the Pixel (OnePlus↔30F0/B,
  Pixel↔D77C/A): OnePlus PTT → B → A → Pixel rx=163 → AudioTrack
  started; Pixel PTT → A → B → OnePlus rx=170 lost=1 → `play()
  8 kHz / 1296 B` logged. Human listen/latency spot-check optional —
  delivery proven both directions.
- Known quirks: e2e link idles out ~3 s but re-links transparently on send
  (voicebench straight after idle still 150/150 dropped=0); A's `VOICE RX`
  console report can print several seconds late while an SPP client is
  connected (cosmetic — engine counters on `status` are exact and immediate);
  don't trust `media.audio_flinger` espphone greps (uid appears while idle) —
  use `dumpsys audio` `state:started` transitions **on the Pixel**; OxygenOS
  has no AudioPlaybackConfiguration section at all — there prove playback via
  `logcat -d | grep OnePlusAudioTrackInjector` (or `playerEvent ... event: 2
  ... espphone`); its `dumpsys audio` only logs `rec start/stop` lines. A
  Pixel-style dumpsys FAIL on the OnePlus is a false negative.

### Phase 11 — hardening + soak
- [x] **App conversation overhaul (2026-10-11)**: HOLD TO TALK replaced by
  MUTE toggle (mic streams continuously while connected & unmuted =
  hands-free full duplex; PTT_BEGIN/END sent on unmute/mute), SPEAKER
  ON/OFF toggle, proximity sensor → black overlay + brightness 0 +
  swallowed touches (phone-at-ear mode), chat auto-scroll to newest line.
  Speaker routing uses `AudioTrack.setPreferredDevice` — Pixel/Android 14
  silently ignores plain `setMode(MODE_IN_COMMUNICATION)` (AudioService
  mode-owner rules), so `setSpeakerphoneOn` alone did nothing.
- [x] **Full-duplex freeze root cause + fix (2026-10-11)**: with both phones
  streaming, each board's ESP-NOW RX goes partially deaf (BT-coex), the
  3 s supervision timeout unpaired the link ("LINK down (no frame…)")
  and every phone frame was **silently dropped while unpaired** — mutual
  cascade froze voice both ways (rx=0–2 total). Fixes in `e2e_link.cpp`:
  `kLinkTimeoutMs` 3000→15000 (sticky pairing), `kHelloIdleMs` 1000→500,
  plus `unpdrop` counter in bridge status. Verified: 2 phones × 60 s
  continuous — zero LINK transitions, ~70 fps each way, loss ~1–2 %,
  drop ≤2 %, audio audible on speakers both phones.
- [x] Task WDT on loop() (8 s, panic→reboot; `wdt: armed rc=0` verified on
  both boards — ported from 09 during scaffolding).
- [x] BT/SPP re-listen + link-down INFO (verified 2026-10-11): board B
  held in reset 20 s → A `LINK down (no frame for 15000ms)` at t+15 s →
  **phone on A got `INFO link=down` with SPP intact** (t+17 s); B back →
  re-paired → `INFO link=up` t+32 s; phone on B auto-reconnected (~2 s
  backoff) once B's SPP server returned.
- [x] App stats screen (2026-10-11): long-press the status line → session
  time, connect count, last disconnect reason, last INFO, text/voice
  chunk counters.
- [ ] 1 h mixed soak: chat + alternating PTT + `tx` background; 0 reboots/WDT, heap drift < 1 KB.
- [ ] Update root README row + this file's status; final AGENTS.md verification section.
- Exit: soak report numbers written here; project marked done.

---

## Risks / fallbacks

| risk | mitigation |
|---|---|
| BT Classic + ESP-NOW share one radio (coexistence) | Phase 3 measures it *before* the app investment. Fallback: SoftAP+TCP on the same frame layer — phases 4+ don't care about the transport. |
| flash/RAM pressure (WiFi + BT stacks together) | `huge_app.csv` partition ready; heap watched in `status` from Phase 2 on. |
| Android SPP latency varies by OEM | 20 ms chunk cadence + TCP\_NODELAY-style flush on the socket; measure in Phase 10, raise chunk to 40 ms if needed. |
| only one test phone until Phase 9 | CLI "fake phone" modes (Phases 5/7) carry the far end; real phone↔phone in Phase 9. |
| µ-law quality insufficient later | codec lives only in the app — swap to Opus/AAC-ELD in a later phase without touching firmware. |

## Status

Phases 0–10 complete, Phase 11 underway (2026-10-11). Verified on hardware
with **two phones** (Pixel 7 Pro + OnePlus 6, both paired to both boards):
chat 2-way with auto-reconnect, full-duplex hands-free voice with
MUTE/SPEAKER toggles + proximity ear mode + auto-scrolling chat; the
full-duplex freeze is fixed (sticky pairing under BT-coex deafness).
Remaining Phase 11: WDT, 1 h soak, app stats screen, final docs.
