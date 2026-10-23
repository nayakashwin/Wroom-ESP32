# 14ESPHJ1602ALCD16pin — ESP32 → HJ1602A 16×2 LCD

Reads text from the serial port and displays it on a 16×2 character LCD
(HD44780-compatible, 16-pin parallel header).  Long messages are word-
wrapped into 16-char lines and scroll upward line by line, with a pause
at the end.

## Wiring

| LCD pin | Function | ESP32 (NodeMCU-32S) |
|:-------:|----------|---------------------|
| 1 (VSS) | GND      | GND                 |
| 2 (VDD) | 3.3 V    | 3.3 V               |
| 3 (V0)  | Contrast | 10 kΩ pot (VDD↔GND, wiper → V0) |
| 4 (RS)  | Register | GPIO4               |
| 5 (RW)  | Read/Write | GND (hard-wired)  |
| 6 (E)   | Enable   | GPIO5               |
| 7–10    | D0–D3    | NC (4-bit mode)     |
| 11 (D4) | Data     | GPIO14              |
| 12 (D5) | Data     | GPIO12              |
| 13 (D6) | Data     | GPIO13              |
| 14 (D7) | Data     | GPIO15              |
| 15 (A)  | LED +    | 3.3 V (some modules need ~220 Ω series R) |
| 16 (K)  | LED −    | GND                 |

**Contrast pot is required** — without it the display will be blank or
show only solid blocks.  A 10 kΩ linear pot between 3.3 V and GND, with
the wiper connected to V0, works for most modules.

## Display behaviour

| Message | Display |
|:--------------:|---------|
| ≤ 16 chars     | Centred on row 0 |
| 17–32 chars    | Word-wrapped across row 0 and row 1 |
| > 32 chars     | Word-wrapped into 16-char lines; the screen scrolls upward one line at a time (2.5 s/line, 1 s pause after the last line, then repeat) |

Text is wrapped at spaces; a single word longer than 16 chars is split
at the column edge.

New input at any time immediately interrupts scrolling and shows the new
message.

## Usage

```bash
# Build & flash
pio run -e default -t upload

# Serial monitor
pio device monitor -b 115200
```

Type or paste text in the monitor and press Enter.  The LCD updates
immediately.

## Hardware notes

- **HJ1602A** is a common Chinese 16×2 module (blue background, white
  characters, built-in backlight).  Any HD44780-compatible 1602 works.
- The backlight LED (pins 15/16) draws ~20 mA — safe from the ESP32's
  3.3 V rail (which can supply ~800 mA on the onboard regulator).
- No WiFi, no secrets. Requires the `LiquidCrystal` library
  (`arduino-libraries/LiquidCrystal@^1.0.7`) — included via `lib_deps`.
