/* 15ESPHJ1602ALCD16pinTemp — Chesterfield, MO weather clock on an HJ1602A 16x2 LCD.

Joins WiFi (WIFI_SSID / WIFI_PASS from _secrets/secrets.h), syncs the clock
over NTP to America/Chicago, and fetches the current temperature + relative
humidity for Chesterfield, Missouri from Open-Meteo (no API key) every 10
minutes.  Displayed on the 16x2 LCD:

  row 0:  7:45 AM Wed        local time, NTP re-synced every 30 min
  row 1:  45°F 7°C RH 89%    weather, rounded °F + °C (° = HD44780 0xDF)

Until NTP has synced / the first weather fetch succeeds, the corresponding
row shows -- placeholders.  WiFi loss is detected every second; the board
reconnects and refetches weather.

Wiring (NodeMCU-32S pinout, identical to 14ESPHJ1602ALCD16pin):
  LCD RS  → GPIO4
  LCD E   → GPIO5
  LCD D4  → GPIO14
  LCD D5  → GPIO12
  LCD D6  → GPIO13
  LCD D7  → GPIO15
  LCD RW  → GND  (hard-wired for write-only)
  LCD VSS → GND
  LCD VDD → 3.3 V
  LCD V0  → 10 kΩ pot (VDD↔GND wiper to V0 for contrast)
  LCD A   → 3.3 V (backlight anode; some modules need a ~220 Ω series R)
  LCD K   → GND  (backlight cathode)

Requires LiquidCrystal + ArduinoJson (see platformio.ini).
LED GPIO2 flashes 90 ms once per second as a heartbeat.
*/

#include <Arduino.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <string.h>

#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

// HJ1602A 16×2 LCD (parallel, 4-bit mode) — same pins as project 14
LiquidCrystal lcd(4, 5, 14, 12, 13, 15);

#define LCD_COLS 16
#define LCD_ROWS 2

// ---- Chesterfield, Missouri: 38.6631 N, 90.5771 W, America/Chicago ----
static const char *API_HOST  = "api.open-meteo.com";
static const char *API_PATH  =
    "/v1/forecast?latitude=38.6631&longitude=-90.5771"
    "&current=temperature_2m,relative_humidity_2m"
    "&temperature_unit=fahrenheit&timezone=America%2FChicago";
static const char *TZ_POSIX  = "CST6CDT,M3.2.0,M11.1.0";  // America/Chicago

static const unsigned long WEATHER_OK_MS   = 10UL * 60UL * 1000UL;  // refresh
static const unsigned long WEATHER_FAIL_MS = 30UL * 1000UL;         // retry
static const unsigned long WIFI_RETRY_MS   = 5UL * 1000UL;          // reconnect
static const unsigned long WIFI_TIMEOUT_MS = 20UL * 1000UL;

bool  haveWeather  = false;
float tempF        = 0.0f;
int   humidity     = -1;
unsigned long weatherDueMs = 0;

// current + last-drawn screen contents (padded to 16 chars)
char row0[LCD_COLS + 1], row1[LCD_COLS + 1];
char shown0[LCD_COLS + 1] = "", shown1[LCD_COLS + 1] = "";

unsigned long lastTick = 0;   // 1 s display tick
unsigned long ledOffMs = 0;   // heartbeat LED off-time

void connectWiFi();
bool fetchWeather();

