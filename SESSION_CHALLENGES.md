# Session Retrospective — Challenges & Solutions

Everything that went wrong while setting up ESP32 build/flash on this machine
(AIPC1), and how each was resolved. Read this before debugging a similar
problem.

---

## 1. PlatformIO installed in a throwaway venv

**Symptom:** `pio` lived only in `/tmp/opencode/piovenv` — would vanish on
reboot/tmp cleanup; `pip install platformio` refused with
`error: externally-managed-environment` (PEP 668, Debian 13 / Python 3.13).

**Fix:** official isolated installer (creates its own venv, PEP 668-safe):

```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py -o /tmp/get-platformio.py
python3 /tmp/get-platformio.py          # → ~/.platformio/penv
echo 'export PATH="$HOME/.platformio/penv/bin:$PATH"' >> ~/.bashrc
```

Toolchains/platform (`espressif32@6.13.0`, xtensa GCC, esptool) live in the
shared `~/.platformio` core dir, so both the old and new install reused them —
no re-download.

## 2. Board `nodemcuv3` not found

**Symptom:** `UnknownBoard: Unknown board ID 'nodemcuv3'`.

**Fix:** created a board definition at
`~/.platformio/platforms/espressif32/boards/nodemcuv3.json` (4 MB flash,
320 KB RAM, 240 MHz, variant `esp32`, frameworks arduino+espidf,
upload speed 460800). Rebuild: `pio run -e prod_nodemcuv3` → SUCCESS.

## 3. Serial permission denied, then total USB failure

Three distinct failures on the CP2102 (`10c4:ea60`):

| Stage | Symptom |
|---|---|
| a | `[Errno 5] could not open port /dev/ttyUSB0` |
| b | `cp210x ttyUSB0: failed set request 0x0 status: -71` → `cp210x_open - Unable to enable UART` |
| c | `usb 3-11: can't set config #1, error -71` — device gone from `/dev`, EPROTO at config-set |

**a — permissions:** user wasn't actually in `dialout` (earlier attempt never
took effect; group changes only apply to new logins). Fixed with
`sudo usermod -aG dialout dietpi`; **workaround for existing shells:**
`sg dialout -c '<command>'`.

**b/c — hardware:** `-71` (EPROTO) at control-transfer/config-set level is a
USB signal/power fault, not software. Software recovery attempts (3×
`echo 0/1 > /sys/bus/usb/devices/3-11/authorized`) all failed — enumeration
succeeded, configuration didn't. **Resolution: physical reseat of the USB
cable** (done by user) → new device address, `/dev/ttyUSB0` back, upload
SUCCESS. Lesson: don't burn time on resets when dmesg shows `error -71`
repeatedly — check cable/port first.

## 4. `pio` not on PATH in non-interactive shells

**Symptom:** headless/agent sessions: `pio: command not found`, even though
`.bashrc` exports the path — bash only sources `.bashrc` for interactive
shells.

**Fix:** every automated runner injects PATH itself:
- `llm_agent.py` prepends `~/.platformio/penv/bin` to `PATH` for each bash
  tool call
- ad-hoc scripts: `export PATH="$HOME/.platformio/penv/bin:$PATH"` first

## 5. opencode blocked the local model (two blockers)

Running the local `maternion/mimo-v2.6:9b-thinking` model on the task through
`opencode run` failed:

1. `permission requested: external_directory (/home/dietpi/*); auto-rejecting`
   — headless opencode auto-denies access outside the project dir, so the
   model couldn't even `ls ~/.platformio` or read `~/.bashrc`.
2. Same PATH problem as #4; after a rejected call the model stalled
   (~6 min of thinking, no progress).

**Fix:** replaced opencode for scripted work with a purpose-built Python
harness, `_shared/llm_agent.py`:
- 4 tools (`bash`, `write_file`, `read_file`, `list_files`) — no directory
  jail, full machine access as requested
- native OpenAI tool-calling against Ollama `/v1/chat/completions`
  (verified the model emits proper `tool_calls` before building on it)
