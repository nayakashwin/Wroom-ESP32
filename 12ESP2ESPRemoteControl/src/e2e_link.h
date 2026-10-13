// e2e_link.h — encrypted point-to-point ESP-NOW link for two ESP32s.
//
// VENDORED from ../09ESP2ESPCommunication/src/e2e_link.h with these diffs
// (everything else is byte-identical, keep it that way when syncing):
//   1. kMagic on the wire changed "9E2E" -> "1E2E" so 09 and 12 boards that
//      share a channel + PSK ignore each other instead of pairing.
//   2. NVS namespace "e2e" -> "e12" (runtime psk / phy rate), so 12 does not
//      inherit a rate stored by 09 experiments on the same board.
//   3. Default PHY rate 54M -> 1M_L: receiver sensitivity ~-98 dBm instead of
//      ~-74 dBm (~24 dB more link budget) - range over throughput.
//   4. Link supervision timeout 3 s -> 8 s to ride long-range fades without
//      pairing churn (sessions still auto-retry if one dies).
//   5. New setQuiet(true): suppress per-session chatter (start/progress/DONE
//      prints) for the continuous telemetry stream; anomalies (abort, retry,
//      link down, key mismatch) always print. App reads lastTx()/txDoneCount().
//   6. Paired HELLO cadence 1000 ms -> 250 ms: a peer that reboots is
//      re-paired within ~250 ms (the stream gap shrinks accordingly).
//
// One firmware image runs on both boards. Each board broadcasts a small
// plaintext HELLO (MAC + key fingerprint) until it hears the other; when the
// fingerprints match, both sides register each other as an *encrypted* ESP-NOW
// peer (AES-CCM-128 with a key derived from a shared passphrase) and all
// further frames are ciphertext on air.
//
// On top of that link sits a small reliable-transfer engine:
//   * frames carry (stream id, seq, total)
//   * the receiver slides a 64-frame receive window and answers with bitmap
//     ACKs every <=8 ms while a stream is active
//   * the sender keeps a 64-frame send window, retransmits gaps and stalls
// Data payloads are generated on demand (PayloadGen), so no payload ring
// buffer is needed and arbitrary user payloads are supported.
#ifndef E2E_LINK_H
#define E2E_LINK_H

#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------- build-time configuration
// WiFi channel both boards must share (1..13). Pick a quiet one.
#ifndef E2E_CHANNEL
#define E2E_CHANNEL 6
#endif

// Max bytes handed to esp_now_send(): 18 B header + payload. 250 is the
// ESP-NOW v1 maximum and is delivered reliably with encryption on this
// stack (measured); lower it only if mixing with other ESP-NOW users.
#ifndef E2E_FRAME_MAX
#define E2E_FRAME_MAX 250
#endif

// Pre-shared key passphrase. Priority: NVS value written by the serial
// `key <psk>` command  >  this build flag  >  this default.
// CHANGE IT (build flag or `key` command) before using this for real.
#ifndef E2E_PSK
#define E2E_PSK "esp32-ptp-default-key-CHANGE-ME"
#endif

