// e2e_link.cpp — encrypted point-to-point ESP-NOW link engine.
//
// Wire format (little-endian, packed):
//   Hdr{magic,type,flags,stream,seq,total,len} + payload
//   T_HELLO  broadcast, plaintext: HelloBody{key fingerprint, max frame}
//   T_DATA/T_TEXT encrypted unicast session chunk (seq/total for ARQ)
//   T_ACK    encrypted unicast: hdr.seq = receiver base, AckBody.got bitmap
//   T_PING/T_PONG encrypted unicast: PingBody{seq, sender t0}
//
// Reliability: 64-frame sliding windows on both sides. The receiver slides
// its base only across *received* frames and answers bitmap ACKs every
// <=8 ms while a session is active (plus a 2 s grace after completion, so a
// lost final ACK still gets re-answered after the sender retransmits). The
// sender never transmits past ackBase+63 (so the receiver never sees a frame
// outside its window) and retransmits gaps at >=25 ms spacing.
#include "e2e_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#ifndef E2E_PHY_RATE
#define E2E_PHY_RATE WIFI_PHY_RATE_54M
#endif
#define E2E_STR(x) #x
#define E2E_XSTR(x) E2E_STR(x)

namespace e2e {
namespace {

// ------------------------------------------------------------- wire format
struct __attribute__((packed)) Hdr {
  uint32_t magic;
  uint8_t  type;
  uint8_t  flags;
  uint16_t stream;
  uint32_t seq;
  uint32_t total;
  uint16_t len;
};
static_assert(sizeof(Hdr) == HDR_SIZE, "header layout drifted");

const uint32_t kMagic    = 0x45324539u;  // "9E2E" on the wire (LE)
const uint8_t  F_PATTERN = 1u << 0;      // payload is the bench pattern

struct __attribute__((packed)) HelloBody { uint32_t fp; uint16_t frameMax; uint16_t caps; };
struct __attribute__((packed)) AckBody {
  uint64_t got; uint32_t bytes; uint32_t dup;
  uint16_t verifyFails; uint16_t pad;
};
struct __attribute__((packed)) PingBody { uint32_t seq; uint32_t t0; };

// ------------------------------------------------------------------ tuning
const uint32_t kHelloSeekMs   = 250;   // HELLO cadence while unpaired
const uint32_t kHelloIdleMs   = 1000;  // supervision cadence while paired
const uint32_t kLinkTimeoutMs = 3000;
const uint32_t kAckPeriodMs  = 8;
const uint32_t kAckFrames    = 32;
const uint32_t kRetxMinMs    = 50;     // min spacing between attempts of a frame
const uint32_t kAbortMs      = 5000;   // no ack progress -> give up
const uint32_t kGraceMs      = 2000;   // keep acking after a completed stream
const uint32_t kPingTimeoutMs = 500;
const uint32_t kWindow       = 64;     // arq window (one uint64 bitmap)
const uint32_t kInFlightMax  = 8;      // attempts handed to esp_now_send at once:
                                       // the wifi mac serialises them, deeper
                                       // queues only add latency and premature
                                       // retransmits (measured dup storm)

// --------------------------------------------------------------- send state
struct TxState {
  bool active; uint8_t type; uint16_t stream;
  uint32_t total, next, ackBase, acked;
  uint64_t ackGot;
  PayloadGen gen; void* ctx;
  uint32_t retx, bytes, firstBytes, startMs, lastProgressMs, lastAckMs, lastPrintMs;
  uint32_t lastTry[kWindow]; uint8_t attempts[kWindow];
  char name[10];
};

struct RxState {
  bool active, doneReported;
  uint16_t stream; uint32_t total, base;
  uint64_t got;
  uint32_t unique, dup, bytes, verifyFails, startMs, lastMs;
};

// ------------------------------------------------------------------ storage
uint8_t  g_frameBuf[250];
char     g_psk[64];
const char* g_pskSource = "default (E2E_PSK)";
uint8_t  g_pmk[16], g_lmk[16];
uint32_t g_fp;
uint8_t  g_own[6];
const uint8_t kBcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

volatile bool    g_paired = false;
uint8_t  g_peer[6];
uint16_t g_peerFrameMax = E2E_FRAME_MAX;
uint32_t g_linkUps = 0, g_lastPeerMs = 0, g_lastHelloMs = 0, g_lastAckMs = 0;
uint16_t g_streamSeq = 0;
uint32_t g_txDone = 0;

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_inFlight = 0;
volatile uint32_t g_macFailTotal = 0, g_macFailSession = 0;
volatile uint32_t g_sendErr = 0, g_rxOverflow = 0, g_rxTotal = 0;
volatile uint32_t g_rxSinceAck = 0;

struct Heard { uint8_t mac[6]; uint32_t fp; uint16_t frameMax; bool valid; };
Heard g_heard;
volatile bool g_peerFpBad = false;   // paired peer's HELLO carries another key

struct AckMail { uint16_t stream; uint32_t base; uint64_t got; };
AckMail g_ack; bool g_ackNew = false;

struct PingResp { uint32_t seq, t0; bool valid; } g_pingResp;  // answer a ping
struct PongMail { uint32_t seq, t0, at; bool valid; } g_pong;
struct OutPing  { uint32_t seq, t0; bool valid; } g_outPing;

struct MsgSlot { uint16_t len; uint32_t seq, total; uint8_t src[6]; uint8_t data[E2E_FRAME_MAX - HDR_SIZE]; };
struct MsgFifo {
  MsgSlot slot[16]; uint8_t head, tail; uint32_t dropped;
} g_msg;

RxState g_rx;
bool g_rxStartPrint = false;
RxReport g_rxLast, g_rxDrop; bool g_rxDropNew = false;
TxState g_tx;
TxReport g_txLast;
MessageHandler g_msgH = nullptr;
PingStats g_ping;

char* macStr(const uint8_t m[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return out;
}

void deriveKeys(const char* psk) {
  char buf[96];
  uint8_t sha[32];
  snprintf(buf, sizeof buf, "e2e-pmk|%s", psk);
  mbedtls_sha256((const uint8_t*)buf, strlen(buf), sha, 0);
  memcpy(g_pmk, sha, 16);
  snprintf(buf, sizeof buf, "e2e-lmk|%s", psk);
  mbedtls_sha256((const uint8_t*)buf, strlen(buf), sha, 0);
  memcpy(g_lmk, sha, 16);
  uint32_t h = 2166136261u;               // FNV-1a of the LMK = key fingerprint
  for (int i = 0; i < 16; i++) { h ^= g_lmk[i]; h *= 16777619u; }
  g_fp = h;
}

uint16_t payloadCap() {
  uint16_t fm = g_peerFrameMax < E2E_FRAME_MAX ? g_peerFrameMax : E2E_FRAME_MAX;
  if (fm > 250) fm = 250;
  return fm - HDR_SIZE;
}

// Deterministic bench payload; the receiver regenerates this to verify
// end-to-end integrity (on top of the AES-CCM MIC).
inline uint8_t benchByte(uint32_t seq, uint16_t i) {
  return (uint8_t)(seq * 131u + i * 17u + (seq >> 8));
}

bool sendFrame(const uint8_t to[6], uint8_t type, uint16_t stream, uint32_t seq,
               uint32_t total, uint8_t flags, const uint8_t* payload, uint16_t len) {
  Hdr h;
  h.magic = kMagic; h.type = type; h.flags = flags; h.stream = stream;
  h.seq = seq; h.total = total; h.len = len;
  memcpy(g_frameBuf, &h, HDR_SIZE);
  if (len) memcpy(g_frameBuf + HDR_SIZE, payload, len);
  esp_err_t e = esp_now_send(to, g_frameBuf, HDR_SIZE + len);
  if (e != ESP_OK) { g_sendErr++; return false; }
  portENTER_CRITICAL(&g_mux);
  g_inFlight++;
  portEXIT_CRITICAL(&g_mux);
  return true;
}

// ------------------------------------------------------------ rx frame path
void rxSessionFrame(const uint8_t mac[6], const Hdr* h, const uint8_t* p, uint32_t now) {
  portENTER_CRITICAL(&g_mux);
  if (!g_rx.active || g_rx.stream != h->stream) {
    if (g_rx.active && !g_rx.doneReported) {   // unfinished stream replaced
      g_rxDrop.stream = g_rx.stream; g_rxDrop.frames = g_rx.total;
      g_rxDrop.unique = g_rx.unique; g_rxDrop.dup = g_rx.dup;
      g_rxDrop.bytes = g_rx.bytes; g_rxDrop.verifyFails = g_rx.verifyFails;
      g_rxDrop.ms = now - g_rx.startMs; g_rxDrop.complete = false;
      g_rxDropNew = true;
    }
    g_rx = RxState{};
    g_rx.active = true; g_rx.stream = h->stream; g_rx.total = h->total;
    g_rx.startMs = g_rx.lastMs = now;
    g_rxStartPrint = true;
  }
  if (h->total) g_rx.total = h->total;
  uint32_t seq = h->seq;
  if (seq < g_rx.base) {
    g_rx.dup++;
  } else if (seq - g_rx.base >= kWindow) {
    g_rxOverflow++;            // impossible while the sender honours its window
  } else {
    uint64_t bit = 1ULL << (seq - g_rx.base);
    if (g_rx.got & bit) {
      g_rx.dup++;
    } else {
      g_rx.got |= bit;
      g_rx.unique++; g_rx.bytes += h->len;
      if (h->flags & F_PATTERN) {
        bool ok = true;
        for (uint16_t i = 0; i < h->len; i++)
          if (p[i] != benchByte(seq, i)) { ok = false; break; }
        if (!ok) g_rx.verifyFails++;
      }
      if (h->type == T_TEXT && g_msgH && h->len <= sizeof g_msg.slot[0].data) {
        uint8_t nxt = (uint8_t)((g_msg.head + 1) % 16);
        if (nxt == g_msg.tail) {
          g_msg.dropped++;     // app not draining fast enough
        } else {
          MsgSlot& s = g_msg.slot[g_msg.head];
          memcpy(s.data, p, h->len);
          s.len = h->len; s.seq = seq; s.total = h->total;
          memcpy(s.src, mac, 6);
          g_msg.head = nxt;
        }
      }
    }
    while (g_rx.got & 1ULL) { g_rx.got >>= 1; g_rx.base++; }  // slide over received
  }
  g_rx.lastMs = now;
  g_rxTotal++;
  g_rxSinceAck++;
  portEXIT_CRITICAL(&g_mux);
}

void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (!mac || len < (int)HDR_SIZE) return;
  const Hdr* h = (const Hdr*)data;
  if (h->magic != kMagic || h->len > 250 - HDR_SIZE || (int)(HDR_SIZE + h->len) != len) return;
  uint32_t now = millis();

  bool fromPeer;
  portENTER_CRITICAL(&g_mux);
  fromPeer = g_paired && memcmp(mac, g_peer, 6) == 0;
  if (fromPeer) g_lastPeerMs = now;
  portEXIT_CRITICAL(&g_mux);

  if (h->type == T_HELLO) {
    if (h->len < sizeof(HelloBody)) return;
    HelloBody b; memcpy(&b, data + HDR_SIZE, sizeof b);
    portENTER_CRITICAL(&g_mux);
    if (!g_paired && !fromPeer) {   // single-peer link: ignore strangers once paired
      memcpy(g_heard.mac, mac, 6);
      g_heard.fp = b.fp;
      g_heard.frameMax = b.frameMax ? b.frameMax : 240;
      g_heard.valid = true;
    }
    // a paired peer whose fingerprint changed can no longer decrypt our
    // frames - treat the plaintext HELLO as a key mismatch, not supervision
    if (fromPeer && b.fp != g_fp) g_peerFpBad = true;
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  if (!fromPeer) return;

  switch (h->type) {
    case T_DATA:
    case T_TEXT:
      rxSessionFrame(mac, h, data + HDR_SIZE, now);
      break;
    case T_ACK: {
      if (h->len < sizeof(AckBody)) return;
      AckBody b; memcpy(&b, data + HDR_SIZE, sizeof b);
      portENTER_CRITICAL(&g_mux);
      g_ack.stream = h->stream; g_ack.base = h->seq; g_ack.got = b.got;
      g_ackNew = true;
      portEXIT_CRITICAL(&g_mux);
      break;
    }
    case T_PING: {
      if (h->len < sizeof(PingBody)) return;
      PingBody b; memcpy(&b, data + HDR_SIZE, sizeof b);
      portENTER_CRITICAL(&g_mux);
      g_pingResp.seq = b.seq; g_pingResp.t0 = b.t0; g_pingResp.valid = true;
      portEXIT_CRITICAL(&g_mux);
      break;
    }
    case T_PONG: {
      if (h->len < sizeof(PingBody)) return;
      PingBody b; memcpy(&b, data + HDR_SIZE, sizeof b);
      portENTER_CRITICAL(&g_mux);
      g_pong.seq = b.seq; g_pong.t0 = b.t0; g_pong.at = now; g_pong.valid = true;
      portEXIT_CRITICAL(&g_mux);
      break;
    }
    default: break;
  }
}

void onSent(const uint8_t* mac, esp_now_send_status_t st) {
  (void)mac;
  portENTER_CRITICAL(&g_mux);
  if (g_inFlight) g_inFlight--;
  if (st != ESP_NOW_SEND_SUCCESS) { g_macFailTotal++; g_macFailSession++; }
  portEXIT_CRITICAL(&g_mux);
}

// ------------------------------------------------------------ tx session
bool sendSessionFrame(uint32_t seq) {
  uint8_t slot = (uint8_t)(seq % kWindow);
  uint16_t cap = payloadCap();
  uint16_t n = g_tx.gen(g_tx.ctx, seq, g_frameBuf + HDR_SIZE, cap);
  if (n > cap) n = cap;
  uint32_t now = millis();
  if (!sendFrame(g_peer, g_tx.type, g_tx.stream, seq, g_tx.total,
                 g_tx.type == T_DATA ? F_PATTERN : 0, g_frameBuf + HDR_SIZE, n))
    return false;
  bool isNew = (seq == g_tx.next);
  if (isNew) {
    g_tx.bytes += n;
    g_tx.attempts[slot] = 0;   // slot may hold counters of an older frame
  }
  if (!g_tx.firstBytes) g_tx.firstBytes = n;
  g_tx.lastTry[slot] = now;
  g_tx.attempts[slot]++;
  if (isNew) g_tx.next++;
  return true;
}

void finishTx(uint32_t now, bool ok, const char* reason) {
  if (!g_tx.active) return;
  uint32_t macFail;
  portENTER_CRITICAL(&g_mux);
  macFail = g_macFailSession; g_macFailSession = 0;
  portEXIT_CRITICAL(&g_mux);
  g_txLast.stream = g_tx.stream;
  g_txLast.frames = g_tx.total; g_txLast.acked = g_tx.acked;
  g_txLast.retx = g_tx.retx; g_txLast.macFail = macFail;
  g_txLast.bytes = g_tx.bytes; g_txLast.ms = now - g_tx.startMs; g_txLast.ok = ok;
  g_tx.active = false;
  g_txDone++;
  if (ok) {
    double mbps = g_txLast.ms ? (g_txLast.bytes * 8.0) / (g_txLast.ms * 1000.0) : 0.0;
    Serial.printf("TX DONE s=%u %s acked=%lu/%lu retx=%lu macfail=%lu bytes=%lu ms=%lu rate=%.2fMbps\n",
                  g_txLast.stream, g_tx.name, (unsigned long)g_txLast.acked,
                  (unsigned long)g_txLast.frames, (unsigned long)g_txLast.retx,
                  (unsigned long)g_txLast.macFail, (unsigned long)g_txLast.bytes,
                  (unsigned long)g_txLast.ms, mbps);
  } else {
    Serial.printf("TX ABORT s=%u %s acked=%lu/%lu bytes=%lu ms=%lu (%s)\n",
                  g_txLast.stream, g_tx.name, (unsigned long)g_txLast.acked,
                  (unsigned long)g_txLast.frames, (unsigned long)g_txLast.bytes,
                  (unsigned long)g_txLast.ms, reason ? reason : "?");
  }
}

void txEngine(uint32_t now) {
  if (!g_tx.active) return;

  AckMail am; bool newAck = false;
  portENTER_CRITICAL(&g_mux);
  if (g_ackNew) { am = g_ack; g_ackNew = false; newAck = true; }
  portEXIT_CRITICAL(&g_mux);

  if (newAck && am.stream == g_tx.stream) {
    uint32_t ackedNew = am.base + __builtin_popcountll(am.got);
    if (ackedNew >= g_tx.acked) {   // ignore out-of-order (stale) acks
      if (ackedNew != g_tx.acked) g_tx.lastProgressMs = now;
      g_tx.ackBase = am.base; g_tx.ackGot = am.got;
      g_tx.acked = ackedNew; g_tx.lastAckMs = now;
    }
  }
  if (g_tx.acked >= g_tx.total) { finishTx(now, true, nullptr); return; }

  // retransmit gaps, oldest first; only when earlier attempts had time to
  // complete and be acked (kRetxMinMs) and the mac queue has drained
  uint32_t limit = g_tx.next < g_tx.ackBase + kWindow ? g_tx.next : g_tx.ackBase + kWindow;
  for (uint32_t s = g_tx.ackBase; s < limit; s++) {
    if (g_tx.ackGot & (1ULL << (s - g_tx.ackBase))) continue;
    uint8_t slot = (uint8_t)(s % kWindow);
    if (g_tx.attempts[slot] == 0) continue;
    if (now - g_tx.lastTry[slot] < kRetxMinMs) continue;
    if (g_inFlight >= kInFlightMax) break;
    if (sendSessionFrame(s)) g_tx.retx++;
    else break;
  }

  // send new frames inside the peer's window
  while (g_tx.next < g_tx.total && g_tx.next < g_tx.ackBase + kWindow &&
         g_inFlight < kInFlightMax) {
    if (!sendSessionFrame(g_tx.next)) break;
  }

  if (now - g_tx.lastProgressMs > kAbortMs) { finishTx(now, false, "no ack progress"); return; }

  if (now - g_tx.lastPrintMs >= 1000) {
    g_tx.lastPrintMs = now;
    uint32_t macFail;
    portENTER_CRITICAL(&g_mux);
    macFail = g_macFailSession;
    portEXIT_CRITICAL(&g_mux);
    uint32_t pct = g_tx.total ? (g_tx.acked * 100) / g_tx.total : 100;
    Serial.printf("TX s=%u %lu%% %lu/%lu retx=%lu mac=%lu q=%lu\n",
                  g_tx.stream, (unsigned long)pct, (unsigned long)g_tx.acked,
                  (unsigned long)g_tx.total, (unsigned long)g_tx.retx,
                  (unsigned long)macFail, (unsigned long)g_sendErr);
  }
}

}  // namespace

// ------------------------------------------------------------------- api
bool begin() {
  // read an optional runtime psk straight from nvs (Preferences::begin on a
  // missing namespace logs an error, nvs_open lets us probe quietly)
  nvs_handle_t h;
  if (nvs_open("e2e", NVS_READONLY, &h) == ESP_OK) {
    char v[64] = {0};
    size_t l = sizeof v;
    if (nvs_get_str(h, "psk", v, &l) == ESP_OK && l > 8 && l <= sizeof g_psk) {
      memcpy(g_psk, v, l - 1 < sizeof g_psk ? l - 1 : sizeof g_psk - 1);
      g_pskSource = "nvs";
    }
    nvs_close(h);
  }
  if (!g_psk[0]) strncpy(g_psk, E2E_PSK, sizeof g_psk - 1);
  deriveKeys(g_psk);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_err_t ec = esp_wifi_set_channel(E2E_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_set_pmk(g_pmk);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t bp = {};
  memcpy(bp.peer_addr, kBcast, 6);
  bp.channel = 0; bp.ifidx = WIFI_IF_STA; bp.encrypt = false;
  esp_now_add_peer(&bp);

  esp_err_t er = esp_wifi_config_espnow_rate(WIFI_IF_STA, E2E_PHY_RATE);
  WiFi.macAddress(g_own);

  char ms[18];
  Serial.printf("link: esp-now ch=%d frame<=%u payload<=%u psk-fp=%08lX(%s) rate=%s rc=%d/%d\n",
                E2E_CHANNEL, (unsigned)E2E_FRAME_MAX, (unsigned)(E2E_FRAME_MAX - HDR_SIZE),
                (unsigned long)g_fp, g_pskSource, E2E_XSTR(E2E_PHY_RATE),
                (int)er, (int)ec);
  return true;
}

void poll() {
  uint32_t now = millis();

  // -- a paired peer announcing a different key invalidates the link
  bool fpBad;
  portENTER_CRITICAL(&g_mux);
  fpBad = g_peerFpBad; g_peerFpBad = false;
  portEXIT_CRITICAL(&g_mux);
  if (fpBad && g_paired) {
    g_paired = false;
    char ms[18]; macStr(g_peer, ms);
    Serial.printf("LINK down: key changed on %s - set the same 'key <psk>' on both boards\n", ms);
    if (g_tx.active) finishTx(now, false, "key mismatch");
  }

  // -- pairing from a heard HELLO
  Heard hd; bool tryPair = false;
  portENTER_CRITICAL(&g_mux);
  if (g_heard.valid) { hd = g_heard; g_heard.valid = false; tryPair = !g_paired; }
  portEXIT_CRITICAL(&g_mux);
  if (tryPair) {
    if (hd.fp == g_fp) {
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, hd.mac, 6);
      p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = true;
      memcpy(p.lmk, g_lmk, 16);
      esp_err_t e = esp_now_is_peer_exist(hd.mac) ? esp_now_mod_peer(&p) : esp_now_add_peer(&p);
      if (e == ESP_OK) {
        memcpy(g_peer, hd.mac, 6);
        g_peerFrameMax = hd.frameMax > 250 ? 250 : hd.frameMax;
        g_paired = true; g_linkUps++; g_lastPeerMs = now;
        char ms[18]; macStr(g_peer, ms);
        Serial.printf("LINK up peer=%s fp=%08lX frame<=%u payload<=%u\n",
                      ms, (unsigned long)g_fp,
                      (unsigned)(g_peerFrameMax < E2E_FRAME_MAX ? g_peerFrameMax : E2E_FRAME_MAX),
                      (unsigned)payloadCap());
      } else {
        Serial.printf("ERR: esp_now_add_peer: %d\n", (int)e);
      }
    } else {
      static uint32_t lastWarn = 0; static uint32_t warnFp = 1;
      if (hd.fp != warnFp || now - lastWarn > 2000) {
        char ms[18]; macStr(hd.mac, ms);
        Serial.printf("WARN: key mismatch with %s (peer fp=%08lX, ours %08lX) - set the same 'key <psk>' on both boards\n",
                      ms, (unsigned long)hd.fp, (unsigned long)g_fp);
        warnFp = hd.fp; lastWarn = now;
      }
    }
  }

  // -- link supervision
  if (g_paired && (int32_t)(now - g_lastPeerMs) > (int32_t)kLinkTimeoutMs) {
    g_paired = false;
    char ms[18]; macStr(g_peer, ms);
    Serial.printf("LINK down (no frame from %s for %ums)\n", ms, (unsigned)kLinkTimeoutMs);
    if (g_tx.active) finishTx(now, false, "link lost");
  }

  // -- HELLO beacon
  uint32_t period = g_paired ? kHelloIdleMs : kHelloSeekMs;
  if ((int32_t)(now - g_lastHelloMs) >= (int32_t)period) {
    g_lastHelloMs = now;
    HelloBody b; b.fp = g_fp; b.frameMax = E2E_FRAME_MAX; b.caps = 0;
    sendFrame(kBcast, T_HELLO, 0, 0, 0, 0, (const uint8_t*)&b, sizeof b);
  }

  // -- answer pings, resolve pongs
  PingResp r; bool doPong = false;
  portENTER_CRITICAL(&g_mux);
  if (g_pingResp.valid) { r = g_pingResp; g_pingResp.valid = false; doPong = true; }
  portEXIT_CRITICAL(&g_mux);
  if (doPong) {
    PingBody b; b.seq = r.seq; b.t0 = r.t0;
    sendFrame(g_peer, T_PONG, 0, 0, 0, 0, (const uint8_t*)&b, sizeof b);
  }
  PongMail pg; bool gotPong = false;
  portENTER_CRITICAL(&g_mux);
  if (g_pong.valid) { pg = g_pong; g_pong.valid = false; gotPong = true; }
  portEXIT_CRITICAL(&g_mux);
  if (gotPong && g_outPing.valid && g_outPing.seq == pg.seq && g_outPing.t0 == pg.t0) {
    g_outPing.valid = false;
    uint32_t rtt = pg.at - pg.t0;
    g_ping.ok++; g_ping.lastRttMs = rtt;
    if (!g_ping.minRttMs || rtt < g_ping.minRttMs) g_ping.minRttMs = rtt;
    if (rtt > g_ping.maxRttMs) g_ping.maxRttMs = rtt;
    g_ping.sumRttMs += rtt;
  }
  if (g_outPing.valid && now - g_outPing.t0 > kPingTimeoutMs) {
    g_outPing.valid = false;
    g_ping.timeouts++;
  }

  // -- receiver: snapshot state, send bitmap acks, report
  RxState s;
  portENTER_CRITICAL(&g_mux);
  s = g_rx;
  bool startPrint = false, donePrint = false, dropPrint = false;
  if (g_rxStartPrint) { g_rxStartPrint = false; startPrint = true; }
  if (g_rxDropNew) { g_rxDropNew = false; dropPrint = true; }
  if (g_rx.active && g_rx.total && g_rx.base >= g_rx.total && !g_rx.doneReported) {
    g_rx.doneReported = true;
    g_rxLast.stream = g_rx.stream; g_rxLast.frames = g_rx.total;
    g_rxLast.unique = g_rx.unique; g_rxLast.dup = g_rx.dup;
    g_rxLast.bytes = g_rx.bytes; g_rxLast.verifyFails = g_rx.verifyFails;
    g_rxLast.ms = now - g_rx.startMs; g_rxLast.complete = true;
    donePrint = true;
  }
  portEXIT_CRITICAL(&g_mux);

  if (startPrint)
    Serial.printf("RX s=%u total=%lu payload<=%u\n", s.stream,
                  (unsigned long)(s.total ? s.total : 0), (unsigned)payloadCap());
  if (dropPrint)
    Serial.printf("RX s=%u superseded after %lu/%lu frames (dup=%lu verifyFails=%lu)\n",
                  g_rxDrop.stream, (unsigned long)g_rxDrop.unique,
                  (unsigned long)g_rxDrop.frames, (unsigned long)g_rxDrop.dup,
                  (unsigned long)g_rxDrop.verifyFails);
  if (donePrint) {
    const RxReport& q = g_rxLast;
    Serial.printf("RX DONE s=%u frames=%lu/%lu dup=%lu verify=%s bytes=%lu ms=%lu\n",
                  q.stream, (unsigned long)q.unique, (unsigned long)q.frames,
                  (unsigned long)q.dup, q.verifyFails ? "FAIL" : "ok",
                  (unsigned long)q.bytes, (unsigned long)q.ms);
  }

  uint32_t rxSinceAck;
  portENTER_CRITICAL(&g_mux);
  rxSinceAck = g_rxSinceAck;
  portEXIT_CRITICAL(&g_mux);
  if (s.active && (rxSinceAck >= kAckFrames || now - g_lastAckMs >= kAckPeriodMs)) {
    portENTER_CRITICAL(&g_mux);
    g_rxSinceAck = 0;
    portEXIT_CRITICAL(&g_mux);
    AckBody b;
    b.got = s.got; b.bytes = s.bytes; b.dup = s.dup;
    b.verifyFails = (uint16_t)s.verifyFails; b.pad = 0;
    sendFrame(g_peer, T_ACK, s.stream, s.base, s.total, 0, (const uint8_t*)&b, sizeof b);
    g_lastAckMs = now;
  }
  if (s.active && s.doneReported && now - s.lastMs > kGraceMs) {
    portENTER_CRITICAL(&g_mux);
    g_rx.active = false;
    portEXIT_CRITICAL(&g_mux);
  }

  static uint32_t lastRxPrintMs = 0; static uint32_t lastRxPrintUnique = 0;
  if (s.active && !s.doneReported && now - lastRxPrintMs >= 1000 && s.unique != lastRxPrintUnique) {
    lastRxPrintMs = now; lastRxPrintUnique = s.unique;
    Serial.printf("RX s=%u %lu/%lu dup=%lu bytes=%lu\n", s.stream,
                  (unsigned long)s.unique, (unsigned long)(s.total ? s.total : 0),
                  (unsigned long)s.dup, (unsigned long)s.bytes);
  }

  // -- deliver TEXT chunks to the application
  if (g_msgH) {
    for (int n = 0; n < 16; n++) {
      MsgSlot slot;
      portENTER_CRITICAL(&g_mux);
      if (g_msg.head == g_msg.tail) { portEXIT_CRITICAL(&g_mux); break; }
      slot = g_msg.slot[g_msg.tail];
      g_msg.tail = (uint8_t)((g_msg.tail + 1) % 16);
      portEXIT_CRITICAL(&g_mux);
      g_msgH(slot.src, slot.seq, slot.total, slot.data, slot.len);
    }
  }

  // -- sender
  txEngine(now);
}

bool paired() { return g_paired; }
void peerMac(uint8_t mac[6]) { memcpy(mac, g_peer, 6); }
uint32_t linkUpCount() { return g_linkUps; }
uint16_t payloadMax() { return payloadCap(); }
uint32_t keyFingerprint() { return g_fp; }
const char* pskSource() { return g_pskSource; }
uint32_t txDoneCount() { return g_txDone; }
uint32_t macFailCount() { return g_macFailTotal; }
uint32_t rxTotalFrames() { return g_rxTotal; }

uint32_t sendErrCount() { return g_sendErr; }
uint32_t rxOverflowCount() { return g_rxOverflow; }
uint32_t msgDroppedCount() { return g_msg.dropped; }

bool streamActive() { return g_tx.active; }

bool startStream(uint8_t type, PayloadGen gen, void* ctx, uint32_t frames, const char* name) {
  if (!g_paired || g_tx.active || !gen || frames == 0) return false;
  uint32_t now = millis();
  g_tx = TxState{};
  g_tx.active = true; g_tx.type = type; g_tx.gen = gen; g_tx.ctx = ctx;
  g_tx.total = frames;
  if (++g_streamSeq == 0) ++g_streamSeq;
  g_tx.stream = g_streamSeq;
  strncpy(g_tx.name, name ? name : "?", sizeof g_tx.name - 1);
  g_tx.startMs = g_tx.lastProgressMs = g_tx.lastAckMs = g_tx.lastPrintMs = now;
  portENTER_CRITICAL(&g_mux);
  g_ackNew = false; g_macFailSession = 0;
  portEXIT_CRITICAL(&g_mux);
  Serial.printf("TX s=%u %s frames=%lu payload<=%u\n",
                g_tx.stream, g_tx.name, (unsigned long)frames, (unsigned)payloadCap());
  return true;
}

void stopStream(const char* reason) { finishTx(millis(), g_tx.acked >= g_tx.total, reason); }

namespace {
struct BlobCtx { const uint8_t* d; uint16_t len; uint16_t chunk; };
uint8_t g_blobBuf[1024];
BlobCtx g_blobCtx;

uint16_t blobGen(void* c, uint32_t seq, uint8_t* out, uint16_t maxLen) {
  BlobCtx* b = (BlobCtx*)c;
  uint32_t off = seq * b->chunk;
  if (off >= b->len) return 0;
  uint32_t n = b->len - off;
  if (n > b->chunk) n = b->chunk;
  if (n > maxLen) n = maxLen;
  memcpy(out, b->d + off, n);
  return (uint16_t)n;
}
}  // namespace

bool sendBlob(const uint8_t* data, uint16_t len) {
  if (!len || len > sizeof g_blobBuf) return false;
  memcpy(g_blobBuf, data, len);
  g_blobCtx.d = g_blobBuf; g_blobCtx.len = len;
  g_blobCtx.chunk = payloadCap();
  uint32_t frames = ((uint32_t)len + g_blobCtx.chunk - 1) / g_blobCtx.chunk;
  return startStream(T_TEXT, blobGen, &g_blobCtx, frames, "msg");
}

bool sendPing() {
  if (!g_paired || g_outPing.valid) return false;
  static uint32_t seq = 0;
  PingBody b; b.seq = ++seq; b.t0 = millis();
  if (!sendFrame(g_peer, T_PING, 0, 0, 0, 0, (const uint8_t*)&b, sizeof b)) return false;
  g_outPing.seq = b.seq; g_outPing.t0 = b.t0; g_outPing.valid = true;
  g_ping.sent++;
  return true;
}

void onMessage(MessageHandler h) { g_msgH = h; }
const TxReport& lastTx() { return g_txLast; }
const RxReport& lastRx() { return g_rxLast; }
const PingStats& pingStats() { return g_ping; }
void resetPingStats() { g_ping = PingStats{}; g_outPing = OutPing{}; }

uint16_t benchGen(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen) {
  uint16_t n = *(const uint16_t*)ctx;   // ctx points at the payload length
  if (n > maxLen) n = maxLen;
  for (uint16_t i = 0; i < n; i++) out[i] = benchByte(seq, i);
  return n;
}

}  // namespace e2e
