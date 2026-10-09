// 09ESP2ESPCommunication — encrypted point-to-point ESP-NOW link between two
// ESP32s, tuned for speed and reliability (see README.md).
//
// Same firmware on both boards. Serial console (115200):
//   help | status | tx [KB] | stop | ping [n] | msg <text> | key <psk>
// On first pairing the board with the higher MAC auto-runs a benchmark so
// plugging two boards in is enough to see throughput and loss numbers.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include <stdio.h>
#include <string.h>

#include "e2e_link.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
#ifndef E2E_AUTO_BENCH
#define E2E_AUTO_BENCH 1
#endif
#ifndef E2E_AUTO_BENCH_KB
#define E2E_AUTO_BENCH_KB 128
#endif

static const uint32_t BAUD       = 115200;
static const size_t   CMDBUF_MAX = 256;

static char   line[CMDBUF_MAX];
static size_t lineLen = 0;

static uint16_t g_benchPayload;   // ctx for e2e::benchGen

// ------------------------------------------------------------ msg assembly
static uint8_t  g_asm[1024];
static uint32_t g_asmLen = 0, g_asmChunks = 0, g_asmTotal = 0;

static void onMsg(const uint8_t src[6], uint32_t seq, uint32_t total,
                  const uint8_t* data, uint16_t len) {
  uint32_t off = seq * (uint32_t)e2e::payloadMax();
  if (seq == 0) { g_asmLen = 0; g_asmChunks = 0; g_asmTotal = total; }
  else if (total != g_asmTotal) return;
  if (off + len <= sizeof g_asm) {
    memcpy(g_asm + off, data, len);
    if (off + len > g_asmLen) g_asmLen = off + len;
    g_asmChunks++;
  }
  if (g_asmChunks >= g_asmTotal && g_asmTotal) {
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             src[0], src[1], src[2], src[3], src[4], src[5]);
    Serial.printf("[msg] from %s %lu bytes in %lu frame(s): \"%.*s\"\n",
                  mac, (unsigned long)g_asmLen, (unsigned long)g_asmTotal,
                  (int)g_asmLen, g_asm);
    g_asmTotal = 0;
  }
}

// -------------------------------------------------------------- ping machine
static bool     g_pinging = false;
static uint32_t g_pingTarget = 0, g_printOk = 0, g_printTo = 0, g_pingNextMs = 0;
static uint32_t g_pingStartMs = 0;

static void pingStart(uint32_t n) {
  e2e::resetPingStats();
  g_pingTarget = n; g_printOk = g_printTo = 0;
  g_pingNextMs = millis();
  g_pingStartMs = millis();
  g_pinging = true;
}

static void pingMachine() {
  if (!g_pinging) return;
  const e2e::PingStats& ps = e2e::pingStats();
  uint32_t now = millis();
  while (g_printOk < ps.ok && g_printOk + g_printTo < g_pingTarget) {
    g_printOk++;
    Serial.printf("ok: ping %lu/%lu rtt=%lums\n",
                  (unsigned long)(g_printOk + g_printTo), (unsigned long)g_pingTarget,
                  (unsigned long)ps.lastRttMs);
  }
  while (g_printTo < ps.timeouts && g_printOk + g_printTo < g_pingTarget) {
    g_printTo++;
    Serial.printf("ERR: ping %lu/%lu timed out\n",
                  (unsigned long)(g_printOk + g_printTo), (unsigned long)g_pingTarget);
  }
  if (ps.sent < g_pingTarget && (int32_t)(now - g_pingNextMs) >= 0) {
    if (e2e::sendPing()) g_pingNextMs = now + 150;
    else g_pingNextMs = now + 20;
  }
  if ((ps.sent >= g_pingTarget && ps.ok + ps.timeouts >= g_pingTarget) ||
      now - g_pingStartMs > g_pingTarget * 300 + 3000) {
    if (ps.ok)
      Serial.printf("ok: ping done: %lu/%lu ok, rtt min/avg/max = %lu/%lu/%lu ms\n",
                    (unsigned long)ps.ok, (unsigned long)g_pingTarget,
                    (unsigned long)ps.minRttMs,
                    (unsigned long)(ps.sumRttMs / ps.ok), (unsigned long)ps.maxRttMs);
    else
      Serial.println("ERR: ping: no reply (all timed out)");
    g_pinging = false;
  }
}

