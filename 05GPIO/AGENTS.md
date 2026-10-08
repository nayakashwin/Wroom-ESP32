# 05GPIO — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Build output: `.pio/build/default/firmware.bin`
- Firmware: **serial GPIO console** — one line per command, 115200 baud,
  commands accepted case-insensitively.

### Covered GPIOs (26, every pin broken out on the NodeMCU ESP-32S v1.1)

```
0 1 2 3 4 5 12 13 14 15 16 17 18 19 21 22 23 25 26 27 32 33 34 35 36 39
```

Refused by design: `6-11` (SPI flash), `37/38` (not bonded on WROOM-32),
`EN` (reset). Input-only (rejected for `mode out` / `pwm`): `34 35 36 39`.

### Commands

| cmd | meaning |
|---|---|
| `help` | command list |
| `pins` | table: pin / class / mode / live level / ADC channel |
| `mode <p> <in\|out\|pu\|pd>` | pinMode; `pu`/`pd` = pull-up/pull-down |
| `w <p> <0\|1>` | digital write (auto-`out` if the pin is untouched) |
| `r <p>` | digital read |
| `a <p>` | `analogRead` + `analogReadMilliVolts` |
| `pwm <p> <Hz> <0-255>` | LEDC, 8-bit duty, own channel from a 16-entry pool |
| `blink <p> <on_ms> <off_ms>` | non-blocking blink (driven from `loop()`) |
| `tog <p>` / `stop <p>` | toggle / detach pwm + stop blink |
| `all <0\|1>` | write every pin already in `mode out` |
| `stat` | configured/output/pwm/blink counts, heap, uptime |

### Behaviour notes

- Boot: **all pins left hi-z / untouched** — nothing is driven until a command
  asks for it. Banner: `05GPIO online: 26 controllable gpio, 115200 baud`
  followed by the pin list and `help`.
- Warnings printed when a sensitive pin is first driven: straps `0 2 5 12 15`,
  UART0 `1 3` (garbles the console), UART2 `16 17`, DAC `25 26`.
- `w`/`tog`/`blink`/`pwm` on a non-output pin return `ERR: ... - use: mode <p> out`
  unless the pin is untouched, in which case it is auto-configured (`auto:` line).
- `pins`/`stat`/error lines are the verification anchors: every reply starts with
  `ok:`, `ERR:`, `auto:` or `note:` (except `pins`, `stat`, `help`, `r`, `a`).
