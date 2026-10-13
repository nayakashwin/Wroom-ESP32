// 10ESPPhoneComms — phone↔ESP32↔ESP32↔phone comms relay (see PLAN.md).
//
// Phase 1: the 09 link engine runs on both boards so they pair and carry
// reliable encrypted traffic. Same image on both boards. Serial console
// (115200): help | status | tx [KB] | stop | ping [n] | msg <text> | rate
// [name] | key <psk> | btsend <text>. On first pairing the higher-MAC board
// auto-runs a benchmark. The frame layer arrives in phase 4+.
// Phase 2 adds BluetoothSerial SPP: the phone side of the relay.
#include <Arduino.h>
#include <BluetoothSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

#include <stdio.h>
#include <string.h>

#include "e2e_link.h"
#include "frame.h"

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
static const uint32_t WDT_TIMEOUT_S = 8;   // loop() never blocks > ~0.5 s

static char   line[CMDBUF_MAX];
static size_t lineLen = 0;

// -------------------------------------------------------------- bluetooth
static BluetoothSerial SerialBT;
static bool   g_btClientWas = false;
static frame::Parser g_btParser;

// forward a complete frame out over SPP to the phone
static void btSendFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  uint8_t buf[frame::HDR_SIZE + frame::MAX_PAYLOAD];
  size_t n = frame::encode(type, payload, len, buf, sizeof buf);
  if (!n) { Serial.println("ERR: frame encode failed"); return; }
  if (g_btClientWas) SerialBT.write(buf, n);
}

// ---- bridge state (phase 7) ------------------------------------------------
static uint32_t g_framesFromBt = 0, g_textToBt = 0, g_voiceToBt = 0;
static uint32_t g_unpairedDrops = 0;   // phone frames dropped: link unpaired
static bool     g_localPtt = false;       // local phone holds PTT
static uint32_t g_remoteTalkMs = 0;       // peer PTT/audio seen at (2 s hold)
static bool     g_pecho = false;          // bench: answer app PING with PONG
static uint32_t g_voiceLogMs = 0, g_voiceLogN = 0;

// voice-path rx stats (jitter of chunk arrivals, reported by voiceRxReport)
static uint32_t g_vrxCount = 0, g_vrxLastMs = 0, g_vrxJMin = 1, g_vrxJMax = 0, g_vrxJSum = 0;
static uint32_t g_vrxFirstMs = 0, g_vrxLastSeq = 0;
static bool     g_vrxReported = true;

// app-frame ping machine (phone-to-phone latency probe, CLI flavour)
static bool     g_fpingActive = false;
static uint32_t g_fpingTarget = 0, g_fpingSent = 0, g_fpingOk = 0;
static uint32_t g_fpingNextMs = 0, g_fpingEndMs = 0;
static uint32_t g_fpingMin = (uint32_t)-1, g_fpingMax = 0, g_fpingSum = 0;

static void onBtFrame(void*, uint8_t type, const uint8_t* payload, uint16_t len) {
  g_framesFromBt++;
  if (type == frame::T_VOICE) {          // 50/s: do not flood the console
    if (millis() - g_voiceLogMs >= 1000) {
      g_voiceLogMs = millis();
      Serial.printf("[btframe] VOICE x%lu/s len=%u\n",
                    (unsigned long)(g_voiceLogN ? g_voiceLogN : 1), (unsigned)len);
      g_voiceLogN = 0;
    } else g_voiceLogN++;
  } else {
    Serial.printf("[btframe] %s len=%u \"%.*s\"\n", frame::typeName(type),
                  (unsigned)len, (int)len, (const char*)payload);
  }

  if (type == frame::T_TEXT) {
    if (!e2e::paired()) { Serial.println("ERR: [btframe] not paired - text dropped"); return; }
    if (e2e::streamActive()) { Serial.println("ERR: [btframe] stream busy - text dropped"); return; }
    if (!e2e::sendBlob(payload, len)) Serial.println("ERR: [btframe] sendBlob failed");
    return;
  }

  // everything else rides the best-effort path, inner frame intact
  if (type == frame::T_PTT_BEGIN) g_localPtt = true;
  if (type == frame::T_PTT_END)   g_localPtt = false;
  if (!e2e::paired()) { g_unpairedDrops++; return; }
  uint8_t buf[frame::HDR_SIZE + 227];
  size_t n = frame::encode(type, payload, len, buf, sizeof buf);
  if (n) {
    if (!e2e::sendVoice(buf, (uint16_t)n) && type != frame::T_VOICE)
      Serial.println("ERR: [btframe] sendVoice dropped a control frame");
  }
}

