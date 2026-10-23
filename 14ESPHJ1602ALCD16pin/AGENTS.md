# 14ESPHJ1602ALCD16pin — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2). `lib_deps`
  = `arduino-libraries/LiquidCrystal@^1.0.7`.
- Build output: `.pio/build/default/firmware.bin`.
- Firmware: reads newline-terminated text from Serial (115200 baud) and
  displays on an HJ1602A 16×2 LCD via 4-bit parallel interface.
  Short messages (≤16 chars) are centred on row 0; longer text is word-
  wrapped into 16-char lines; 1–2 lines shown statically; >2 lines
  scroll upward line by line (2.5 s/line, 1 s pause after the last
  line, then repeat).  New input interrupts scrolling immediately.
- No WiFi, no secrets, no external libraries.

## Files

- `src/main.cpp` — app: serial reader, message formatter, LCD driver
  (LiquidCrystal), auto-scroll state machine
- `platformio.ini` — env knobs (LED_BUILTIN=2)
