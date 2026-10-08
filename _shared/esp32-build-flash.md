# ESP32 Build & Flash — LLM Prompt

**Single source of truth.** Loaded automatically in every opencode session via
`~/.config/opencode/opencode.jsonc` → `"instructions"`. For any other LLM,
paste everything below the line. Edit *this* file only — projects link to it,
never copy it.

---

You are working on an ESP32 firmware project. Follow these facts exactly.

## Hardware / environment

- Board: NodeMCU v3 with ESP-WROOM-32 (4 MB flash, 320 KB RAM, 240 MHz)
- USB-UART: CP2102 (`USB VID:PID=10C4:EA60`) → serial device `/dev/ttyUSB0`
- Onboard LED: GPIO2 (`LED_BUILTIN=2`)
- Serial monitor baud: **115200**
- User: `dietpi`, in group `dialout`
- PlatformIO Core 6.2.0 lives in `~/.platformio/penv/bin` (on PATH via `~/.bashrc`)
- Tool packages (espressif32@6.13.0, xtensa-esp32 GCC, esptool) already cached in
  `~/.platformio` — never reinstall the platform unless it is missing

## One-time machine setup (skip if `pio --version` works and /dev/ttyUSB0 exists)

```bash
# 1. PlatformIO Core (official isolated installer, PEP 668 safe)
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py

# 2. Shell PATH
grep -q '.platformio/penv/bin' ~/.bashrc || \
  echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.bashrc
export PATH="$HOME/.platformio/penv/bin:$PATH"

# 3. Serial permissions
sudo usermod -aG dialout "$USER"          # takes effect on new login
sudo cp ~/.platformio/penv/lib/python*/site-packages/platformio/assets/system/99-platformio-udev.rules \
        /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger

# 4. USB port power control (see "USB port power control" below)
sudo apt-get install -y uhubctl
```

If your shell session does not yet have `dialout`, prefix flash/monitor commands
with: `sg dialout -c '<command>'`

## Project layout

```
platformio.ini      # environments (env:prod_nodemcuv3 is the working one)
src/main.cpp        # firmware source
```

`platformio.ini` essentials:

```ini
[env:prod_nodemcuv3]
platform   = espressif32
board      = nodemcuv3
framework  = arduino
monitor_speed = 115200
upload_speed  = 115200
build_flags = -D LED_BUILTIN=2 -D BOOT_ADDR_0=0x10000 -D BOOT_PAGESIZE=4096
```

The board definition `nodemcuv3.json` is custom-installed at
`~/.platformio/platforms/espressif32/boards/nodemcuv3.json`. If that file is
missing (fresh machine), recreate it: copy any ESP32 WROOM board json in that
directory, set `"name": "NodeMCU-32 (ESP-WROOM-32)"`, `"mcu": "esp32"`,
`"flash_size": "4MB"`, `"maximum_size": 4194304`,
`"maximum_ram_size": 327680`, `"speed": 460800`, frameworks
`["arduino","espidf"]`, `build.variant = esp32`.

## Secrets (WiFi / Telegram credentials)

No project stores a WiFi password, bot token or chat id in its source. They
live in one file shared by the whole repo:

```
_secrets/secrets.h          gitignored, real values - never commit
_secrets/secrets.example.h  committed template + how to obtain each value
```

Projects that need them (01, 02, 03, 04, 06, 08) put the folder on the include
path and include the header:

```ini
build_flags  = -D LED_BUILTIN=2 -I../_secrets
```

```cpp
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"          // WIFI_SSID, WIFI_PASS, BOT_TOKEN, CHAT_ID
```

05GPIO and 07BlueToothTempSensor need no secrets. A missing file stops the
build with that `#error` — never inline credentials to work around it, and
never commit `.pio/` (compiled images contain the secrets as strings).

Note on environment names: every project in this repo uses `[env:default]`
(`pio run -e default`); the `prod_nodemcuv3` env below is the name used by the
older standalone examples.

## Build

```bash
pio run -e prod_nodemcuv3
```

Expected: `[SUCCESS]`, ~269 KB flash / ~21 KB RAM. Output firmware:
`.pio/build/prod_nodemcuv3/firmware.bin`

## Flash

```bash
sg dialout -c 'pio run -e prod_nodemcuv3 -t upload'
```