// a best-effort chunk from the peer: it carries a complete end-to-end frame
static void onVoiceRx(uint32_t seq, const uint8_t* data, uint16_t len) {
  uint32_t now = millis();
  if (g_vrxCount == 0) { g_vrxFirstMs = now; g_vrxJMin = (uint32_t)-1; g_vrxJMax = 0; g_vrxJSum = 0; }
  else {
    uint32_t d = now - g_vrxLastMs;
    g_vrxJSum += d;
    if (d < g_vrxJMin) g_vrxJMin = d;
    if (d > g_vrxJMax) g_vrxJMax = d;
  }
  g_vrxLastMs = now;
  g_vrxLastSeq = seq;
  g_vrxCount++;
  g_vrxReported = false;

  if (len < frame::HDR_SIZE || data[0] != frame::M1 || data[1] != frame::M2) {
    Serial.printf("WARN: voice chunk %u B is not a frame\n", (unsigned)len);
    return;
  }
  uint8_t  type = data[2];
  uint16_t plen = (uint16_t)(data[3] | (data[4] << 8));
  if ((uint16_t)(frame::HDR_SIZE + plen) != len) {
    Serial.println("WARN: voice chunk len mismatch");
    return;
  }

  g_voiceToBt++;
  if (g_btClientWas) SerialBT.write(data, len);   // raw toward the phone

  if (type == frame::T_VOICE) {
    g_remoteTalkMs = millis();
    if (millis() - g_voiceLogMs >= 1000) { g_voiceLogMs = millis(); g_voiceLogN = 0; }
    else g_voiceLogN++;
    return;
  }
  Serial.printf("[vframe] %s len=%u\n", frame::typeName(type), (unsigned)plen);

  if (type == frame::T_PTT_BEGIN) g_remoteTalkMs = millis();
  if (type == frame::T_PING && g_pecho) {
    uint8_t pong[frame::HDR_SIZE + 4];
    size_t n = frame::encode(frame::T_PONG, data + frame::HDR_SIZE, plen, pong, sizeof pong);
    if (n) e2e::sendVoice(pong, (uint16_t)n);
  }
  if (type == frame::T_PONG && g_fpingActive && plen >= 4) {
    uint32_t t0 = (uint32_t)data[frame::HDR_SIZE] | ((uint32_t)data[frame::HDR_SIZE + 1] << 8) |
                  ((uint32_t)data[frame::HDR_SIZE + 2] << 16) | ((uint32_t)data[frame::HDR_SIZE + 3] << 24);
    uint32_t rtt = millis() - t0;
    g_fpingOk++;
    if (rtt < g_fpingMin) g_fpingMin = rtt;
    if (rtt > g_fpingMax) g_fpingMax = rtt;
    g_fpingSum += rtt;
    Serial.printf("ok: fping %lu/%lu rtt=%lums\n",
                  (unsigned long)g_fpingOk, (unsigned long)g_fpingTarget,
                  (unsigned long)rtt);
  }
}

static void fpingMachine() {
  if (!g_fpingActive) return;
  uint32_t now = millis();
  if (g_fpingSent < g_fpingTarget && (int32_t)(now - g_fpingNextMs) >= 0) {
    uint32_t t0 = now;
    uint8_t buf[frame::HDR_SIZE + 4];
    size_t n = frame::encode(frame::T_PING, (const uint8_t*)&t0, 4, buf, sizeof buf);
    if (n && e2e::sendVoice(buf, (uint16_t)n)) {
      g_fpingSent++;
      g_fpingNextMs = now + 200;
    } else g_fpingNextMs = now + 20;
  }
  if ((g_fpingSent >= g_fpingTarget && g_fpingOk >= g_fpingTarget) ||
      (int32_t)(now - g_fpingEndMs) >= 0) {
    if (g_fpingOk)
      Serial.printf("ok: fping done: %lu/%lu ok, rtt min/avg/max = %lu/%lu/%lu ms\n",
                    (unsigned long)g_fpingOk, (unsigned long)g_fpingTarget,
                    (unsigned long)g_fpingMin,
                    (unsigned long)(g_fpingSum / g_fpingOk), (unsigned long)g_fpingMax);
    else
      Serial.println("ERR: fping: no PONG (peer needs 'pecho on')");
    g_fpingActive = false;
  }
}