// ----------------------------------------------------------------- bench
static bool startBench(uint32_t kb) {
  if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return false; }
  if (e2e::streamActive()) { Serial.println("ERR: a stream is already running ('stop' first)"); return false; }
  g_benchPayload = e2e::payloadMax();
  uint32_t frames = (kb * 1024u + g_benchPayload - 1) / g_benchPayload;
  if (!e2e::startStream(e2e::T_DATA, e2e::benchGen, &g_benchPayload, frames, "bench")) {
    Serial.println("ERR: cannot start stream");
    return false;
  }
  return true;
}

static bool g_autoDone = false, g_userTouched = false;
static uint32_t g_lastLinks = 0, g_linkAt = 0;

static void autoBench() {
#if E2E_AUTO_BENCH
  if (g_autoDone || g_userTouched) return;
  uint32_t links = e2e::linkUpCount();
  if (links != g_lastLinks) { g_lastLinks = links; g_linkAt = millis(); }
  if (!links || millis() - g_linkAt < 1500 || !e2e::paired()) return;
  uint8_t own[6], peer[6];
  WiFi.macAddress(own);
  e2e::peerMac(peer);
  if (memcmp(own, peer, 6) > 0) startBench(E2E_AUTO_BENCH_KB);
  g_autoDone = true;
#endif
}

// ------------------------------------------------------------------- cli
static bool ciEq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static uint32_t takeNum(char* s, uint32_t def, uint32_t lo, uint32_t hi) {
  if (!s) return def;
  char* end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s || *end || v < (long)lo || v > (long)hi) return 0xFFFFFFFFu;
  return (uint32_t)v;
}

static void cmdHelp() {
  Serial.println("commands:");
  Serial.println("  status          link, keys, counters, last results");
  Serial.println("  tx [KB]         encrypted throughput bench (default 128 KB)");
  Serial.println("  stop            abort the running stream");
  Serial.println("  ping [n]        round-trip latency, n pings (default 5, max 50)");
  Serial.println("  msg <text>      send text reliably to the peer (chunked if long)");
  Serial.println("  key <psk>       set the pre-shared key (8..63 chars), save + reboot");
  Serial.println("  help            this list");
}

