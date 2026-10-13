// 12ESP2ESPRemoteControl — constant one-way message stream from one ESP32 to
// the other over encrypted ESP-NOW, tuned for maximum reliability at long
// distances (reliability over speed). Same firmware on both boards.
//
// Reliability stack (layered, see README.md for the reasoning):
//   PHY    1 Mbps DSSS (best receiver sensitivity, ~24 dB more link budget
//          than 54M), runtime `rate` command can trade back
//   LINK   the soak-proven 09 engine: AES-CCM-128 encryption, 64-frame
//          bitmap-ACK window, retransmission, ACK-rewind on peer reboot,
//          session auto-retry, HELLO supervision, millis-wrap safety
//   REDUND every message transmitted `copies` times (default 3) as distinct
//          frames - any single copy delivers the message
//   APP    per-message CRC32 + id window dedup + gap accounting; every
//          distinct received message is printed to the serial monitor
//
// Roles: `auto` (default) elects the sender by MAC (higher MAC transmits),
// or pin with `role tx` / `role rx` (persisted). Hardware select at boot:
// short GPIO21 to GND -> this board transmits, GPIO22 to GND -> receives
// (both open = auto; a grounded strap beats the persisted value). The
// reverse direction stays available for control: `msg <text>` and `ping`
// work from either board.
//
// Serial console (115200):
//   help | status | role [auto|tx|rx] | period [ms] | copies [n] |
//   stream on|off | msg <text> | ping [n] | rate [name] | key <psk> | verbose
//   | pins
#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#include "e2e_link.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static const uint32_t BAUD          = 115200;
static const uint32_t WDT_TIMEOUT_S = 8;      // loop() never blocks > ~0.5 s
static const size_t   CMDBUF_MAX    = 256;

// ----------------------------------------------------------------- defaults
enum Role : uint8_t { ROLE_AUTO = 0, ROLE_TX = 1, ROLE_RX = 2 };

// Role straps - short to GND at boot (INPUT_PULLUP, grounded = LOW).
// Genuinely non-analog pins with no boot-strap or flash function; each sits
// next to a GND pin on the right header, so a 2-pin jumper does the job:
//   GPIO21 -> GND : this board is the transmitter (tx)
//   GPIO22 -> GND : this board is the receiver    (rx)
//   both open     : auto (MAC election) / NVS `role`
//   both grounded : invalid - warned, falls back to auto/NVS
static const uint8_t PIN_STRAP_TX = 21;
static const uint8_t PIN_STRAP_RX = 22;
static uint8_t g_hwRole    = ROLE_AUTO;   // set by pinSetup() from the straps
static bool    g_hwConflict = false;      // both straps grounded
static const uint32_t DEF_PERIOD_MS = 200;    // one message every 200 ms
static const uint8_t  DEF_COPIES    = 3;      // redundancy copies per message
static const char     kTeleText[]   = "remote-control telemetry";

// ------------------------------------------------- telemetry message (wire)
// One frame per copy, K frames per message. Deterministic per (id, copy) so
// engine retransmits regenerate identical bytes.
struct __attribute__((packed)) Tele {
  uint32_t id;            // message id, increments at the sender
  uint8_t  copy;          // 0..copies-1: which redundancy copy this frame is
  uint8_t  copies;        // K as configured at the sender
  uint8_t  rsv;
  uint32_t uptimeMs;      // sender uptime at capture
  uint32_t heapFree;      // sender free heap at capture
  uint64_t pins;          // GPIO bitmask captured at the sender: bit N = GPIO N
  uint32_t crc;           // CRC32 of all preceding bytes
  char     text[37];      // fixed label (NUL-terminated)
};
static_assert(sizeof(Tele) == 64, "telemetry layout drifted");
// fail the build if the message can no longer fit a frame (see e2e_link.h)
static_assert(sizeof(Tele) <= 250 - e2e::HDR_SIZE, "Tele outgrew the frame");

