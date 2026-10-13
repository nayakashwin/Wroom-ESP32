# 10ESPPhoneComms — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2). No `lib_deps`
  (BluetoothSerial and ESP-NOW ship with the framework), no `_secrets/`
  include (BT + ESP-NOW need no WiFi credentials).
- Firmware: phone↔ESP32↔ESP32↔phone relay — Android app talks Classic BT SPP
  to the ESP32; the two ESP32s talk over the vendored encrypted ESP-NOW link
  from 09ESP2ESPCommunication. Text chat (reliable) + voice PTT
  (best-effort µ-law 8 kHz, phones transcode).
- **Same image on both boards.** Two-board workflow: flash `/dev/ttyUSB0` and
  `/dev/ttyUSB1` (see 09's AGENTS.md for the dual-port capture recipe and
  serial-open reset gotchas — they apply here too).
- Phases, wire protocol and exit criteria: **[PLAN.md](PLAN.md) in this
  folder** — read it before changing anything; update its checkboxes as
  phases complete.
- Android app source will live in `android/` (built with the SDK installed
  in Phase 8; recorded here once present).
- Build output: `.pio/build/default/firmware.bin`.

## Android app (android/)

- Build: `cd android && ./gradlew assembleDebug` →
  `app/build/outputs/apk/debug/app-debug.apk` (needs `local.properties` with
  `sdk.dir=/home/dietpi/Android/Sdk`; JDK 21 via apt, Gradle 8.7 wrapper).
- SDK: `~/Android/Sdk` (cmdline-tools 12.0, platform-tools, android-34,
  build-tools 34.0.0). adb: `~/Android/Sdk/platform-tools/adb`.
- Zero androidx dependencies — plain Activity, programmatic UI, Kotlin.
- Package `com.espphone.app`; files: Frame.kt (protocol port), EspLink.kt
  (SPP + reader thread), G711.kt (µ-law), Voice.kt (mic pump + jitter-buffer
  player), MainActivity.kt (UI + PTT).
- Pair the phone with the boards (`ESPPhone-xxxx`) in Android BT settings
  before connecting from the app (the app only lists bonded devices).

## Current state

Phases 0–10 complete, Phase 11 underway (2026-10-11). Two phones (Pixel 7
Pro + OnePlus 6), both paired to both boards, both running the APK:
- **Chat**: 2-way with auto-reconnect (2 s backoff) across board reboot
  and phone BT off/on; auto-scrolls to newest.
- **Voice — full duplex hands-free**: mic streams while connected &
  unmuted; MUTE toggle; SPEAKER ON/OFF (per-track `preferredDevice`
  routing); proximity sensor blacks the screen at ear; verified
  continuous 2-phone streaming (~70 fps each way, ~1–2 % loss).
- 2026-10-11 fix: full-duplex freeze (mutual unpair under BT-coex RX
  deafness + silent unpaired drops) → sticky pairing
  (`kLinkTimeoutMs=15000`, `kHelloIdleMs=500`) + `unpdrop` counter.
- Phase 11 remaining: loop WDT, 1 h soak, app stats screen, final docs.

Serial CLI: `help | status | tx | stop | ping | msg | rate | key |
voicebench | ptt | fping | pecho | txt | btsend`. LED: solid = local PTT,
5 Hz = remote talking, fast blink = ARQ stream, 1 Hz = unpaired, 0.25 Hz =
paired idle.

## Gotchas learned the hard way

- **BT controller must be enabled BEFORE `esp_wifi_start()`.** WiFi-first
  → `SerialBT.begin()` aborts in `coex_core_enable` → boot loop. Order in
  `setup()`: `SerialBT.begin(name)` then `e2e::begin()`.
- **WiFi modem sleep must stay ON when BT coexists.** The vendored 09 link
  called `esp_wifi_set_ps(WIFI_PS_NONE)`; IDF aborts with "Should enable WiFi
  modem sleep when both WiFi and Bluetooth are enabled". Fork in
  `src/e2e_link.cpp` `begin()` documents it.
- **huge_app.csv partition is mandatory** — WiFi + BTClassic stacks exceed
  the default.csv 1.3 MB app slot (115.9%).
- **Coexistence cost (BT idle, ESP-NOW bench):** 0.75–0.77 Mbps vs
  1.24–1.47 in 09 (~40%), ping 20 → rtt 3/6/22 ms, heap ~68 KB free. Voice
  budget 64 kbps/direction has ~12× headroom — no pivot to SoftAP+TCP needed.
- Everything from 09's AGENTS.md (serial-open resets, dual-port capture
  recipe, deep-queue duplicate storms) applies here too.
- **Harnesses must drain the serial ports while writing.** A non-reading
  harness back-pressures console prints until a board blocks mid-printf and
  stops accepting commands — looks like catastrophic message loss, is only a
  test artifact. Reader threads fix it (100/100 verified after).
- **Pair a new phone entirely via adb**: `am start -a
  android.settings.BLUETOOTH_SETTINGS` → tap "Pair new device" → tap the
  `ESPPhone-xxxx` entry → tap "PAIR" (uiautomator, re-resolve bounds per
  dump), verify `dumpsys bluetooth_manager | grep bond_state_changed` →
  `BOND_STATE_BONDED`. Same flow for app dialogs (bonded list picker).
- **uiautomator reports ALL-CAPS button text** (`text="CONNECT ESP32"`,
  not "Connect ESP32") — match case or use a `[^"]*` prefix regex.
- **Android 14 silently ignores plain `setMode(MODE_IN_COMMUNICATION)`**
  (AudioService mode-owner rules; `dumpsys audio` stays
  `Actual mode = MODE_NORMAL` and `setSpeakerphoneOn` does nothing).
  Route audio with `AudioTrack.setPreferredDevice` instead; verify via
  the `AudioPlaybackConfiguration` entry (`deviceIds:[2]` = speaker).
- **e2e link under BT-coex**: each board's ESP-NOW RX goes partially deaf
  while its SPP is loaded — a paired peer's *encrypted* frames (incl.
  voice) are supervision, and a MAC+fingerprint-matched HELLO also
  refreshes it, but delivery can be ~10 % → never use short supervision
  timeouts (hence 15 s sticky pairing). While unpaired, bridge frames
  from the phone are dropped silently — check `unpdrop` in `status`.
- **OnePlus 6 (OxygenOS 10) SPP flaps** under full-duplex audio load
  (`bt: client disconnected`/`CONNECTED` cycling on the board) — traffic
  rate recovers immediately; the app's 2 s auto-reconnect masks it.
- Playback proof per device: Pixel → `dumpsys audio`
  `AudioPlaybackConfiguration ... state:started`; OnePlus →
  `logcat | grep OnePlusAudioTrackInjector` (its dumpsys has no
  playback-config section, only `rec start/stop` lines).