static void cmdStatus() {
  uint8_t peer[6]; e2e::peerMac(peer);
  char mac[18];
  snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
  Serial.printf("ok: link %s peer=%s fp=%08lX ch=%d frame<=%u payload<=%u\n",
                e2e::paired() ? "UP" : "DOWN", e2e::paired() ? mac : "-",
                (unsigned long)e2e::keyFingerprint(), E2E_CHANNEL,
                (unsigned)E2E_FRAME_MAX, (unsigned)e2e::payloadMax());
  Serial.printf("ok: psk source=%s, linkUps=%lu txDone=%lu rxFrames=%lu macFail=%lu sendErr=%lu rxOverflow=%lu msgDrop=%lu\n",
                e2e::pskSource(), (unsigned long)e2e::linkUpCount(),
                (unsigned long)e2e::txDoneCount(), (unsigned long)e2e::rxTotalFrames(),
                (unsigned long)e2e::macFailCount(), (unsigned long)e2e::sendErrCount(),
                (unsigned long)e2e::rxOverflowCount(), (unsigned long)e2e::msgDroppedCount());
  const e2e::TxReport& t = e2e::lastTx();
  const e2e::RxReport& r = e2e::lastRx();
  Serial.printf("ok: last tx: s=%u acked=%lu/%lu retx=%lu %s\n",
                t.stream, (unsigned long)t.acked, (unsigned long)t.frames,
                (unsigned long)t.retx, t.ok ? "ok" : "incomplete");
  Serial.printf("ok: last rx: s=%u unique=%lu/%lu dup=%lu verify=%s\n",
                r.stream, (unsigned long)r.unique, (unsigned long)r.frames,
                (unsigned long)r.dup, r.verifyFails ? "FAIL" : "ok");
  Serial.printf("ok: heap free=%u min=%u, uptime=%lu s\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)(millis() / 1000));
}

static void handleLine(char* buf) {
  g_userTouched = true;
  char* cmd = strtok(buf, " \t");
  if (!cmd) return;

  if (ciEq(cmd, "help") || ciEq(cmd, "?") || ciEq(cmd, "h")) { cmdHelp(); return; }
  if (ciEq(cmd, "status") || ciEq(cmd, "stat"))              { cmdStatus(); return; }

  if (ciEq(cmd, "tx")) {
    uint32_t kb = takeNum(strtok(NULL, " \t"), 128, 1, 16384);
    if (kb == 0xFFFFFFFFu) { Serial.println("ERR: kb must be 1..16384"); return; }
    startBench(kb);
    return;
  }
  if (ciEq(cmd, "stop")) {
    e2e::stopStream("stopped by user");
    Serial.println("ok: stop");
    return;
  }
  if (ciEq(cmd, "ping")) {
    uint32_t n = takeNum(strtok(NULL, " \t"), 5, 1, 50);
    if (n == 0xFFFFFFFFu) { Serial.println("ERR: n must be 1..50"); return; }
    pingStart(n);
    return;
  }
  if (ciEq(cmd, "msg") || ciEq(cmd, "send")) {
    char* text = strtok(NULL, "");
    if (!text || !*text) { Serial.println("ERR: msg needs text"); return; }
    if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return; }
    if (e2e::streamActive()) { Serial.println("ERR: busy - 'stop' the running stream first"); return; }
    if (!e2e::sendBlob((const uint8_t*)text, (uint16_t)strlen(text)))
      Serial.println("ERR: msg too long (max 1024) or cannot start");
    else
      Serial.printf("ok: msg queued (%u bytes, reliable)\n", (unsigned)strlen(text));
    return;
  }
  if (ciEq(cmd, "key")) {
    char* psk = strtok(NULL, " \t");
    if (!psk || !*psk) { Serial.println("ERR: key needs a passphrase (8..63 chars)"); return; }
    size_t n = strlen(psk);
    if (n < 8 || n > 63) { Serial.println("ERR: key length must be 8..63"); return; }
    Preferences p;
    p.begin("e2e", false);
    p.putString("psk", psk);
    p.end();
    Serial.println("ok: psk saved to nvs, rebooting - run the same 'key' on the other board");
    delay(400);
    ESP.restart();
    return;
  }

  Serial.printf("ERR: unknown command '%s' - type 'help'\n", cmd);
}

static void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) return;
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = 0;
      lineLen = 0;
      if (line[0]) handleLine(line);
      continue;
    }
    if (lineLen < CMDBUF_MAX - 1) line[lineLen++] = (char)c;
    else { lineLen = 0; Serial.println("ERR: line too long"); }
  }
}

// ------------------------------------------------------------------- led
static void ledUpdate() {
  uint32_t now = millis();
  uint32_t period = e2e::streamActive() ? 100 : (e2e::paired() ? 0 : 500);
  uint8_t level = period == 0 ? 1 : ((now % (period * 2)) < period);
  digitalWrite(LED_BUILTIN, level);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(BAUD);
  delay(200);
  bool ok = e2e::begin();
  Serial.printf("\n09ESP2ESPCommunication online: encrypted esp-now p2p link%s, 115200 baud\n",
                ok ? "" : " INIT FAILED");
  Serial.println("waiting for peer (both boards: same channel, same key) - type 'help'");
  e2e::onMessage(onMsg);
}

void loop() {
  e2e::poll();
  pollSerial();
  pingMachine();
  autoBench();
  ledUpdate();
  delay(1);
}