/* Pad s in place with spaces to LCD_COLS chars. */
void padRow(char *s) {
  size_t n = strlen(s);
  for (; n < LCD_COLS; n++) s[n] = ' ';
  s[LCD_COLS] = '\0';
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.clear();
  lcd.print("15 ESP32 Temp");
  lcd.setCursor(0, 1);
  lcd.print("Chesterfield MO");

  connectWiFi();

  // NTP; POSIX TZ string applies CST/CDT after sync.  Guest VLANs often
  // block UDP/123 — if unsynced, fetchWeather() falls back to the
  // current.time + utc_offset_seconds fields from the weather API.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  sntp_set_sync_interval(30UL * 60UL * 1000UL);  // re-check time every 30 min
  setenv("TZ", TZ_POSIX, 1);
  tzset();

  if (WiFi.status() == WL_CONNECTED) fetchWeather();
  weatherDueMs = millis() + (haveWeather ? WEATHER_OK_MS : WEATHER_FAIL_MS);

  // report time source (10 s grace for the NTP client)
  bool synced = false;
  for (int i = 0; i < 20 && !synced; i++) {
    delay(500);
    synced = time(nullptr) > 100000;
  }
  Serial.printf("time: %s\n",
                synced ? "NTP synced" : "NTP NOT synced - relying on API fallback");

  lcd.clear();
  Serial.println("ready");
}

void loop() {
  unsigned long now = millis();

  // heartbeat: LED on 90 ms per tick
  if (ledOffMs && (long)(now - ledOffMs) >= 0) {
    digitalWrite(LED_BUILTIN, LOW);
    ledOffMs = 0;
  }
  if (now - lastTick < 1000) return;
  lastTick = now;
  digitalWrite(LED_BUILTIN, HIGH);
  ledOffMs = now + 90;

  // --- WiFi watchdog ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi: lost - reconnecting");
    WiFi.disconnect();
    connectWiFi();
  }

  // --- weather timer ---
  if ((long)(now - weatherDueMs) >= 0) {
    if (WiFi.status() == WL_CONNECTED) {
      fetchWeather();
      weatherDueMs = now + (haveWeather ? WEATHER_OK_MS : WEATHER_FAIL_MS);
    } else {
      weatherDueMs = now + WIFI_RETRY_MS;
    }
  }

  // --- row 0: local time, no seconds (blank until time is set) ---
  time_t t = time(nullptr);
  if (t > 100000) {
    struct tm ti;
    localtime_r(&t, &ti);
    strftime(row0, sizeof row0, "%I:%M %p %a", &ti);  // 07:45 AM Wed
    if (row0[0] == '0') memmove(row0, row0 + 1, strlen(row0));  // 7:45 AM Wed
  } else {
    strcpy(row0, "--:-- -- ---");
  }

  // --- row 1: temperature in °F and °C (rounded) + relative humidity ---
  if (haveWeather)
    snprintf(row1, sizeof row1, "%.0f\xDF" "F %.0f\xDF" "C RH %d%%", tempF,
             (tempF - 32.0f) * 5.0f / 9.0f, humidity);
  else
    strcpy(row1, "--\xDF" "F --\xDF" "C RH ---%");

  padRow(row0);
  padRow(row1);

  static bool row0dumped = false;
  if (!row0dumped && t > 100000) {
    row0dumped = true;
    Serial.printf("display: row0=[%s] row1=[%s]\n", row0, row1);
  }

  if (strcmp(row0, shown0)) {
    strcpy(shown0, row0);
    lcd.setCursor(0, 0);
    lcd.print(row0);
  }
  if (strcmp(row1, shown1)) {
    strcpy(shown1, row1);
    lcd.setCursor(0, 1);
    lcd.print(row1);
  }
}

/* Blocking WiFi join with timeout (01Wifi style).  Retries from the loop
   watchdog every WIFI_RETRY_MS while disconnected.  Scans first and pins
   the join to the strongest BSS of our SSID — otherwise the STA can latch
   onto a distant repeater/neighbor with the same SSID (−80 dBm) and stall. */