Expected: esptool writes at `0x00008000`, `0x0000e000`, `0x00010000`, then
`Hash of data verified` and `Hard resetting via RTS pin...` → `[SUCCESS]`.

Force a rebuild first if edits seem ignored: `pio run -t clean && pio run -e prod_nodemcuv3`.

## Serial monitor (verify the firmware runs)

```bash
sg dialout -c 'pio device monitor -b 115200 -p /dev/ttyUSB0'
# or non-interactive read of 12 s:
sg dialout -c 'python3 -c "
import serial,time
s=serial.Serial(\"/dev/ttyUSB0\",115200,timeout=1); s.reset_input_buffer()
t=time.time(); out=[]
while time.time()-t<12:
    d=s.read(64)
    if d: out.append(d)
print(b\"\".join(out).decode(errors=\"replace\"))
s.close()"'
```

Current firmware prints `sos` every ~4.5 s (Morse SOS on GPIO2, 500 ms/element).

## USB port power control

`uhubctl` 2.6.0 is installed. The board **must be plugged into one of the 4 ports
of the TI TUSB8041 hub (`4-4`)** for power switching to work.

| Hub | Ports | Switchable |
|---|---|---|
| `4-4` TI TUSB8041 (USB 3.0) | 4 | **yes** (`ppps`) |
| `2-1` Intel 8087:8002 | 8 | yes (`ppps`) |
| `1-1` Intel 8087:800a | 6 | yes (`ppps`) |
| `3-0` xHCI root hub (USB 2.0, 15 ports) | 15 | **no** — `wHubCharacteristic 0x000a`, overcurrent protection only |

Root-hub ports have no power switch in software. A board on `3-x` can only be
soft-reset via `authorized=0/1`, which resets the CP2102 bridge only — it does not
cut VBUS and does not reset the ESP32.

```bash
usb-power status              # hub state + where the CP2102 is
usb-power locate              # devpath, hub, port, and whether it is controllable
usb-power cycle               # off, wait 2 s, on  (auto-detects the board)
usb-power cycle 4-4-2 5       # explicit port, 5 s off
usb-power off|on [port]
```

Wrapper at `~/.local/bin/usb-power` (needs passwordless sudo). It refuses root-hub
targets, and confirms success twice: it parses uhubctl's own `Port N: ... off` /
`power` state, then verifies the device left /sys. Never report a power cycle as
successful without its confirmation line.

Two gotchas baked into the wrapper — do not drop them:

- **`uhubctl` requires `-e` (exact) on this machine.** Plain `uhubctl -l 4-4`
  fails with `No compatible devices detected at location 4-4!` because its USB3
  duality handling does not match the xHCI bus numbering.
- Non-root `uhubctl` prints `There were permission problems while accessing USB`
  and finds nothing. Always `sudo`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Permission denied: /dev/ttyUSB0` | Prefix with `sg dialout -c '...'`; check `id` shows `dialout` |
| `port is busy` | Another monitor/flash session holds it — kill it, or `usb-power cycle` |
| `can't set config #1, error -71` / `failed set request 0x0 status: -71` | USB signal/power fault. Move the board to a TUSB8041 port (`usb-power locate` to confirm), then `usb-power cycle`. Do **not** use the `authorized` toggle — it does not restore VBUS |
| `/dev/ttyUSB0` missing | `usb-power locate`. If the board is on `3-x` (root hub) it cannot be power-cycled — reseat the cable, or move it to `4-4-x` |
| `uhubctl`: `No compatible devices detected at location 4-4!` | Missing `-e`; use `usb-power` or add `-e` |
| `uhubctl`: `permission problems while accessing USB` | Add `sudo` |
| `pip install platformio` → PEP 668 | Use the official installer (step 1 above), never `--break-system-packages` |
| Board `nodemcuv3` unknown | Recreate the board json (see Project layout) |


## Rules for the agent

1. Always run `pio` from the project root (where `platformio.ini` is).
2. Always flash with `sg dialout -c '...'` in an unprovisioned shell.
3. After flashing, read the serial port to confirm output — do not claim success
   from the upload step alone.
4. Never delete `~/.platformio` (1.5 GB of cached toolchains).
5. Re-verify with `pio run -e prod_nodemcuv3` after every source edit.
6. Power-cycle the board with `usb-power cycle`, never by writing
   `authorized` or yanking the cable. A power cut is a hard disconnect — only do
   it when the board is idle, or when deliberately forcing a reset.