static void btSetup() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof name, "ESPPhone-%02X%02X", mac[4], mac[5]);
  bool ok = SerialBT.begin(name);   // SPP server, standard UUID
  Serial.printf("bt: SPP server %s as \"%s\"\n", ok ? "started" : "FAILED", name);
  g_btParser.begin(onBtFrame, nullptr);
}

// phone-facing INFO frames: pushed on BT connect and on link state changes
static bool g_wasPaired = false;
static uint32_t g_infoEveryMs = 0;

static void sendInfoNow(const char* why) {
  uint8_t peer[6]; e2e::peerMac(peer);
  char line[96];
  snprintf(line, sizeof line, "link=%s peer=%02X:%02X:%02X:%02X:%02X:%02X heap=%u (%s)",
           e2e::linkUp() ? "up" : "down",
           peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
           (unsigned)ESP.getFreeHeap(), why);
  btSendFrame(frame::T_INFO, (const uint8_t*)line, (uint16_t)strlen(line));
}

static void infoMachine() {
  if (!g_btClientWas) { g_wasPaired = e2e::linkUp(); return; }
  bool p = e2e::linkUp();
  if (p != g_wasPaired || (g_btClientWas && millis() - g_infoEveryMs >= 10000)) {
    g_infoEveryMs = millis();
    g_wasPaired = p;
    sendInfoNow(p ? "change" : "change");
  }
}

static void btReportClient() {
  bool has = SerialBT.hasClient();
  if (has != g_btClientWas) {
    g_btClientWas = has;
    Serial.printf("bt: client %s\n", has ? "CONNECTED" : "disconnected");
    if (has) sendInfoNow("bt-connect");
  }
}

static void btPoll() {
  btReportClient();
  while (SerialBT.available()) {
    int c = SerialBT.read();
    if (c < 0) return;
    g_btParser.feed((uint8_t)c);
  }
}

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
    btSendFrame(frame::T_TEXT, g_asm, (uint16_t)g_asmLen);   // onward to the phone
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

// ------------------------------------------------------------- voice bench
// voicebench [s] [hz]: pump synthetic voice-sized chunks through the
// best-effort path; the peer prints a loss/jitter summary when it stops.
static bool     g_vbActive = false;
static uint32_t g_vbEndMs = 0, g_vbNextMs = 0, g_vbSent = 0, g_vbDrop = 0;
static uint32_t g_vbHz = 50;
static const uint32_t g_vbLen = 160;   // 20 ms of 8 kHz µ-law (5 B hdr + 155 B)
static uint8_t  g_vbPayload[160];

static void voiceBenchMachine() {
  if (!g_vbActive) return;
  uint32_t now = millis();
  if ((int32_t)(now - g_vbEndMs) >= 0) {
    Serial.printf("ok: voicebench done: sent=%lu dropped=%lu (%lu Hz, %lu B)\n",
                  (unsigned long)g_vbSent, (unsigned long)g_vbDrop,
                  (unsigned long)g_vbHz, (unsigned long)g_vbLen);
    g_vbActive = false;
    return;
  }
  if ((int32_t)(now - g_vbNextMs) >= 0) {
    if (e2e::sendVoice(g_vbPayload, g_vbLen)) g_vbSent++;
    else g_vbDrop++;
    g_vbNextMs += 1000 / g_vbHz;
    if ((int32_t)(now - g_vbNextMs) > 10) g_vbNextMs = now;   // fell behind: resync
  }
}