- PATH + shared instructions (`_shared/esp32-build-flash.md`) injected
- JSONL transcript for auditing

Result: the same model completed create→build→flash→verify unattended on the
first harness run.

## 6. Background processes killed by the tool timeout

**Symptom:** launching a long job with `nohup … &` died together with the
shell-tool's 40 s/120 s timeout.

**Fix:** detach into a new session:

```bash
setsid nohup <cmd> > log 2>&1 < /dev/null & disown
```

…then poll the log in follow-up calls. (`setsid` is what makes it survive.)

## 7. C++ edit landed inside `void setup()`

**Symptom:** `expected unqualified-id before 'while'`,
`'Serial' does not name a type` — a hasty patch inserted the SOS loop inside
`setup()`, which Arduino only allows to contain declarations/compatible
blocks.

**Fix:** rewrote the file with proper `setup()`/`loop()` structure. Lesson:
after any "smart" patch, always run a build before claiming success.

## 8. Invalid `platformio.ini` options (warnings)

**Symptom:** `Warning! Ignore unknown configuration option 'monitor.speed'`
(and `upload.speed`, `monitor.upload.tool`, …).

**Cause:** dotted names are not valid ini options — PlatformIO uses
`monitor_speed` / `upload_speed`. The dotted forms are *silently ignored*, so
your baud setting was never applied.

**Fix:** underscore names in `test_blink/platformio.ini` (prod_nodemcuv3 env)
and in new projects' template; `monitor.upload.tool` (nonexistent option)
deleted. The espidf experiment env `prod_pro_esp32dev` still warns on
purpose: "fixing" `upload.flash_size = 8MB` to a valid key would *activate*
an 8 MB layout on this 4 MB chip.

## 9. Wrong environment name

**Symptom:** `UnknownEnvNamesError: Unknown environment names 'default'` in
`test_blink`.

**Cause:** env names differ per project — old `test_blink` = `prod_nodemcuv3`
/ `prod_pro_esp32dev`; scaffolded projects = `default`.

**Fix:** check `platformio.ini` before using `-e <name>`; documented in
`NEW_PROJECT.md`.

## 10. Intermittent empty serial reads

**Symptom:** `/dev/ttyUSB0` reads sometimes return 0 bytes while the board is
clearly running.

**Cause:** opening the CP2102 toggles DTR/RTS, which resets/holds the ESP32
via the auto-reset circuit; output during that window is lost.

**Fix:** open → `sleep(2)` → `reset_input_buffer()` → then read (see snippet
in `NEW_PROJECT.md`); retry once if empty. Also: use
`~/.platformio/penv/bin/python` — system `python3` has no `pyserial`.

## 11. Missing helpers in bare shells

- `rg: command not found` in headless sessions → agents fell back to
  `grep`/`python3`.
- `esptool.py` not on PATH (it ships inside the platform package, not
  `penv/bin`) → flash via `pio run -t upload` instead of calling esptool
  directly.
- PlatformIO udev-rules URL from the docs 404'd → copied the bundled file:
  `~/.platformio/penv/lib/python*/site-packages/platformio/assets/system/99-platformio-udev.rules`
  → `/etc/udev/rules.d/`, then `udevadm control --reload-rules && udevadm trigger`.

---

## What worked end-to-end (for reference)

| Run | Result |
|---|---|
| `test_blink` SOS firmware (assistant) | build+flash+serial verified (`sos` every ~4.5 s) |
| `blink300` — local mimo via `llm_agent.py` | wrote code, built, flashed, verified `cycle_ms=600` (600 ms = 300+300) |
| `blink400` — local mimo, improved prompts | warning-free build, polarity made explicit, flashed, verified 800 ms spacing exactly |

Golden rules learned:

1. Verify with **serial output** after every flash — never trust the upload.
2. `sg dialout -c '...'` until a fresh login shows `dialout` in `id`.
3. `error -71` in dmesg ⇒ physical USB problem, reset toggles won't help.
4. Interactive-only config (`.bashrc`) must be re-injected by every script.
5. Headless agents need explicit permissions/PATH — the task prompt alone
   is not enough.
