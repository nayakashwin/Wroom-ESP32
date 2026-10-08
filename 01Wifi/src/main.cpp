#include <Arduino.h>
#include <WiFi.h>
#include <lwip/ip_addr.h>
#include "ping/ping_sock.h"
#include <Ticker.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

/* ---------------- secrets (in _secrets/secrets.h, never committed) -------- */
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

/* ---------------- config ---------------- */
static const char *PING_HOST = "google.com";
static const uint32_t PING_COUNT = 3;        // pings in one session
static const uint32_t PING_TOTAL_LIMIT = 3;  // lifetime cap - stop after 3 pings
static const uint32_t PING_INTERVAL_MS = 1000;
static const uint32_t PING_TIMEOUT_MS = 1000;
static const uint32_t PING_DATA_SIZE = 56;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_DELAY_MS = 3000;

/* ---------------- state ---------------- */
enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };

static WifiState wifiState = WIFI_IDLE;
static uint32_t connStartMs = 0;
static uint32_t retryAtMs = 0;
static bool autoPingDone = false;
static bool linkLostPrinted = false;
static uint32_t pingsSent = 0;  // total ping attempts this boot

struct PingBatch {
  volatile bool done;
  uint32_t index;  // callback counter (single ping task)
  uint32_t received;
  uint32_t lost;
  uint32_t rtt[PING_COUNT];
  bool ok[PING_COUNT];
};
static PingBatch batch;

/* ---------------- ping callbacks ---------------- */
static void onPingSuccess(esp_ping_handle_t hdl, void *arg) {
  PingBatch *b = (PingBatch *)arg;
  uint32_t idx = b->index++;
  uint32_t rtt = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &rtt, sizeof(rtt));
  if (idx < PING_COUNT) {
    b->ok[idx] = true;
    b->rtt[idx] = rtt;
  }
  b->received++;
}

static void onPingTimeout(esp_ping_handle_t hdl, void *arg) {
  PingBatch *b = (PingBatch *)arg;
  b->index++;
  b->lost++;
}

static void onPingEnd(esp_ping_handle_t hdl, void *arg) {
  ((PingBatch *)arg)->done = true;
}

static bool pingBudgetLeft() { return pingsSent < PING_TOTAL_LIMIT; }

/* ---------------- one ping session (never over budget) ---------------- */
static void doPingBatch() {
  uint32_t n = PING_TOTAL_LIMIT - pingsSent;
  if (n > PING_COUNT) n = PING_COUNT;
  if (n == 0) return;

  IPAddress ip;
  if (WiFi.hostByName(PING_HOST, ip) != 1) {
    Serial.printf("%s: DNS lookup failed\n", PING_HOST);
    return;
  }

  Serial.printf("PING %s (%s) %u bytes of data\n", PING_HOST, ip.toString().c_str(),
                (unsigned)(PING_DATA_SIZE + 8));

  memset(&batch, 0, sizeof(batch));
  batch.done = false;

  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count = n;
  cfg.interval_ms = PING_INTERVAL_MS;
  cfg.timeout_ms = PING_TIMEOUT_MS;
  cfg.data_size = PING_DATA_SIZE;
  cfg.tos = 0;
  cfg.ttl = 64;
  IP_ADDR4(&cfg.target_addr, ip[0], ip[1], ip[2], ip[3]);
  cfg.task_stack_size = 3072;
  cfg.task_prio = 5;
  cfg.interface = 0;

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.cb_args = &batch;
  cbs.on_ping_success = onPingSuccess;
  cbs.on_ping_timeout = onPingTimeout;
  cbs.on_ping_end = onPingEnd;

  esp_ping_handle_t hdl = NULL;
  esp_err_t err = esp_ping_new_session(&cfg, &cbs, &hdl);
  if (err != ESP_OK) {
    Serial.printf("ping session failed: %d\n", (int)err);
    return;
  }

  uint32_t start = millis();
  esp_ping_start(hdl);
  const uint32_t guard = n * (PING_INTERVAL_MS + PING_TIMEOUT_MS) + 5000;
  while (!batch.done && (millis() - start) < guard) {
    delay(10);
  }
  if (!batch.done) {
    Serial.println("ping session timed out");
    esp_ping_stop(hdl);
  }
  esp_ping_delete_session(hdl);

  for (uint32_t i = 0; i < batch.index && i < n; i++) {
    if (batch.ok[i]) {
      Serial.printf("64 bytes from %s: icmp_seq=%u time=%u ms\n", ip.toString().c_str(),
                    (unsigned)i, (unsigned)batch.rtt[i]);
    } else {
      Serial.printf("no reply from %s: icmp_seq=%u\n", ip.toString().c_str(), (unsigned)i);
    }
  }

  uint32_t minRtt = 0, maxRtt = 0, sumRtt = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!batch.ok[i]) continue;
    uint32_t r = batch.rtt[i];
    if (minRtt == 0 || r < minRtt) minRtt = r;
    if (r > maxRtt) maxRtt = r;
    sumRtt += r;
  }
  uint32_t avgRtt = batch.received ? (sumRtt + batch.received / 2) / batch.received : 0;
  uint32_t loss = batch.received + batch.lost
                      ? (batch.lost * 100) / (batch.received + batch.lost)
                      : 0;

  Serial.printf("--- %s ping statistics ---\n", PING_HOST);
  Serial.printf("%u packets transmitted, %u received, %u%% packet loss\n",
                (unsigned)(batch.received + batch.lost), (unsigned)batch.received,
                (unsigned)loss);
  if (batch.received) {
    Serial.printf("round-trip min/avg/max = %u/%u/%u ms\n", (unsigned)minRtt,
                  (unsigned)avgRtt, (unsigned)maxRtt);
  }

  pingsSent += batch.received + batch.lost;
  if (!pingBudgetLeft()) {
    Serial.printf("%u pings done - stopping, no more pings\n", (unsigned)PING_TOTAL_LIMIT);
  }
}