// Every GPIO the ESP-WROOM-32 exposes and the app may touch. GPIO 1/3 are the
// serial console, 6-11 are flash - neither is read. 34-39 are input-only (no
// internal pulls: unconnected they read floating noise - tie them if you need
// stable values). GPIO 2 is LED_BUILTIN, driven by this firmware; its bit
// reports the LED state.
static const uint8_t kPins[] = {
  0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22,
  23, 25, 26, 27, 32, 33, 34, 35, 36, 39
};

static void pinSetup() {
  for (uint8_t p : kPins) {
    if (p == LED_BUILTIN) continue;                // driven as the LED
    if (p == PIN_STRAP_TX || p == PIN_STRAP_RX)
      pinMode(p, INPUT_PULLUP);                    // role straps: open reads 1
    else
      pinMode(p, p >= 34 ? INPUT : INPUT_PULLDOWN); // unconnected pins read 0
  }
  bool tx = digitalRead(PIN_STRAP_TX) == LOW;
  bool rx = digitalRead(PIN_STRAP_RX) == LOW;
  g_hwRole     = (tx != rx) ? (tx ? ROLE_TX : ROLE_RX) : ROLE_AUTO;
  g_hwConflict = tx && rx;
}

static uint64_t readPins() {
  uint64_t m = 0;
  for (uint8_t p : kPins)
    if (digitalRead(p)) m |= 1ULL << p;
  return m;
}

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

// ----------------------------------------------------------------- config
static uint8_t  g_role    = ROLE_AUTO;  // NVS "role"
static uint32_t g_periodMs = DEF_PERIOD_MS;  // NVS "period"
static uint8_t  g_copies  = DEF_COPIES;      // NVS "copies"
static bool     g_streamOn = true;           // runtime only, default on
static bool     g_verbose  = false;          // print duplicate copies too