static void voiceRxReport() {
  if (g_vrxReported || !g_vrxCount) return;
  if ((int32_t)(millis() - g_vrxLastMs) < 1500) return;   // still streaming
  const e2e::VoiceStats& v = e2e::voiceStats();
  Serial.printf("VOICE RX: %lu frames, lastSeq=%lu, engine: rxSeen=%lu rxLost=%lu rxDup=%lu, inter-arrival ms min/avg/max = %lu/%lu/%lu\n",
                (unsigned long)g_vrxCount, (unsigned long)g_vrxLastSeq,
                (unsigned long)v.rxSeen, (unsigned long)v.rxLost, (unsigned long)v.rxDup,
                (unsigned long)g_vrxJMin,
                (unsigned long)(g_vrxJSum / (g_vrxCount ? g_vrxCount - 1 : 1)),
                (unsigned long)g_vrxJMax);
  g_vrxReported = true;
}

// ----------------------------------------------------------------- bench
static bool startBench(uint32_t kb) {  if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return false; }
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

// ------------------------------------------------------------------- rate
struct RateName { const char* n; uint8_t v; };
static const RateName kRates[] = {
  {"1m",  WIFI_PHY_RATE_1M_L},  {"2m",  WIFI_PHY_RATE_2M_L},
  {"5m5", WIFI_PHY_RATE_5M_L},  {"11m", WIFI_PHY_RATE_11M_L},
  {"6m",  WIFI_PHY_RATE_6M},    {"9m",  WIFI_PHY_RATE_9M},
  {"12m", WIFI_PHY_RATE_12M},   {"18m", WIFI_PHY_RATE_18M},
  {"24m", WIFI_PHY_RATE_24M},   {"36m", WIFI_PHY_RATE_36M},
  {"48m", WIFI_PHY_RATE_48M},   {"54m", WIFI_PHY_RATE_54M},
  {"mcs0", WIFI_PHY_RATE_MCS0_LGI}, {"mcs1", WIFI_PHY_RATE_MCS1_LGI},
  {"mcs2", WIFI_PHY_RATE_MCS2_LGI}, {"mcs3", WIFI_PHY_RATE_MCS3_LGI},
  {"mcs4", WIFI_PHY_RATE_MCS4_LGI}, {"mcs5", WIFI_PHY_RATE_MCS5_LGI},
  {"mcs6", WIFI_PHY_RATE_MCS6_LGI}, {"mcs7", WIFI_PHY_RATE_MCS7_LGI},
  {"mcs7s", WIFI_PHY_RATE_MCS7_SGI},
};

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

static void cmdRate(char* a) {
  if (!a || !*a) {
    Serial.printf("ok: rate=%u - names:", (unsigned)e2e::phyRate());
    for (const RateName& r : kRates) Serial.printf(" %s", r.n);
    Serial.println("\n     lower = more range/interference margin, throughput stays ~equal");
    return;
  }
  for (const RateName& r : kRates) {
    if (ciEq(a, r.n)) {
      if (e2e::setPhyRate(r.v, true))
        Serial.printf("ok: phy rate %s (%u) applied and saved (survives reboot)\n", r.n, (unsigned)r.v);
      else
        Serial.println("ERR: esp_wifi_config_espnow_rate refused this rate");
      return;
    }
  }
  Serial.println("ERR: unknown rate - 'rate' with no argument lists valid names");
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
  Serial.println("  rate [name]     PHY rate (range vs speed); no arg = list names");
  Serial.println("  key <psk>       set the pre-shared key (8..63 chars), save + reboot");
  Serial.println("  voicebench [s] [hz]  best-effort voice path bench (def 10 s @ 50 Hz)");
  Serial.println("  ptt on|off      software PTT (same path the phone uses)");
  Serial.println("  fping [n]       app-frame PING/PONG over the voice path (peer: 'pecho')");
  Serial.println("  pecho           toggle PING->PONG answering (bench aid)");
  Serial.println("  txt <text>      build a TEXT frame, dump it, send to peer (phone-inject)");
  Serial.println("  btsend <text>   write a line to the bluetooth client");
  Serial.println("  help            this list");
}