namespace e2e {

// Frame types on the wire (see e2e_link.cpp). DATA and TEXT are session
// frames and differ only in how the application treats the payload.
static const uint8_t  T_HELLO = 1;   // broadcast, plaintext, discovery only
static const uint8_t  T_DATA  = 2;   // session chunk, pattern-verified bench
static const uint8_t  T_TEXT  = 3;   // session chunk, application payload
static const uint8_t  T_ACK   = 4;   // receiver -> sender bitmap ack
static const uint8_t  T_PING  = 5;
static const uint8_t  T_PONG  = 6;

static const size_t   HDR_SIZE = 18; // bytes of frame header

// ------------------------------------------------------------------- types

// Build the payload for one outgoing frame. Return its length (<= maxLen).
// Called from the sender's loop task, once per frame *transmission attempt*
// (retransmits regenerate the same bytes), so it must be deterministic and
// fast. `seq` is the frame index within the session.
typedef uint16_t (*PayloadGen)(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen);

// Called from loop() context (not an ISR) for every received TEXT frame.
// `seq`/`total` identify the chunk position; reassembly for multi-frame
// messages is up to the application (see main.cpp for an example).
typedef void (*MessageHandler)(const uint8_t src[6], uint32_t seq, uint32_t total,
                               const uint8_t* data, uint16_t len);

struct TxReport {          // result of the last finished sender session
  uint16_t stream;
  uint32_t frames, acked;  // planned / acknowledged by the peer
  uint32_t retx;           // retransmitted frames
  uint32_t macFail;        // attempts the WiFi MAC layer gave no ack for
  uint32_t bytes;          // payload bytes delivered
  uint32_t ms;             // start -> fully acked
  bool     ok;             // all frames acknowledged
};

struct RxReport {          // result of the last finished receiver session
  uint16_t stream;
  uint32_t frames, unique, dup;
  uint32_t bytes;
  uint32_t verifyFails;    // bench pattern mismatches (0 = clean)
  uint32_t ms;
  bool     complete;       // every frame 0..frames-1 received
};

struct PingStats {
  uint32_t sent, ok, timeouts, lastRttMs, minRttMs, maxRttMs, sumRttMs;
};

// ------------------------------------------------------------------- api

// Bring the link up (WiFi STA on E2E_CHANNEL, ESP-NOW, keys, peers).
// Returns false only if the WiFi/ESP-NOW stack fails to start.
bool begin();

// Quiet mode: suppress routine per-session prints (TX start/progress/DONE,
// RX start/DONE) for high-rate streams. Aborts, retries, LINK up/down and key
// warnings always print. Call after begin(), before streaming.
void setQuiet(bool q);

// Service the link; call from loop() as often as possible.
void poll();

bool     paired();               // peer registered + key fingerprint matched
void     peerMac(uint8_t mac[6]);
uint16_t payloadMax();           // negotiated max payload bytes per frame

// Reliable transfer: sends `frames` frames built by `gen` until the peer's
// bitmap acks every one of them (type = T_DATA or T_TEXT). One session at a
// time per board; returns false when busy / not paired.
bool startStream(uint8_t type, PayloadGen gen, void* ctx, uint32_t frames,
                 const char* name);
void stopStream(const char* reason);
bool streamActive();

// Convenience: send an arbitrary buffer as one reliable TEXT session
// (auto-chunked across frames when longer than payloadMax()).
bool sendBlob(const uint8_t* data, uint16_t len);

// Fire one ping (8-byte frame); result via pingStats().
bool sendPing();

// Runtime PHY rate (wifi_phy_rate_t value, 0x00..0x2A). Applies immediately,
// optionally persisted to NVS and used instead of E2E_PHY_RATE after reboot.
// Lower rates survive interference/distance better; throughput is nearly
// rate-independent on this driver (see README section 7).
bool     setPhyRate(uint8_t rateEnum, bool persist);
uint8_t  phyRate();

// Built-in deterministic bench payload generator (verified on receive).
// ctx must point at a uint16_t holding the payload length to send.
uint16_t benchGen(void* ctx, uint32_t seq, uint8_t* out, uint16_t maxLen);

void         onMessage(MessageHandler h);   // install TEXT receiver hook
const TxReport& lastTx();
const RxReport& lastRx();
const PingStats& pingStats();
void         resetPingStats();

// Debug helpers used by the `status` command.
uint32_t keyFingerprint();   // FNV-1a of the derived LMK (identity check)
const char* pskSource();     // "nvs" or the compile-time default
uint32_t millisWraps();      // 49.7-day millis() rollovers seen (logged too)
bool     retryPending();     // a failed stream is waiting for the link to return
uint32_t linkUpCount();
uint32_t txDoneCount();      // finished sender sessions (ok or aborted)
uint32_t rxTotalFrames();    // all valid session frames ever received
uint32_t macFailCount();     // all MAC-level send failures
uint32_t sendErrCount();     // esp_now_send() refusals (queue full etc.)
uint32_t rxOverflowCount();  // frames outside the rx window (should stay 0)
uint32_t msgDroppedCount();  // TEXT chunks the app did not drain in time

}  // namespace e2e

#endif  // E2E_LINK_H