static bool cfgReadU32(const char* key, uint32_t* out) {
  nvs_handle_t h;
  if (nvs_open("e12", NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = nvs_get_u32(h, key, out) == ESP_OK;
  nvs_close(h);
  return ok;
}

static bool cfgWriteU32(const char* key, uint32_t v) {
  nvs_handle_t h;
  if (nvs_open("e12", NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_set_u32(h, key, v);
  nvs_commit(h);
  nvs_close(h);
  return true;
}

static void loadCfg() {
  uint32_t v;
  if (cfgReadU32("role", &v) && v <= ROLE_RX) g_role = (uint8_t)v;
  if (cfgReadU32("period", &v) && v >= 100 && v <= 60000) g_periodMs = v;
  if (cfgReadU32("copies", &v) && v >= 1 && v <= 4) g_copies = (uint8_t)v;
}

static const char* roleName(uint8_t r) {
  return r == ROLE_TX ? "tx" : r == ROLE_RX ? "rx" : "auto";
}

// Effective role: `auto` resolves by MAC election once paired (the board with
// the higher MAC transmits - deterministic, both sides agree, no extra airtime).
static uint8_t effRole() {
  if (g_role != ROLE_AUTO) return g_role;
  if (!e2e::paired()) return 0;  // unresolved
  uint8_t own[6], peer[6];
  WiFi.macAddress(own);
  e2e::peerMac(peer);
  return memcmp(own, peer, 6) > 0 ? ROLE_TX : ROLE_RX;
}

// ------------------------------------------------------------------- tx side
static Tele     g_snap;                 // current outbound message (session source)
static bool     g_due = false;          // snapshot waiting for the radio
static uint32_t g_nextId = 1;
static uint32_t g_lastPeriodMs = 0;
static uint32_t g_txCreated = 0, g_txDrop = 0;
static uint32_t g_txOk = 0, g_txFail = 0, g_txRetx = 0, g_txMs = 0;
static uint32_t g_txDoneSeen = 0;
static uint32_t g_sessStartMs = 0, g_lastStallMs = 0;
static uint32_t g_txSessId = 0;      // message id of the running tele session
static bool     g_teleSess = false;

static uint16_t teleGen(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen) {
  if (maxLen < sizeof(Tele)) return 0;
  Tele t = *(const Tele*)ctx;
  t.copy = (uint8_t)seq;               // seq 0..K-1 selects the copy index
  t.crc = crc32((const uint8_t*)&t, offsetof(Tele, crc));
  memcpy(out, &t, sizeof t);
  return sizeof t;
}

static void txMachine(uint32_t now) {
  // completion feedback for every finished session (telemetry or `msg`).
  // Note: if the engine auto-retries a dead tele session, the retry
  // regenerates from the *current* g_snap (freshest message), not the
  // stalled one - intentional freshness semantics; the stale one is in drop.
  uint32_t done = e2e::txDoneCount();
  while (g_txDoneSeen < done) {
    g_txDoneSeen++;
    bool wasTele = g_teleSess;
    g_teleSess = false;
    const e2e::TxReport& t = e2e::lastTx();
    if (t.ok) {
      g_txOk++; g_txRetx += t.retx; g_txMs += t.ms;
      if (t.retx || t.ms > 500) {  // normal completes are silent; margin use is not
        if (wasTele)
          Serial.printf("[tx] #%lu slow path: acked=%lu/%lu retx=%lu macfail=%lu ms=%lu\n",
                        (unsigned long)g_txSessId, (unsigned long)t.acked,
                        (unsigned long)t.frames,
                        (unsigned long)t.retx, (unsigned long)t.macFail,
                        (unsigned long)t.ms);
        else
          Serial.printf("[tx] msg slow path: acked=%lu/%lu retx=%lu macfail=%lu ms=%lu\n",
                        (unsigned long)t.acked, (unsigned long)t.frames,
                        (unsigned long)t.retx, (unsigned long)t.macFail,
                        (unsigned long)t.ms);
      }
    } else {
      g_txFail++;                   // engine already printed the TX ABORT line
    }
  }

  // visibility while a telemetry session grinds through retransmits
  if (g_teleSess && e2e::streamActive() && g_sessStartMs &&
      now - g_sessStartMs > 2000 && now - g_lastStallMs >= 2000) {
    g_lastStallMs = now;
    Serial.printf("[tx] #%lu in flight %.1fs - marginal link, retransmitting\n",
                  (unsigned long)g_txSessId, (now - g_sessStartMs) / 1000.0);
  }

  uint8_t r = effRole();
  if (r != ROLE_TX || !g_streamOn || !e2e::paired()) return;

  // capture a fresh snapshot every period tick; freshness wins over an older
  // message still waiting for the radio (counted as drop, gaps visible at rx)
  if ((int32_t)(now - g_lastPeriodMs) >= (int32_t)g_periodMs) {
    if (g_due) g_txDrop++;
    g_snap.id = g_nextId++;
    g_snap.copy = 0; g_snap.copies = g_copies; g_snap.rsv = 0;
    g_snap.uptimeMs = now;
    g_snap.heapFree = ESP.getFreeHeap();
    g_snap.pins = readPins();
    g_snap.crc = 0;
    strncpy(g_snap.text, kTeleText, sizeof g_snap.text - 1);
    g_snap.text[sizeof g_snap.text - 1] = 0;
    g_due = true;
    g_lastPeriodMs = now;
    g_txCreated++;
  }

  if (g_due && !e2e::streamActive() && !e2e::retryPending()) {
    if (e2e::startStream(e2e::T_TEXT, teleGen, &g_snap, g_copies, "tel")) {
      g_due = false;
      g_sessStartMs = now;
      g_txSessId = g_snap.id;
      g_teleSess = true;
    }
  }
}

// ------------------------------------------------------------------- rx side
static uint32_t g_rxBase = 0;      // oldest id in the gap-accounting window
static uint64_t g_rxWin = 0;       // bitmap: id g_rxBase+i received (engine-level)
static bool     g_rxMaxSet = false;
static uint32_t g_rxMax = 0;
static uint32_t g_rxMsgs = 0, g_rxCopies = 0, g_rxDups = 0, g_rxLate = 0;
static uint32_t g_rxGaps = 0, g_rxCrcFails = 0, g_rxFirstNot0 = 0;
static uint32_t g_lastRxPulseMs = 0;
static uint32_t g_lastTeleMs = 0;            // last fresh telemetry arrival
static uint32_t g_lastSilentHintMs = 0;      // stale-silence hint rate limit

// Printed-once bookkeeping: a 64-id window that slides with the stream.
// (A bare id%64 map collides - #65 with #1, #66 with #2 ... - which the
// 35-min soak caught: the receiver printed ids 1..64 and suppressed the
// rest. The base moves forward, so ids only "collide" across 64-message
// *slides*, which a sequential stream never does.)
static uint32_t g_pBase = 0;
static uint64_t g_pMap = 0;
static bool     g_pValid = false;

static bool alreadyPrinted(uint32_t id) {
  if (!g_pValid) return false;
  uint32_t d = id - g_pBase;                 // wrap-safe unsigned diff
  return d < 64 && ((g_pMap >> d) & 1);      // older than the window: printed
}

static void markPrinted(uint32_t id) {
  if (!g_pValid) { g_pValid = true; g_pBase = id; g_pMap = 1; return; }
  uint32_t d = id - g_pBase;
  if (d < 64) { g_pMap |= 1ULL << d; return; }
  uint32_t shift = d - 63;                   // id ahead: slide the window
  g_pMap = shift >= 64 ? 0 : (g_pMap >> shift);
  g_pBase = id - 63;
  g_pMap |= 1ULL << 63;
}

static void printTele(const Tele& t, bool late) {
  static uint64_t lastPins = 0;
  static bool     haveLast = false;
  Serial.printf("[rx] #%lu copy%u/%u up=%lus heap=%luB pins=0x%010llX \"%s\" crc=ok%s\n",
                (unsigned long)t.id, (unsigned)t.copy, (unsigned)t.copies,
                (unsigned long)(t.uptimeMs / 1000), (unsigned long)t.heapFree,
                (unsigned long long)t.pins, t.text, late ? " (late)" : "");
  if (haveLast) {
    uint64_t d = t.pins ^ lastPins;
    if (d) {
      uint32_t shown = 0, total = 0;
      Serial.print("[rx] pin change:");
      for (uint8_t p : kPins) {
        if (d & (1ULL << p)) {
          total++;
          if (shown < 8) {
            Serial.printf(" %u:%u->%u", p,
                          (unsigned)((lastPins >> p) & 1),
                          (unsigned)((t.pins >> p) & 1));
            shown++;
          }
        }
      }
      if (total > shown)
        Serial.printf(" (+%lu more)", (unsigned long)(total - shown));
      Serial.println();
    }
  }
  lastPins = t.pins;
  haveLast = true;
}

static void printTeleDup(const Tele& t) {
  Serial.printf("[rx+] #%lu copy%u/%u (duplicate copy suppressed)\n",
                (unsigned long)t.id, (unsigned)t.copy, (unsigned)t.copies);
}

static void handleTele(const Tele& t, uint32_t now) {
  g_rxCopies++;
  uint32_t id = t.id;

  // LED heartbeat reflects *reception* (any valid copy), not print gating
  g_lastRxPulseMs = now;

  // sender reboot heuristic: id jumped backwards beyond possible reordering
  // (the rx window is 64, so copies can never be more than 64 ids late - a
  // bigger backward jump means the sender's id space restarted). The old
  // 100000 threshold never fired in short runs; soak run 2 showed every
  // post-reboot message miscounted as `late` instead.
  if (g_rxMaxSet && id < g_rxMax && g_rxMax - id > 64u) {
    Serial.printf("[rx] note: id went %lu -> %lu, sender rebooted - resync window\n",
                  (unsigned long)g_rxMax, (unsigned long)id);
    g_rxMaxSet = false;
    g_rxBase = 0; g_rxWin = 0;
    g_pValid = false;                       // printed map refers to old ids
  }

  // Print gate: every distinct message is printed exactly once, whenever its
  // first copy arrives - even after newer data (e.g. a message whose session
  // completed around our own reboot). Sliding printed-window, see above.
  if (alreadyPrinted(id)) {
    g_rxDups++;
    if (g_verbose) printTeleDup(t);
    return;
  }
  markPrinted(id);

  bool late = false;
  if (!g_rxMaxSet) {
    g_rxMaxSet = true; g_rxMax = id;
    g_rxBase = id; g_rxWin = 1;
  } else if (id > g_rxMax) {
    uint32_t missed = id - g_rxMax - 1;
    if (missed) {
      g_rxGaps += missed;
      Serial.printf("[rx] ! %lu message(s) missing before #%lu (all copies + retx failed)\n",
                    (unsigned long)missed, (unsigned long)id);
    }
    if (id >= g_rxBase + 64) { g_rxBase = id - 63; g_rxWin = 0; }
    g_rxWin |= 1ULL << (id - g_rxBase);
    g_rxMax = id;
  } else if (id >= g_rxBase) {              // inside window: a hole backfilled
    uint64_t bit = 1ULL << (id - g_rxBase);
    if (!(g_rxWin & bit) && g_rxGaps) g_rxGaps--;
    g_rxWin |= bit;
    late = true; g_rxLate++;
  } else {                                  // below window, never printed
    late = true; g_rxLate++;                // (stalled message racing our reboot)
  }

  g_rxMsgs++;
  if (t.copy) g_rxFirstNot0++;   // first arrival was copy>0: redundancy paid off
  g_lastTeleMs = now;
  printTele(t, late);
}

static void onMsg(const uint8_t src[6], uint32_t seq, uint32_t total,
                  const uint8_t* data, uint16_t len) {
  (void)src;
  if (len == sizeof(Tele)) {
    Tele t;
    memcpy(&t, data, sizeof t);
    if (t.crc == crc32((const uint8_t*)&t, offsetof(Tele, crc))) {
      handleTele(t, millis());
      return;
    }
    g_rxCrcFails++;   // impossible behind AES-CCM; if it ever ticks, tell someone
    return;
  }
  // ad-hoc text from the peer's `msg` command (control channel, rx -> tx ok)
  Serial.printf("[msg] from peer (%lu/%lu): \"%.*s\"\n",
                (unsigned long)seq, (unsigned long)total, (int)len, data);
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

static void pingMachine(uint32_t now) {
  if (!g_pinging) return;
  const e2e::PingStats& ps = e2e::pingStats();
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

static void cmdRate(char* a) {
  if (!a || !*a) {
    Serial.printf("ok: rate=%u - names:", (unsigned)e2e::phyRate());
    for (const RateName& r : kRates) Serial.printf(" %s", r.n);
    Serial.println("\n     1m (default here) = max range; higher = throughput");
    return;
  }
  for (const RateName& r : kRates) {
    if (!strcmp(a, r.n)) {
      if (e2e::setPhyRate(r.v, true))
        Serial.printf("ok: phy rate %s (%u) applied and saved (survives reboot)\n",
                      r.n, (unsigned)r.v);
      else
        Serial.println("ERR: esp_wifi_config_espnow_rate refused this rate");
      return;
    }
  }
  Serial.println("ERR: unknown rate - 'rate' with no argument lists valid names");
}

// --------------------------------------------------------------------- cli
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
  Serial.println("  status          link, roles, tx/rx delivery stats, counters");
  Serial.println("  role [auto|tx|rx] show or pin the role (or strap GPIO21/22 to GND at boot)");
  Serial.println("  period [ms]     TX message period 100..60000 (default 200, saved)");
  Serial.println("  copies [n]      redundancy copies per message 1..4 (default 3, saved)");
  Serial.println("  stream on|off   start/stop the telemetry stream");
  Serial.println("  msg <text>      reliable text to the peer (control channel)");
  Serial.println("  ping [n]        round-trip latency under load (default 5, max 50)");
  Serial.println("  rate [name]     PHY rate; 1m = max range (default), no arg = list");
  Serial.println("  key <psk>       set the pre-shared key (8..63 chars), save + reboot");
  Serial.println("  verbose         toggle printing duplicate copies as well");
  Serial.println("  stop            abort the running stream session");
  Serial.println("  pins            local GPIO snapshot (the web monitor polls this)");
  Serial.println("  help            this list");
}

static void cmdStatus() {
  uint8_t peer[6]; e2e::peerMac(peer);
  uint8_t er = effRole();
  Serial.printf("ok: link %s peer=%02X:%02X:%02X:%02X:%02X:%02X fp=%08lX ch=%d rate=%u(%s) payload<=%u\n",
                e2e::paired() ? "UP" : "DOWN",
                peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
                (unsigned long)e2e::keyFingerprint(), E2E_CHANNEL,
                (unsigned)e2e::phyRate(), e2e::pskSource(), (unsigned)e2e::payloadMax());
  Serial.printf("ok: cfg role=%s(%s) stream=%s period=%lums copies=%u verbose=%s\n",
                roleName(g_role),
                g_role == ROLE_AUTO ? (er ? roleName(er) : "unresolved") : "pinned",
                g_streamOn ? "on" : "off", (unsigned long)g_periodMs,
                (unsigned)g_copies, g_verbose ? "on" : "off");
  Serial.printf("ok: tx created=%lu ok=%lu fail=%lu retx=%lu avgMs=%lu drop=%lu%s\n",
                (unsigned long)g_txCreated, (unsigned long)g_txOk,
                (unsigned long)g_txFail, (unsigned long)g_txRetx,
                (unsigned long)(g_txOk ? g_txMs / g_txOk : 0),
                (unsigned long)g_txDrop,
                e2e::retryPending() ? " (retry pending)" : "");
  Serial.printf("ok: rx msgs=%lu copies=%lu dup=%lu late=%lu gaps=%lu crcErr=%lu firstNotCopy0=%lu\n",
                (unsigned long)g_rxMsgs, (unsigned long)g_rxCopies,
                (unsigned long)g_rxDups, (unsigned long)g_rxLate,
                (unsigned long)g_rxGaps, (unsigned long)g_rxCrcFails,
                (unsigned long)g_rxFirstNot0);
  Serial.printf("ok: engine linkUps=%lu txDone=%lu rxFrames=%lu macFail=%lu sendErr=%lu rxOverflow=%lu msgDrop=%lu\n",
                (unsigned long)e2e::linkUpCount(), (unsigned long)e2e::txDoneCount(),
                (unsigned long)e2e::rxTotalFrames(), (unsigned long)e2e::macFailCount(),
                (unsigned long)e2e::sendErrCount(), (unsigned long)e2e::rxOverflowCount(),
                (unsigned long)e2e::msgDroppedCount());
  const e2e::TxReport& t = e2e::lastTx();
  const e2e::RxReport& r = e2e::lastRx();
  Serial.printf("ok: last engine tx: s=%u acked=%lu/%lu retx=%lu %s\n",
                t.stream, (unsigned long)t.acked, (unsigned long)t.frames,
                (unsigned long)t.retx, t.ok ? "ok" : "incomplete");
  Serial.printf("ok: last engine rx: s=%u unique=%lu/%lu dup=%lu complete=%d\n",
                r.stream, (unsigned long)r.unique, (unsigned long)r.frames,
                (unsigned long)r.dup, r.complete);
  Serial.printf("ok: heap free=%u min=%u, uptime=%lu s, millis-wraps=%lu\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)(millis() / 1000), (unsigned long)e2e::millisWraps());
}

static char line[CMDBUF_MAX];
static size_t lineLen = 0;

static void handleLine(char* buf) {
  char* cmd = strtok(buf, " \t");
  if (!cmd) return;

  if (ciEq(cmd, "help") || ciEq(cmd, "?") || ciEq(cmd, "h")) { cmdHelp(); return; }
  if (ciEq(cmd, "status") || ciEq(cmd, "stat"))              { cmdStatus(); return; }
  if (ciEq(cmd, "pins") || ciEq(cmd, "gpio")) {
    Serial.printf("[local] pins=0x%010llX up=%lus\n",
                  (unsigned long long)readPins(), (unsigned long)(millis() / 1000));
    return;
  }

  if (ciEq(cmd, "role")) {
    char* a = strtok(NULL, " \t");
    if (!a || !*a) {
      uint8_t er = effRole();
      Serial.printf("ok: role=%s, effective=%s (auto = higher MAC transmits)\n",
                    roleName(g_role),
                    g_role == ROLE_AUTO ? (er ? roleName(er) : "unresolved until paired") : "pinned");
      return;
    }
    if (ciEq(a, "auto"))      g_role = ROLE_AUTO;
    else if (ciEq(a, "tx"))   g_role = ROLE_TX;
    else if (ciEq(a, "rx"))   g_role = ROLE_RX;
    else { Serial.println("ERR: role must be auto, tx or rx"); return; }
    cfgWriteU32("role", g_role);
    Serial.printf("ok: role=%s saved (stream restarts automatically)\n", roleName(g_role));
    return;
  }
  if (ciEq(cmd, "period")) {
    uint32_t v = takeNum(strtok(NULL, " \t"), g_periodMs, 100, 60000);
    if (v == 0xFFFFFFFFu) { Serial.println("ERR: period must be 100..60000 ms"); return; }
    g_periodMs = v;
    cfgWriteU32("period", v);
    Serial.printf("ok: period=%lums saved\n", (unsigned long)v);
    return;
  }
  if (ciEq(cmd, "copies")) {
    uint32_t v = takeNum(strtok(NULL, " \t"), g_copies, 1, 4);
    if (v == 0xFFFFFFFFu) { Serial.println("ERR: copies must be 1..4"); return; }
    g_copies = (uint8_t)v;
    cfgWriteU32("copies", v);
    Serial.printf("ok: copies=%u saved (applies to the next message)\n", (unsigned)v);
    return;
  }
  if (ciEq(cmd, "stream")) {
    char* a = strtok(NULL, " \t");
    if (a && ciEq(a, "on"))       g_streamOn = true;
    else if (a && ciEq(a, "off")) g_streamOn = false;
    else { Serial.printf("ERR: 'stream on' or 'stream off' (currently %s)\n",
                         g_streamOn ? "on" : "off"); return; }
    Serial.printf("ok: stream %s\n", g_streamOn ? "on" : "off");
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
  if (ciEq(cmd, "ping")) {
    uint32_t n = takeNum(strtok(NULL, " \t"), 5, 1, 50);
    if (n == 0xFFFFFFFFu) { Serial.println("ERR: n must be 1..50"); return; }
    pingStart(n);
    return;
  }
  if (ciEq(cmd, "rate"))  { cmdRate(strtok(NULL, " \t")); return; }
  if (ciEq(cmd, "verbose")) {
    g_verbose = !g_verbose;
    Serial.printf("ok: verbose %s (duplicate copies %s)\n",
                  g_verbose ? "on" : "off", g_verbose ? "printed" : "suppressed");
    return;
  }
  if (ciEq(cmd, "key")) {
    char* psk = strtok(NULL, " \t");
    if (!psk || !*psk) { Serial.println("ERR: key needs a passphrase (8..63 chars)"); return; }
    size_t n = strlen(psk);
    if (n < 8 || n > 63) { Serial.println("ERR: key length must be 8..63"); return; }
    nvs_handle_t h;
    if (nvs_open("e12", NVS_READWRITE, &h) != ESP_OK) {
      Serial.println("ERR: cannot open nvs");
      return;
    }
    nvs_set_str(h, "psk", psk);
    nvs_commit(h);
    nvs_close(h);
    Serial.println("ok: psk saved to nvs, rebooting - run the same 'key' on the other board");
    delay(400);
    ESP.restart();
    return;
  }
  if (ciEq(cmd, "stop")) {
    e2e::stopStream("stopped by user");
    Serial.println("ok: stop");
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

// ------------------------------------------------------------- stats + led
static uint32_t g_lastStatMs = 0;

static void statMachine(uint32_t now) {
  // stale-silence hint: a paired receiver hearing nothing for 10 s speaks up
  // (both boards pinned rx, sender crashed/stream off, or link one-way-dead).
  // Reference point = later of (re)pairing and last telemetry, so the hint
  // also fires when no message has *ever* arrived.
  static bool     wasPaired = false;
  static uint32_t pairedAtMs = 0;
  bool pr = e2e::paired();
  if (pr && !wasPaired) pairedAtMs = now;
  wasPaired = pr;
  uint32_t ref = g_lastTeleMs > pairedAtMs ? g_lastTeleMs : pairedAtMs;
  if (pr && effRole() == ROLE_RX && now - ref > 10000 &&
      now - g_lastSilentHintMs >= 30000) {
    g_lastSilentHintMs = now;
    Serial.printf("note: paired but no telemetry for %lus - sender down, 'stream off' on the sender, or both boards pinned rx?\n",
                  (unsigned long)((now - ref) / 1000));
  }
  if (now - g_lastStatMs < 30000) return;
  g_lastStatMs = now;
  uint8_t er = effRole();
  Serial.printf("[stat] up=%lus role=%s link=%s | rx msgs=%lu copies=%lu dup=%lu late=%lu gaps=%lu crcErr=%lu | tx ok=%lu fail=%lu retx=%lu drop=%lu | heap=%luB\n",
                (unsigned long)(now / 1000),
                er ? roleName(er) : roleName(g_role),
                e2e::paired() ? "up" : "down",
                (unsigned long)g_rxMsgs, (unsigned long)g_rxCopies,
                (unsigned long)g_rxDups, (unsigned long)g_rxLate,
                (unsigned long)g_rxGaps, (unsigned long)g_rxCrcFails,
                (unsigned long)g_txOk, (unsigned long)g_txFail,
                (unsigned long)g_txRetx, (unsigned long)g_txDrop,
                (unsigned long)ESP.getFreeHeap());
}

static void ledUpdate(uint32_t now) {
  uint8_t level;
  uint8_t r = effRole();
  if (!e2e::paired()) {
    uint32_t period = 500;                     // seeking peer
    level = (now % (period * 2)) < period;
  } else if (r == ROLE_TX) {
    level = g_streamOn ? HIGH : (now % 200) < 100;  // solid while streaming
  } else if (r == ROLE_RX) {
    level = (now - g_lastRxPulseMs) < 40 ? HIGH : LOW;  // pulse per message
  } else {
    uint32_t period = 500;
    level = (now % (period * 2)) < period;
  }
  digitalWrite(LED_BUILTIN, level);
}

// -------------------------------------------------------------- setup/loop
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinSetup();                       // all other header GPIOs as pulled-down inputs
  Serial.begin(BAUD);
  delay(200);
  loadCfg();
  if (g_hwConflict) {
    Serial.printf("cfg: role straps GPIO%u and GPIO%u both shorted to GND - invalid, keeping auto/NVS\n",
                  (unsigned)PIN_STRAP_TX, (unsigned)PIN_STRAP_RX);
  } else if (g_hwRole != ROLE_AUTO) {
    g_role = g_hwRole;             // grounded strap beats the persisted role
  }

  bool ok = e2e::begin();
  e2e::setQuiet(true);          // the continuous stream must not spam TX/RX lines
  e2e::onMessage(onMsg);

  Serial.printf("\n12ESP2ESPRemoteControl online: long-range reliable esp-now telemetry%s\n",
                ok ? "" : " INIT FAILED");
  Serial.printf("cfg: role=%s%s period=%lums copies=%u (redundancy) rate=%u phy, AES-CCM-128 + ARQ + CRC32\n",
                roleName(g_role),
                g_hwRole != ROLE_AUTO ? " (strap)" : "",
                (unsigned long)g_periodMs, (unsigned)g_copies,
                (unsigned)e2e::phyRate());
  Serial.printf("pins: %u gpio inputs reported (pull-down; 34-39 float when open), LED on gpio %u\n",
                (unsigned)(sizeof kPins / sizeof kPins[0]), (unsigned)LED_BUILTIN);
  Serial.println("waiting for peer (both boards: same channel, same key) - type 'help'");
  g_lastStatMs = millis() - 28000;   // first [stat] line ~2 s after boot

  // ruggedization: a hung loop() reboots the board instead of silent death
  esp_err_t wInit = esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_err_t wAdd  = esp_task_wdt_add(nullptr);
  Serial.printf("wdt: armed (init rc=%d, add rc=%d)\n", (int)wInit, (int)wAdd);
}

void loop() {
  esp_task_wdt_reset();
  uint32_t now = millis();
  e2e::poll();
  pollSerial();
  txMachine(now);
  pingMachine(now);
  statMachine(now);
  ledUpdate(now);
  delay(1);
}
