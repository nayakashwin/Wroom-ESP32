#!/usr/bin/env python3
"""ESP-NOW pin monitor: serial bridge + web UI for 12ESP2ESPRemoteControl.

Reads both boards' serial ports (DTR/RTS discipline, EN untouched), polls the
`pins` + `adc` CLI commands every 500 ms for each board's LOCAL GPIO and ADC1
snapshots, and passively parses the receiver's `[rx] ... pins=0x... adc=...`
lines (transmitter's state as it actually arrives over the air) plus
`[rx] pin change:` events.  Analog events are computed here from the stream
with a 128-count threshold (ADC_EVT_DELTA) so radio noise stays out of the
log; the firmware's own deadband-32 `adc change` lines are ignored.
Works with 12 firmware too - missing `adc` support is simply ignored (the
old CLI prints ERR: unknown command).

Serves:
  GET /            the dashboard (index.html next to this file)
  GET /api/state   JSON: boards {tx,rx} {pins, age, uptime, link, ota}, events

Usage:
  python3 pinmon.py [--rx /dev/ttyUSB0] [--tx /dev/ttyUSB1] [--port 8080]
Close any serial monitors first - each port has exactly one owner.
"""
import argparse, json, os, re, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial

HERE = Path(__file__).resolve().parent

RE_LOCAL = re.compile(r"\[local\] pins=0x([0-9A-Fa-f]+) up=(\d+)s")
RE_LOCAL_ADC = re.compile(r"\[local\] adc=([0-9,:]+) up=(\d+)s")
RE_RXLINE = re.compile(r"\[rx\] #(\d+) .*pins=0x([0-9A-Fa-f]+)(?: adc=([0-9,:]+))?")
RE_RXCHANGE = re.compile(r"\[rx\] pin change: (.+)")
RE_LINK = re.compile(r"\blink=(up|down)\b")
RE_LINKUP = re.compile(r"LINK (up|down) ")
ADC_PINS = (32, 33, 34, 35, 36, 39)   # ADC1 channels in firmware kAdcPins order
ADC_EVT_DELTA = 128                   # log analog jumps >= 128 counts (~0.1 V):
                                      # above the radio-coupled +-110 bounce that
                                      # otherwise floods the log (firmware's own
                                      # deadband-32 `adc change` lines are ignored),
                                      # below any real pot movement; measured
                                      # against the last LOGGED value, so a slow
                                      # drift still fires once it accumulates
KMASK = 0
for _p in (0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39):
    KMASK |= 1 << _p

def parse_adc(s):
    """'32:593,33:144,...' -> list in ADC_PINS order (None for missing)."""
    vals = {}
    for pair in s.split(","):
        if ":" in pair:
            k, v = pair.split(":", 1)
            try:
                vals[int(k)] = int(v)
            except ValueError:
                pass
    return [vals.get(p) for p in ADC_PINS]

class Board:
    def __init__(self, key, label, port):
        self.key, self.label, self.port = key, label, port
        self.lock = threading.Lock()
        self.pins = None            # local snapshot (int)
        self.local_t = 0.0
        self.uptime = None
        self.adc = None             # local ADC1 snapshot, list in ADC_PINS order
        self.adc_mark = None        # last locally LOGGED analog values
        self.ota_pins = None        # what the peer received over the air
        self.ota_adc = None         # ...and its analog values
        self.ota_mark = None        # last LOGGED ota analog values
        self.ota_id = None
        self.ota_t = 0.0
        self.link = "?"
        self.err = None
        self.ser = None
        self.prev = None            # for local diff -> events
        self.lines = 0
        self.led_seq = 0            # bumped per [rx] line: physical LED pulse proxy

    def snapshot(self, now):
        with self.lock:
            return {
                "label": self.label, "port": self.port,
                "pins": self.pins,
                "hex": None if self.pins is None else f"0x{self.pins:010X}",
                "adc": self.adc,
                "age_ms": None if not self.local_t else int((now - self.local_t) * 1000),
                "uptime_s": self.uptime, "link": self.link, "err": self.err,
                "lines": self.lines, "led_seq": self.led_seq,
                "ota": None if self.ota_pins is None else {
                    "pins": self.ota_pins, "hex": f"0x{self.ota_pins:010X}",
                    "adc": self.ota_adc,
                    "id": self.ota_id,
                    "age_ms": int((now - self.ota_t) * 1000)},
            }