void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // keeps TLS handshakes responsive

  int8_t bestChan = 0;
  int32_t bestRssi = -127;
  uint8_t bestBssid[6] = {0};
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      memcpy(bestBssid, WiFi.BSSID(i), 6);
      bestChan = WiFi.channel(i);
    }
  }
  WiFi.scanDelete();

  Serial.printf("wifi: connecting to \"%s\"", WIFI_SSID);
  if (bestChan)
    Serial.printf(" (best BSS %02X:%02X:%02X:%02X:%02X:%02X ch%d %d dBm)",
                  bestBssid[0], bestBssid[1], bestBssid[2],
                  bestBssid[3], bestBssid[4], bestBssid[5], bestChan, bestRssi);
  Serial.print('.');
  if (bestChan)
    WiFi.begin(WIFI_SSID, WIFI_PASS, bestChan, bestBssid);
  else
    WiFi.begin(WIFI_SSID, WIFI_PASS);  // SSID not seen in scan — blind join

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(200);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nwifi: connected ip=%s rssi=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else {
    Serial.println("\nwifi: TIMEOUT");
    // Diagnostic: what can the board actually see?  (2.4 GHz only)
    int n = WiFi.scanNetworks();
    Serial.printf("wifi: scan found %d network(s):", n);
    for (int i = 0; i < n; i++)
      Serial.printf(" %s(%d)", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    Serial.println();
    WiFi.scanDelete();
  }
}

/* Days-since-epoch from a broken-down struct tm interpreted as UTC.
   (newlib does not declare timegm() in the Arduino build.) */
time_t mkEpochUtc(const struct tm &t) {
  static const int daysBefore[] = {0, 31, 59, 90, 120, 151, 181,
                                   212, 243, 273, 304, 334, 365};
  int y = t.tm_year + 1900;
  long days = 365L * (y - 1970) + (y - 1969) / 4 - (y - 1901) / 100 +
              (y - 1601) / 400 + daysBefore[t.tm_mon] + (t.tm_mday - 1);
  if (t.tm_mon >= 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
  return days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec;
}

/* GET the Open-Meteo current-weather JSON and update tempF / humidity.
   Response is ~350 B; no filter needed.  HTTPS with setInsecure() — same
   approach as 02TelegramTX. */
bool fetchWeather() {
  digitalWrite(LED_BUILTIN, HIGH);
  WiFiClientSecure client;
  client.setInsecure();  // no cert pinning
  HTTPClient http;
  http.setTimeout(10000);
  bool ok = false;

  if (http.begin(client, String("https://") + API_HOST + API_PATH)) {
    int code = http.GET();
    if (code == 200) {
      // getString() decodes chunked transfer encoding — getStream() would
      // feed raw chunk-length headers into the JSON parser (InvalidInput)
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);
      JsonVariant cur = doc["current"];
      if (!err && cur["temperature_2m"].is<double>() &&
          cur["relative_humidity_2m"].is<double>()) {
        tempF    = cur["temperature_2m"].as<float>();
        humidity = cur["relative_humidity_2m"].as<int>();
        haveWeather = true;
        ok = true;

        // Time fallback for when NTP is blocked (common on guest VLANs):
        // the API returns local time (15-min grid) + UTC offset, enough
        // to set a usable clock; the TZ string still applies CST/CDT.
        if (time(nullptr) < 100000) {
          const char *iso = cur["time"];  // e.g. "2026-10-23T07:45"
          long off = doc["utc_offset_seconds"] | -18000L;
          struct tm ti = {};
          if (iso && strptime(iso, "%Y-%m-%dT%H:%M", &ti)) {
            timeval tv{};
            tv.tv_sec = mkEpochUtc(ti) - off;
            settimeofday(&tv, nullptr);
            Serial.println("time: set from weather API (NTP unsynced)");
          }
        }
      } else if (err) {
        Serial.printf("weather: json error: %s\n", err.c_str());
      } else {
        Serial.println("weather: no current.* fields in response");
      }
    } else {
      Serial.printf("weather: HTTP %d\n", code);
    }
    http.end();
  } else {
    Serial.println("weather: HTTPClient begin failed");
  }

  Serial.printf("weather: %s temp=%.1fF rh=%d%% heap=%lu\n",
                ok ? "ok" : "FAIL", tempF, humidity, ESP.getFreeHeap());
  digitalWrite(LED_BUILTIN, LOW);
  return ok;
}