static void cmdStatus() {
  uint8_t peer[6]; e2e::peerMac(peer);
  char mac[18];
  snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
  Serial.printf("ok: link %s peer=%s fp=%08lX ch=%d frame<=%u payload<=%u rate=%u\n",
                e2e::linkUp() ? "UP" : "DOWN", e2e::linkUp() ? mac : "-",
                (unsigned long)e2e::keyFingerprint(), E2E_CHANNEL,
                (unsigned)E2E_FRAME_MAX, (unsigned)e2e::payloadMax(),
                (unsigned)e2e::phyRate());
  Serial.printf("ok: psk source=%s, linkUps=%lu txDone=%lu rxFrames=%lu macFail=%lu sendErr=%lu rxOverflow=%lu msgDrop=%lu\n",
                e2e::pskSource(), (unsigned long)e2e::linkUpCount(),
                (unsigned long)e2e::txDoneCount(), (unsigned long)e2e::rxTotalFrames(),
                (unsigned long)e2e::macFailCount(), (unsigned long)e2e::sendErrCount(),
                (unsigned long)e2e::rxOverflowCount(), (unsigned long)e2e::msgDroppedCount());
  const e2e::TxReport& t = e2e::lastTx();
  const e2e::RxReport& r = e2e::lastRx();
  Serial.printf("ok: last tx: s=%u acked=%lu/%lu retx=%lu %s%s\n",
                t.stream, (unsigned long)t.acked, (unsigned long)t.frames,
                (unsigned long)t.retx, t.ok ? "ok" : "incomplete",
                e2e::retryPending() ? " (retry pending)" : "");
  Serial.printf("ok: last rx: s=%u unique=%lu/%lu dup=%lu verify=%s\n",
                r.stream, (unsigned long)r.unique, (unsigned long)r.frames,
                (unsigned long)r.dup, r.verifyFails ? "FAIL" : "ok");
  const e2e::VoiceStats& v = e2e::voiceStats();
  Serial.printf("ok: voice tx=%lu drop=%lu rx=%lu lost=%lu dup=%lu\n",
                (unsigned long)v.txSent, (unsigned long)v.txDropped,
                (unsigned long)v.rxSeen, (unsigned long)v.rxLost,
                (unsigned long)v.rxDup);
  Serial.printf("ok: bridge btIn=%lu textOut=%lu voiceOut=%lu unpdrop=%lu ptt=%s remoteTalk=%s pecho=%s\n",
                (unsigned long)g_framesFromBt, (unsigned long)g_textToBt,
                (unsigned long)g_voiceToBt, (unsigned long)g_unpairedDrops,
                g_localPtt ? "held" : "idle",
                (millis() - g_remoteTalkMs < 2000) ? "yes" : "no",
                g_pecho ? "on" : "off");
  Serial.printf("ok: heap free=%u min=%u, uptime=%lu s, millis-wraps=%lu, bt=%s\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)(millis() / 1000),
                (unsigned long)e2e::millisWraps(),
                g_btClientWas ? "connected" : "none");
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
  if (ciEq(cmd, "rate")) {
    cmdRate(strtok(NULL, " \t"));
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

  if (ciEq(cmd, "btsend")) {
    char* text = strtok(NULL, "");
    if (!text || !*text) { Serial.println("ERR: btsend needs text"); return; }
    if (!g_btClientWas) { Serial.println("ERR: no bluetooth client connected"); return; }
    SerialBT.println(text);
    Serial.printf("ok: sent to bt (%u bytes)\n", (unsigned)strlen(text));
    return;
  }

  if (ciEq(cmd, "txt")) {
    char* text = strtok(NULL, "");
    if (!text || !*text) { Serial.println("ERR: txt needs text"); return; }
    uint16_t len = (uint16_t)strlen(text);
    uint8_t f[frame::HDR_SIZE + frame::MAX_PAYLOAD];
    size_t n = frame::encode(frame::T_TEXT, (const uint8_t*)text, len, f, sizeof f);
    Serial.printf("ok: frame %zu bytes:", n);
    for (size_t i = 0; i < n && i < 40; i++) Serial.printf(" %02X", f[i]);
    Serial.println();
    if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return; }
    if (e2e::streamActive()) { Serial.println("ERR: busy - 'stop' the running stream first"); return; }
    if (!e2e::sendBlob((const uint8_t*)text, len))
      Serial.println("ERR: txt too long (max 1024) or cannot start");
    return;
  }

  if (ciEq(cmd, "voicebench")) {
    uint32_t s  = takeNum(strtok(NULL, " \t"), 10, 1, 60);
    uint32_t hz = takeNum(strtok(NULL, " \t"), 50, 10, 100);
    if (s == 0xFFFFFFFFu)  { Serial.println("ERR: seconds must be 1..60"); return; }
    if (hz == 0xFFFFFFFFu) { Serial.println("ERR: hz must be 10..100"); return; }
    if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return; }
    g_vbHz = hz; g_vbSent = g_vbDrop = 0;
    frame::encode(frame::T_VOICE, g_vbPayload, sizeof g_vbPayload - frame::HDR_SIZE,
                  g_vbPayload, sizeof g_vbPayload);   // chunk = real e2e frame
    g_vbActive = true;
    g_vbNextMs = millis(); g_vbEndMs = millis() + s * 1000;
    Serial.printf("ok: voicebench %lus @ %lu Hz x %lu B\n",
                  (unsigned long)s, (unsigned long)hz, (unsigned long)g_vbLen);
    return;
  }

  if (ciEq(cmd, "ptt")) {
    char* arg = strtok(NULL, " \t");
    bool on  = arg && (ciEq(arg, "on")  || ciEq(arg, "1"));
    bool off = arg && (ciEq(arg, "off") || ciEq(arg, "0"));
    if (!on && !off) { Serial.println("ERR: ptt needs on|off"); return; }
    if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return; }
    onBtFrame(nullptr, on ? frame::T_PTT_BEGIN : frame::T_PTT_END,
              (const uint8_t*)"", 0);   // inject exactly as the phone would
    Serial.printf("ok: ptt %s\n", on ? "on" : "off");
    return;
  }
  if (ciEq(cmd, "fping")) {
    uint32_t n = takeNum(strtok(NULL, " \t"), 5, 1, 50);
    if (n == 0xFFFFFFFFu) { Serial.println("ERR: n must be 1..50"); return; }
    if (!e2e::paired()) { Serial.println("ERR: not paired - wait for 'LINK up'"); return; }
    g_fpingTarget = n; g_fpingSent = g_fpingOk = 0;
    g_fpingMin = (uint32_t)-1; g_fpingMax = g_fpingSum = 0;
    g_fpingActive = true; g_fpingNextMs = millis();
    g_fpingEndMs = millis() + n * 250 + 1000;
    return;
  }
  if (ciEq(cmd, "pecho")) {
    g_pecho = !g_pecho;
    Serial.printf("ok: pecho %s (answers app PING frames with PONG)\n",
                  g_pecho ? "on" : "off");
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
// unpaired: 1 Hz blink | paired idle: slow 0.25 Hz heartbeat | local PTT:
// solid | remote talking (PTT_BEGIN or audio within 2 s): 5 Hz | ARQ stream
// in flight: fast blink (as in 09)
static void ledUpdate() {
  uint32_t now = millis();
  uint32_t period;
  if (e2e::streamActive()) period = 100;
  else if (g_localPtt) period = 0;
  else if (now - g_remoteTalkMs < 2000) period = 200;
  else if (!e2e::linkUp()) period = 500;
  else period = 2000;
  uint8_t level = period == 0 ? 1 : ((now % (period * 2)) < period);
  digitalWrite(LED_BUILTIN, level);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(BAUD);
  delay(200);
  btSetup();   // BEFORE e2e::begin(): BT controller must enable before
               // esp_wifi_start() or coex_core_enable() aborts (boot loop)
  bool ok = e2e::begin();
  Serial.printf("\n10ESPPhoneComms online: esp-now link (vendored from 09), phase 2%s, 115200 baud\n",
                ok ? "" : " INIT FAILED");
  Serial.println("waiting for peer (both boards: same channel, same key) - type 'help'");
  e2e::onMessage(onMsg);
  e2e::onVoice(onVoiceRx);

  // ruggedization: if loop() ever hangs (radio driver, stuck print, ...), the
  // task watchdog panics and reboots the board instead of silent death.
  esp_err_t wInit = esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_err_t wAdd  = esp_task_wdt_add(nullptr);  // setup()/loop() task
  Serial.printf("wdt: armed (init rc=%d, add rc=%d) - a hung loop() reboots the board\n",
                (int)wInit, (int)wAdd);
}

void loop() {
  esp_task_wdt_reset();  // ... and prove it to the task watchdog
  e2e::poll();
  pollSerial();
  btPoll();
  pingMachine();
  autoBench();
  voiceBenchMachine();
  voiceRxReport();
  fpingMachine();
  infoMachine();
  ledUpdate();
  delay(1);
}