def open_safe(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.setDTR(False); s.setRTS(False)
    return s

EVENTS = []          # newest last: {"t": epoch, "board": "tx", "src": "local|ota", "text": "..."}
EVLOCK = threading.Lock()

def add_event(board, src, text):
    with EVLOCK:
        EVENTS.append({"t": time.time(), "board": board, "src": src, "text": text})
        del EVENTS[:-40]

def diff_pins(board, new, now):
    old = board.prev
    board.prev = new
    if old is None:
        return
    changed = (old ^ new) & KMASK
    if not changed:
        return
    bits = []
    for b in range(40):
        if changed >> b & 1:
            bits.append(f"IO{b} {old >> b & 1}->{new >> b & 1}")
    add_event(board.key, "local", " ".join(bits))

def adc_jumps(mark, new):
    """Advance a last-logged mark per channel; return jumps >= ADC_EVT_DELTA."""
    bits = []
    for i, (o, n) in enumerate(zip(mark, new)):
        if o is None or n is None:
            continue
        if abs(n - o) >= ADC_EVT_DELTA:
            bits.append(f"ADC{ADC_PINS[i]} {o}->{n}")
            mark[i] = n              # baseline advances only when logged
    return bits

def diff_adc(board, new):
    if board.adc_mark is None:
        board.adc_mark = list(new)
        return
    bits = adc_jumps(board.adc_mark, new)
    if bits:
        add_event(board.key, "local", " ".join(bits))

def reader(board):
    """Line-buffered reader: chunks may split lines mid-message (soak lesson)."""
    while True:
        try:
            if board.ser is None:
                board.ser = open_safe(board.port)
                with board.lock:
                    board.err = None
            buf = b""
            while True:
                d = board.ser.read(4096)
                if d:
                    buf += d
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode(errors="replace").strip("\r")
                        if line:
                            handle_line(board, line)
        except Exception as e:
            with board.lock:
                board.err = str(e)
            try:
                if board.ser: board.ser.close()
            except Exception:
                pass
            board.ser = None
            time.sleep(3)

def handle_line(board, line):
    now = time.time()
    m = RE_LOCAL.search(line)
    if m:
        val = int(m.group(1), 16)
        with board.lock:
            board.lines += 1
            prev_set = board.pins is not None
            board.pins = val
            board.local_t = now
            board.uptime = int(m.group(2))
        if prev_set:
            diff_pins(board, val, now)
        else:
            board.prev = val
        return
    m = RE_LOCAL_ADC.search(line)
    if m:
        vals = parse_adc(m.group(1))
        with board.lock:
            board.lines += 1
            board.adc = vals
            board.local_t = now
            board.uptime = int(m.group(2))
        diff_adc(board, vals)
        return
    m = RE_RXLINE.search(line)
    if m:                                   # receiver saw the peer's telemetry
        b = BOARDS["tx"] if board.key == "rx" else BOARDS["rx"]
        adc = parse_adc(m.group(3)) if m.group(3) else None
        with b.lock:
            b.ota_pins = int(m.group(2), 16)
            b.ota_adc = adc
            b.ota_id = int(m.group(1))
            b.ota_t = now
        with board.lock:                    # this board's LED pulsed on reception
            board.led_seq += 1
        if adc:                             # analog jumps, computed here (see
            if b.ota_mark is None:          # ADC_EVT_DELTA - the firmware's own
                b.ota_mark = list(adc)      # deadband-32 lines are too chatty)
            else:
                bits = adc_jumps(b.ota_mark, adc)
                if bits:
                    add_event(b.key, "ota", " ".join(bits))
        return
    m = RE_RXCHANGE.search(line)
    if m:
        txt = re.sub(r"(\d+):(\d)->(\d)", r"IO\1 \2->\3", m.group(1))
        add_event("tx" if board.key == "rx" else "rx", "ota", txt)
        return
    m = RE_LINK.search(line) or RE_LINKUP.search(line)
    if m:
        with board.lock:
            board.link = m.group(1)

def poller():
    while True:
        for b in BOARDS.values():
            try:
                if b.ser and b.ser.is_open:
                    b.ser.write(b"pins\nadc\n")
            except Exception:
                pass
        time.sleep(0.5)

BOARDS = {}

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/api/state"):
            now = time.time()
            with EVLOCK:
                evs = EVENTS[::-1]
            body = json.dumps({
                "ts": now,
                "boards": {k: b.snapshot(now) for k, b in BOARDS.items()},
                "events": evs,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
        elif self.path in ("/", "/index.html"):
            body = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
        else:
            self.send_response(404); body = b"not found"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rx", default="/dev/ttyUSB0", help="receiver serial port")
    ap.add_argument("--tx", default="/dev/ttyUSB1", help="transmitter serial port")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="0.0.0.0")
    a = ap.parse_args()

    BOARDS["rx"] = Board("rx", "Receiver", a.rx)
    BOARDS["tx"] = Board("tx", "Transmitter", a.tx)
    for b in BOARDS.values():
        threading.Thread(target=reader, args=(b,), daemon=True).start()
    threading.Thread(target=poller, daemon=True).start()

    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    print(f"pin monitor: http://{a.bind}:{a.port}  (rx={a.rx} tx={a.tx})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
