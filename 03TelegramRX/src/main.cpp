#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Ticker.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

/* ---------------- secrets (in _secrets/secrets.h, never committed) --------
 * WIFI_SSID / WIFI_PASS and BOT_TOKEN come from there.
 */
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

/* ---------------- config ---------------- */
static const uint32_t POLL_TIMEOUT_S = 5;  // getUpdates long-poll hold time
static const uint32_t POLL_LIMIT = 10;     // max updates per response (bounds body size)
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_DELAY_MS = 3000;
static const uint32_t HEAP_WARN_BYTES = 40000;  // TLS handshake needs ~40 KB contiguous

/* ---------------- state ---------------- */
enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };

static WifiState wifiState = WIFI_IDLE;
static uint32_t connStartMs = 0;
static uint32_t retryAtMs = 0;
static bool linkLostPrinted = false;
static bool webhookCleared = false;

static uint32_t updateOffset = 0;  // next getUpdates offset (server confirms < offset)
static uint32_t updatesSeen = 0;

static void heapWarnIfLow() {
  if (ESP.getMaxAllocHeap() < HEAP_WARN_BYTES) {
    Serial.println(F("WARN: largest free block < 40 KB - TLS may fail"));
  }
}

/* ---------------- https helper ---------------- */
static int telegramApi(const String &args, String &body) {
  String url = String(F("https://api.telegram.org/bot")) + BOT_TOKEN + args;
  WiFiClientSecure client;
  client.setInsecure();  // TLS without certificate pinning
  HTTPClient http;
  http.setTimeout(20000);  // must exceed the long-poll hold time
  if (!http.begin(client, url)) {
    body = "HTTPClient begin failed";
    return -1;
  }
  int code = http.GET();
  body = http.getString();
  http.end();
  return code;
}

/* ---------------- one incoming update ---------------- */
static void handleUpdate(JsonObject upd) {
  uint32_t uid = upd["update_id"] | 0U;
  if (uid) updateOffset = uid + 1;
  updatesSeen++;

  static const char *MSG_KEYS[] = {"message", "edited_message", "channel_post",
                                   "edited_channel_post"};
  for (const char *k : MSG_KEYS) {
    if (!upd[k].is<JsonObject>()) continue;
    JsonObject m = upd[k];

    const char *text = m["text"];
    if (!text) text = m["caption"];

    long chatId = m["chat"]["id"] | 0L;
    const char *chatTitle = m["chat"]["title"];
    long fromId = m["from"]["id"] | 0L;
    const char *fromName = m["from"]["username"];
    if (!fromName) fromName = m["from"]["first_name"];

    Serial.printf("TG RX: update %u [%s]", (unsigned)uid, k);
    if (fromId) {
      Serial.printf(" from %s (id %ld)", fromName ? fromName : "?", fromId);
    }
    if (chatId) Serial.printf(" chat %ld", chatId);
    if (chatTitle) Serial.printf(" \"%s\"", chatTitle);
    if (text && text[0]) {
      Serial.printf(" : %s\n", text);
    } else {
      Serial.println(F(" : <non-text message>"));
    }
    return;
  }

  const char *kind = nullptr;
  for (JsonPair kv : upd) {
    const char *key = kv.key().c_str();
    if (key && strcmp(key, "update_id") != 0) {
      kind = key;
      break;
    }
  }
  Serial.printf("TG RX: update %u type=%s (not displayed)\n", (unsigned)uid,
                kind ? kind : "?");
}

/* ---------------- one long-poll of getUpdates ---------------- */
/* Keep only the fields handleUpdate prints - the filtered doc stays small
 * even for long messages with entities/metadata. */
static void addMsgFilter(JsonObject dst) {
  dst["text"] = true;
  dst["caption"] = true;
  JsonObject from = dst["from"].to<JsonObject>();
  from["id"] = true;
  from["username"] = true;
  from["first_name"] = true;
  JsonObject chat = dst["chat"].to<JsonObject>();
  chat["id"] = true;
  chat["title"] = true;
}

static const JsonDocument &updateFilter() {
  static JsonDocument filter;
  static bool ready = false;
  if (!ready) {
    ready = true;
    filter["ok"] = true;
    JsonObject elem = filter["result"].to<JsonArray>().add<JsonObject>();
    elem["update_id"] = true;
    addMsgFilter(elem["message"].to<JsonObject>());
    addMsgFilter(elem["edited_message"].to<JsonObject>());
    addMsgFilter(elem["channel_post"].to<JsonObject>());
    addMsgFilter(elem["edited_channel_post"].to<JsonObject>());
  }
  return filter;
}

static void pollUpdates() {
  String args = String(F("/getUpdates?timeout=")) + POLL_TIMEOUT_S +
                F("&limit=") + POLL_LIMIT + F("&offset=") + updateOffset;
  String body;  // bounded by POLL_LIMIT, freed on return
  int code = telegramApi(args, body);

  if (code != HTTP_CODE_OK) {
    Serial.printf("poll failed: HTTP %d %.200s\n", code, body.c_str());
    heapWarnIfLow();
    return;
  }

  // TLS buffers are already released (telegramApi returned); parse while only
  // body + doc are resident, then both die at the end of this function.
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(updateFilter()));
  if (err) {
    Serial.printf("poll json error: %s\n", err.c_str());
    return;
  }
  if (!(doc["ok"] | false)) {
    const char *desc = doc["description"] | "?";
    Serial.printf("poll api error: %s\n", desc);
    return;
  }

  uint32_t n = 0;
  for (JsonObject upd : doc["result"].as<JsonArray>()) {
    handleUpdate(upd);
    n++;
  }
  Serial.printf("poll: %u new (total %u seen, offset %u) heap free=%u min=%u maxblk=%u\n",
                (unsigned)n, (unsigned)updatesSeen, (unsigned)updateOffset,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  heapWarnIfLow();
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

  if (!webhookCleared) {
    webhookCleared = true;
    String body;
    int code = telegramApi(F("/deleteWebhook"), body);
    if (code == HTTP_CODE_OK && (body.indexOf(F("\"ok\":true")) >= 0)) {
      Serial.println(F("webhook cleared - getUpdates polling enabled"));
    } else {
      Serial.printf("deleteWebhook -> HTTP %d %.200s\n", code, body.c_str());
    }
  }
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

// The loop blocks for seconds inside each long-poll, so the heartbeat is
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
  Serial.println(F("=== 03TelegramRX ==="));
  Serial.printf("ssid: %s (hardcoded)\n", WIFI_SSID);
  Serial.println(F("incoming telegram messages are printed as:"));
  Serial.println(F("  TG RX: update <id> [message] from <user> (chat ...) : <text>"));
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
    } else {
      pollUpdates();  // blocks up to POLL_TIMEOUT_S + handshake
    }
  }

  delay(5);
}