/* ---------------- wifi ---------------- */
static void connectNow() {
  Serial.printf("connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiState = WIFI_CONNECTING;
  connStartMs = millis();
}

static void onConnected() {
  wifiState = WIFI_CONNECTED;
  linkLostPrinted = false;
  Serial.printf("connected to \"%s\" ip=%s rssi=%d dBm\n", WIFI_SSID,
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
}

/* ---------------- led heartbeat: lub-dub ... pause ---------------- */
static void updateLed(uint32_t now) {
  static const uint16_t PHASE_MS[4] = {90, 160, 90, 1100};  // on, off, on, off
  static uint8_t phase = 0;
  static uint32_t phaseStart = 0;
  if (phaseStart == 0) phaseStart = now;
  if (now - phaseStart >= PHASE_MS[phase]) {
    phase = (phase + 1) & 3;
    phaseStart = now;
  }
  digitalWrite(LED_BUILTIN, (phase == 0 || phase == 2) ? HIGH : LOW);
}

// The loop blocks for seconds inside the ping session, so the heartbeat is
// driven by a Ticker instead - it keeps beating at 10 ms resolution
// regardless of what the main loop is doing.
static Ticker ledTicker;
static void ledTick() { updateLed(millis()); }

/* ---------------- main ---------------- */
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  ledTicker.attach_ms(10, ledTick);
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=== 01Wifi ==="));
  Serial.printf("ssid: %s (hardcoded)\n", WIFI_SSID);
  Serial.printf("auto-pings %s %u times after connect, then stops\n", PING_HOST,
                (unsigned)PING_TOTAL_LIMIT);
  connectNow();
}

void loop() {
  if (wifiState == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onConnected();
    } else if ((int32_t)(millis() - connStartMs) > (int32_t)CONNECT_TIMEOUT_MS) {
      Serial.println(F("connect timeout - retrying"));
      wifiState = WIFI_IDLE;
      retryAtMs = millis() + RETRY_DELAY_MS;
    }
  } else if (wifiState == WIFI_IDLE) {
    if (retryAtMs && (int32_t)(millis() - retryAtMs) >= 0) {
      retryAtMs = 0;
      connectNow();
    }
  } else {  // WIFI_CONNECTED
    if (WiFi.status() != WL_CONNECTED) {
      if (!linkLostPrinted) {
        Serial.println(F("WiFi link lost - reconnecting ..."));
        linkLostPrinted = true;
        wifiState = WIFI_CONNECTING;
        connStartMs = millis();
      }
    } else if (!autoPingDone && pingBudgetLeft()) {
      autoPingDone = true;
      doPingBatch();
    }
  }

  delay(5);
}
