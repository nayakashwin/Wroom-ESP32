# <project-name> — project rules

Machine-wide ESP32 build/flash rules are loaded automatically from
`~/esp-wroom-32/_shared/esp32-build-flash.md` (global opencode config).
Edit that file for shared rules — never duplicate them here.

## Project specifics

- Environment: `default` (arduino, `nodemcuv3`, LED on GPIO2)
- Firmware: describe what it does and its serial output here
- Build output: `.pio/build/default/firmware.bin`
