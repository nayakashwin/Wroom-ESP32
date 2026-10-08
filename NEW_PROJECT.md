# New ESP32 Project — Step-by-Step Guide

Everything below is verified on this machine (AIPC1 / dietpi / NodeMCU v3 with
ESP-WROOM-32, CP2102 USB-UART on `/dev/ttyUSB0`).

---

## 0. One-time checks (do once per machine or after hardware changes)

```bash
pio --version        # PlatformIO Core 6.2.0; 'pio' must resolve.
                     # If not: open a new terminal, or `source ~/.bashrc`
                     # (PATH line lives in ~/.bashrc: export PATH="$HOME/.platformio/penv/bin:$PATH")

id                   # your groups must include 'dialout'.
                     # If not: sudo usermod -aG dialout $USER  then re-login.
                     # Until re-login, prefix flash/monitor commands with sg dialout -c '...'

ls /dev/ttyUSB0      # board plugged in; if missing: lsusb | grep 10c4
```

Nothing else needs installing: `espressif32@6.13.0`, the xtensa-esp32
toolchain, esptool and the Arduino framework are already cached in
`~/.platformio` (1.5 GB) and are reused by every project.

---

## 1. Scaffold the project from the template

```bash
cd ~/esp-wroom-32
./new_project.sh myproject
```

This creates `~/esp-wroom-32/myproject/` containing:

| File            | Purpose                                                    |
|-----------------|------------------------------------------------------------|
| `platformio.ini`| env `default`: espressif32 / nodemcuv3 / arduino, LED=GPIO2, 115200 baud |
| `src/main.cpp`  | starter blink sketch — replace with your code              |
| `AGENTS.md`     | project-specific notes for LLM agents (name is pre-filled) |
| `.pio/`         | copied build cache (harmless; first build refreshes it)    |

> Env name matters: new projects use `-e default`. `test_blink` is older and
> uses `-e prod_nodemcuv3`. Running `-e default` in `test_blink` fails with
> `UnknownEnvNamesError`.

Manual alternative (no scaffold):

```bash
mkdir -p ~/esp-wroom-32/myproject/src
cd ~/esp-wroom-32/myproject
pio project init --board nodemcuv3 --framework arduino
# then edit platformio.ini to match the template's [env:default] section
```

## 2. Write your firmware

Replace `src/main.cpp` with your code. Requirements/defaults:

- `#include <Arduino.h>`; `pinMode(LED_BUILTIN, OUTPUT)` in `setup()`
  (`LED_BUILTIN=2` comes from `build_flags` in `platformio.ini`)
- Serial at 115200 for debug output
- Keep changes inside the project folder

## 3. Build

```bash
cd ~/esp-wroom-32/myproject
pio run -e default
```

Success criteria — all three:

1. `[SUCCESS]` at the end
2. **No** `Warning! Ignore unknown configuration option` lines
   (root cause: dotted option names like `monitor.speed` are invalid;
   correct form is `monitor_speed` / `upload_speed`)
3. `.pio/build/default/firmware.bin` timestamp updated

## 4. Upload (flash)

```bash
sg dialout -c 'pio run -e default -t upload'
```

(Drop the `sg dialout -c '...'` wrapper after a fresh re-login, once `id`
shows `dialout` in your *current* session.)

Success criteria: esptool writes at `0x00008000`, `0x0000e000`, `0x00010000`
→ `Hash of data verified` → `Hard resetting via RTS pin...` → `[SUCCESS]`.

## 5. Verify it actually runs

Never trust the upload step alone — read the serial port:

```bash
sg dialout -c 'pio device monitor -b 115200'     # Ctrl+C exits
```

or non-interactively (8 s capture):

```bash
sg dialout -c '$HOME/.platformio/penv/bin/python -c "
import serial,time
s=serial.Serial(\"/dev/ttyUSB0\",115200,timeout=1)
time.sleep(2); s.reset_input_buffer()
t=time.time(); out=[]
while time.time()-t<8:
    d=s.read(128)
    if d: out.append(d)
print(b\"\".join(out).decode(errors=\"replace\")[:600])
s.close()"'
```

(`~/.platformio/penv/bin/python` is used because it has `pyserial`;
system `python3` does not.)

## 6. Iterate

Edit source → repeat steps 3–5. If output looks like a stale build:

```bash
pio run -t clean && pio run -e default
```

---

## Optional: have a local LLM do steps 1–5 autonomously

Instead of writing/building yourself, delegate to a local model through the
Python harness (no opencode involved):

1. **Create the project** (step 1) and write `PROMPT.md` in it — a task spec
   for the model. Copy `~/esp-wroom-32/blink400/PROMPT.md` and change the
   requirement (timing, output format…). Do **not** put any code in the
   prompt; only the behaviour to implement.

2. **Run the agent:**

   ```bash
   python3 ~/esp-wroom-32/_shared/llm_agent.py \
     --cwd ~/esp-wroom-32/myproject \
     --prompt ~/esp-wroom-32/myproject/PROMPT.md \
     --model 'maternion/mimo-v2.6:9b-thinking' \
     --log /tmp/myproject_agent.jsonl
   ```

   The harness gives the model four tools — `bash`, `write_file`,
   `read_file`, `list_files` — injects `_shared/esp32-build-flash.md` as
   system rules, and puts `~/.platformio/penv/bin` on PATH for every tool
   call (so `pio` always works).

3. **Monitor** the live transcript (it prints steps, tool calls, results).
   The model writes `src/main.cpp`, builds, flashes and verifies serial by
   itself; it stops when done and prints an evidence report.

4. **Verify independently** afterwards — rebuild, re-read serial (steps 3
   and 5 above). Never accept the model's report without checking.

Quick one-shot chat (no tools):

```bash
python3 ~/esp-wroom-32/_shared/llm_chat.py "your question"
python3 ~/esp-wroom-32/_shared/llm_chat.py -m qwen3-coder:30b "explain this error: ..."
```

---

## Troubleshooting quick reference

| Symptom | Fix |
|---|---|
| `pio: command not found` | `source ~/.bashrc` or open a new terminal |
| `Permission denied: /dev/ttyUSB0` | prefix with `sg dialout -c '...'`; check `id` |
| `UnknownEnvNamesError` | wrong env name — new projects: `-e default`, test_blink: `-e prod_nodemcuv3` |
| `port is busy` | a monitor/flash session still holds the port |
| `can't set config #1, error -71` / `[Errno 5]` | physical USB fault: reseat cable, other port (avoid hub), power-cycle board |
| `/dev/ttyUSB0` missing | `lsusb \| grep 10c4`; reseat if absent |
| Serial read returns empty | open → wait 2 s → `reset_input_buffer()` (DTR/RTS toggles reset the board); retry |
| Stale binary after edit | `pio run -t clean && pio run -e default` |
| `Warning! Ignore unknown configuration option` | use underscore names (`monitor_speed`, `upload_speed`), not dots |

Full machine-wide rules: `_shared/esp32-build-flash.md`.
