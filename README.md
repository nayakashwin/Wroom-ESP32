# ESP32 projects — NodeMCU ESP-32S (ESP-WROOM-32)

Eight PlatformIO projects that build up from "connect to WiFi" to "read BLE
temperature sensors and answer questions in Telegram". Each numbered folder is
a complete, self-contained firmware; later projects reuse earlier ones.

Board: **NodeMCU v3 with ESP-WROOM-32** (4 MB flash, 320 KB RAM, 240 MHz),
USB-UART **CP2102** → `/dev/ttyUSB0`, LED on **GPIO2**, serial **115200**.

---

## 1. Install (once per machine)

```bash
# PlatformIO Core (official installer, PEP 668 safe)
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py
echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
pio --version

# serial permissions
sudo usermod -aG dialout "$USER"      # then log out and back in
sudo cp ~/.platformio/penv/lib/python*/site-packages/platformio/assets/system/99-platformio-udev.rules \
        /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger

ls /dev/ttyUSB0                        # board plugged in - must exist
```

Toolchains (`espressif32@6.13.0`, xtensa-esp32 GCC, esptool, Arduino
framework) are cached in `~/.platformio` and reused by every project — nothing
else to install.

The full build/flash/monitor reference (including the USB hub power-switching
helper and a troubleshooting table) is in
[`_shared/esp32-build-flash.md`](_shared/esp32-build-flash.md). It is also
loaded automatically into every opencode session.

---

## 2. Secrets — do this before building anything

No project contains a WiFi password, bot token or chat id. Those live in one
gitignored file that every project reads:

```bash
cp _secrets/secrets.example.h _secrets/secrets.h
$EDITOR _secrets/secrets.h
```

Fill in four values:

| macro | used by | where to get it |
|---|---|---|
| `WIFI_SSID` | 01, 02, 03, 04, 06, 08 | your 2.4 GHz network name (the ESP32 has no 5 GHz radio) |
| `WIFI_PASS` | 01, 02, 03, 04, 06, 08 | that network's password |
| `BOT_TOKEN` | 02, 03, 04, 06, 08 | Telegram → message **@BotFather** → `/newbot` → it replies with `123456789:AA...` |
| `CHAT_ID` | 02, 04, 06, 08 | send your new bot any message, then open `https://api.telegram.org/bot<TOKEN>/getUpdates` in a browser — your id is under `message` → `chat` → `id`. For a group/channel use the negative id, or the `@channelusername` |

`05GPIO` and `07BlueToothTempSensor` need no secrets at all.

How it is wired up (so you can copy the pattern into a new project):

- `platformio.ini`: `build_flags  = -D LED_BUILTIN=2 -I../_secrets`
- `src/main.cpp`:
  ```cpp
  #if !defined(__has_include) || !__has_include("secrets.h")
  #error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
  #endif
  #include "secrets.h"
  ```
- `.gitignore` keeps `_secrets/secrets.h` and every `.pio/` build tree out of
  git (compiled firmware images contain the secrets as strings, so the build
  output must not be committed either).

If you forget step 2, the build stops immediately with that `#error` message —
nothing else to guess.

---

## 3. Build, flash, monitor (same for every project)

```bash
cd 01Wifi                      # or any other project folder
pio run -e default             # compile -> .pio/build/default/firmware.bin
sg dialout -c 'pio run -e default -t upload'    # flash (skip sg if you are in dialout)
sg dialout -c 'pio device monitor -b 115200 -p /dev/ttyUSB0'   # Ctrl+] to quit
```

Only one program can hold `/dev/ttyUSB0` at a time — close the monitor (or
`apps/arduino-serial-gui`) before flashing. After the upload the board resets
itself and the boot banner appears on the monitor.

---

## 4. What each project does

| folder | needs secrets | what it does |
|---|---|---|
| `01Wifi` | `WIFI_*` | joins WiFi, pings `google.com` 3 times, LED heartbeat. No serial input. |
| `02TelegramTX` | + `BOT_TOKEN`, `CHAT_ID` | **type a line + Enter on the serial console → it arrives in your Telegram chat.** Sends a greeting on connect. |
| `03TelegramRX` | + `BOT_TOKEN` | long-polls `getUpdates` and prints every incoming Telegram message on the serial console. |
| `04TelegramRXTX` | + `BOT_TOKEN`, `CHAT_ID` | 02 and 03 combined: send by typing, receive by polling, one firmware. |
| `05GPIO` | — | serial console for all 26 broken-out GPIOs: mode, pull, PWM, ADC, touch, read/write. |
| `06GPIOTelegram` | + `BOT_TOKEN`, `CHAT_ID` | drive the GPIOs from Telegram *or* the serial console; only the `CHAT_ID` owner may. |
| `07BlueToothTempSensor` | — | BLE BTHome reader for three **LYWSD03MMC** thermometers (ATC firmware), prints temperature / humidity / battery every 5 s. |
| `08BlueToothTelegram` | + `BOT_TOKEN`, `CHAT_ID` | 07 plus Telegram: typing `/temp` (in Telegram or on the serial console) replies with the sensor grid; LED heartbeat + task watchdog. |

Sensor MACs and labels for 07/08 are in each project's `src/main.cpp`
(`kSensors[]`) — edit them to match the thermometers you own.

---

## 5. Adding a project

```bash
./new_project.sh 09MyThing     # copies _project/, renames the env
```

If your new project talks to WiFi or Telegram, add `-I../_secrets` to its
`build_flags` and `#include "secrets.h"` as shown in section 2. Each project's
own `AGENTS.md` documents its commands, memory budget and the gotchas found
while building it — read those before changing code.

---

## 6. Layout

```
.gitignore              secrets.h + .pio/ stay out of git
README.md               this file
new_project.sh          scaffold a new project from _project/
_project/               template used by new_project.sh
_shared/                build/flash/monitor reference (machine-wide)
_secrets/secrets.h              YOUR secrets - never committed
_secrets/secrets.example.h      committed template + instructions
01Wifi … 08BlueToothTelegram/   the projects
```
