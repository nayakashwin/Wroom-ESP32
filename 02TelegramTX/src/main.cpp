#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Ticker.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

/* ---------------- secrets (in _secrets/secrets.h, never committed) --------
 * WIFI_SSID / WIFI_PASS, BOT_TOKEN and CHAT_ID (numeric id, @channelusername
 * or -100... for a group) all come from there.
 */
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

/* ---------------- config ---------------- */
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_DELAY_MS = 3000;
static const size_t TX_LINE_MAX = 160;
static const uint32_t HEAP_WARN_BYTES = 40000;  // TLS handshake needs ~40 KB contiguous

/* ---------------- state ---------------- */
enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };

static WifiState wifiState = WIFI_IDLE;
static uint32_t connStartMs = 0;
static uint32_t retryAtMs = 0;
static bool bootTasksDone = false;
static bool linkLostPrinted = false;

/* ---------------- telegram ---------------- */
static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

static bool telegramSend(const String &text) {
  if (wifiState != WIFI_CONNECTED) {
    Serial.println(F("telegram: wifi not connected - message skipped"));
    return false;
  }

  String url = String(F("https://api.telegram.org/bot")) + BOT_TOKEN + F("/sendMessage");
  String payload = String(F("{\"chat_id\":\"")) + jsonEscape(CHAT_ID) +
                   F("\",\"text\":\"") + jsonEscape(text) + F("\"}");

  WiFiClientSecure client;
  client.setInsecure();  // TLS without certificate pinning
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    Serial.println(F("telegram: HTTPClient begin failed"));
    return false;
  }
  http.addHeader(F("Content-Type"), F("application/json"));

  int code = http.POST(payload);
  String body = http.getString();
  http.end();

  bool ok = (code == HTTP_CODE_OK) && body.startsWith(F("{\"ok\":true"));
  Serial.printf("telegram send -> HTTP %d %s (heap free=%u min=%u maxblk=%u)\n", code,
                ok ? "ok" : "FAIL", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (ESP.getMaxAllocHeap() < HEAP_WARN_BYTES) {
    Serial.println(F("WARN: largest free block < 40 KB - TLS may fail"));
  }
  Serial.printf("telegram body: %.300s\n", body.c_str());
  return ok;
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

/* ---------------- serial: every typed line is sent to the channel --------- */
static void handleSerial() {
  static char buf[TX_LINE_MAX];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      if (len > 0) {
        Serial.printf("sending: %s\n", buf);
        telegramSend(String(buf));
      }
      len = 0;
      continue;
    }
    if (c == 8 || c == 127) {
      if (len > 0) len--;
      continue;
    }
    if (len < TX_LINE_MAX - 1) buf[len++] = c;
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

// The loop blocks for seconds inside telegramSend(), so the heartbeat is
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
  Serial.println(F("=== 02TelegramTX ==="));
  Serial.printf("ssid: %s  chat: %s (hardcoded)\n", WIFI_SSID, CHAT_ID);
  Serial.println(F("type a line + Enter - it is sent to the telegram channel"));
  connectNow();
}

void loop() {
  handleSerial();

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
    } else if (!bootTasksDone) {
      bootTasksDone = true;
      String hello = String(F("02TelegramTX online: ssid=")) + WIFI_SSID +
                     F(" ip=") + WiFi.localIP().toString() +
                     F(" rssi=") + WiFi.RSSI() + F(" dBm");
      telegramSend(hello);
    }
  }

  delay(5);
}
