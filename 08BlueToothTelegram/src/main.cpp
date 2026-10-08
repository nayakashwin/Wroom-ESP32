#include <Arduino.h>

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Ticker.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <BLEAddress.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUUID.h>

#include <esp_task_wdt.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "bthome_parser.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ------------------------------------------------------------- telegram/wifi
// WIFI_SSID / WIFI_PASS, BOT_TOKEN and CHAT_ID come from _secrets/secrets.h
// (never committed; CHAT_ID may be a numeric id, @channelusername or -100...).
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_DELAY_MS = 3000;
static const size_t TX_LINE_MAX = 160;
// Calibrated for *this* build, measured with the BLE scan running: idle
// largest block is ~36 KB and a TLS session leaves ~13 KB, yet sends succeed
// (04/06 needed 40 KB because there was no BLE). Warn when the block the
// handshake would carve out of is genuinely smaller than that.
static const uint32_t HEAP_WARN_BYTES = 12000;

// ------------------------------------------------------------------ sensors
struct SensorDef {
  const char *key;
  const char *label;
  const char *mac;
};

// The three LYWSD03MMC sensors, same as atc.sh / bthome-mon.sh on the Debian
// side and as 07BlueToothTempSensor.
static const SensorDef kSensors[] = {
    {"ria", "Rias Room", "A4:C1:38:3B:E2:F6"},
    {"kitchen", "Kitchen", "A4:C1:38:D9:0C:0C"},
    {"basement", "Basement", "A4:C1:38:10:49:7C"},
};
static const size_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

// Seconds a cached advertisement may be before it is shown as stale. Kitchen is
// a marginal link (~-92 dBm) and advertises far less often, hence 1800.
static const uint32_t kMaxAgeSeconds[] = {600, 1800, 600};

// Serial summary every 5 s, independent of Telegram.
//
// Telegram is input-driven: getUpdates is long-polled every loop pass (the
// 04TelegramRXTX pattern), and the sensor grid is sent *only* in answer to a
// /temp command from the owner - never on a timer. COMMAND_GAP_MS keeps a
// /temp burst from turning into a send storm.
static const uint32_t PRINT_INTERVAL_MS = 5000;
static const uint32_t POLL_TIMEOUT_S = 2;     // long-poll hold, seconds
static const uint32_t POLL_LIMIT = 10;        // bounds the update batch
static const uint32_t COMMAND_GAP_MS = 5000;  // min gap between grid replies

// ------------------------------------------------------------- heartbeat
//
// Visual "I am alive" indicator on GPIO2, and the answer to "is it hung?".
// Driven by a Ticker, because loop() parks for seconds inside TLS - a
// loop()-driven LED would freeze during every send and look dead while the
// board is perfectly healthy.
//
//   lub-dub, 1.1 s pause   healthy (normal running)
//   lub-dub, 0.25 s pause  busy: WiFi connecting or a Telegram send running
//   100 ms fast blink      HUNG - the [hb] ALARM line says which kind
//
// A getUpdates long poll also parks the loop (~3 s, worst case its HTTP
// timeout) but is *normal* work, so it only suppresses the alarm
// (g_quiet_until) and never changes the beat: the LED keeps beating while
// the board is idle and working.
//
// A hang is caught two ways, so the LED never lies about the firmware:
//  1. the Ticker sees no g_loop_ms stamp for 5 s, or a "busy" call running
//     longer than BUSY_MAX_MS (more than any real send/connect) - it blinks
//     the alarm pattern and prints why;
//  2. the task watchdog panics after WDT_TIMEOUT_S and reboots the board -
//     the LED starts beating again and the boot banner reappears on serial.
//
static const uint32_t BUSY_MAX_MS = 25000;  // > send 15 s, > wifi connect 20 s
static const uint32_t WDT_TIMEOUT_S = 30;   // > the worst legitimate block

static Ticker g_ledTicker;
static volatile bool g_busy = false;
static volatile uint32_t g_busy_since = 0;
static volatile uint32_t g_loop_ms = 0;
static volatile uint32_t g_quiet_until = 0;  // alarm suppressed until this ms

// Stamp the start time *before* raising the flag, so the Ticker can never
// pair "busy" with a stale start.
static void setBusy(bool on) {
  if (on) {
    g_busy_since = millis();
    g_busy = true;
  } else {
    g_busy = false;
  }
}

static void updateLed(uint32_t now) {
  static const uint16_t BEAT[4] = {90, 160, 90, 1100};  // on, off, on, pause
  static const uint16_t FAST[4] = {90, 160, 90, 250};
  static uint8_t phase = 0;
  static uint32_t phaseStart = 0;
  static uint8_t lastMode = 255;
  if (phaseStart == 0) phaseStart = now;

  // Log every transition so the serial log shows which pattern the LED is
  // beating with right now (the LED itself can only be judged by eye).
  const bool quietOk = (int32_t)(now - g_quiet_until) >= 0;
  const bool busyTooLong = g_busy && ((now - g_busy_since) >= BUSY_MAX_MS);
  const bool stalled = !g_busy && quietOk && ((now - g_loop_ms) > 5000);
  const bool stuck = stalled || busyTooLong;
  const uint8_t mode = stuck ? 2 : (g_busy ? 1 : 0);

  if (mode != lastMode) {
    lastMode = mode;
    if (mode == 2) {
      Serial.printf(
          "[hb] ALARM: %s (loop last tick %lu ms ago, busy for %lu ms)\n",
          busyTooLong ? "busy call over limit" : "loop stuck",
          (unsigned long)(now - g_loop_ms),
          (unsigned long)(now - g_busy_since));
    } else {
      static const char *const NAME[2] = {"healthy beat", "fast beat (busy)"};
      Serial.printf("[hb] %s\n", NAME[mode]);
    }
  }

  if (stuck) {
    digitalWrite(LED_BUILTIN, ((now / 100) & 1) ? HIGH : LOW);
    return;
  }

  const uint16_t *p = g_busy ? FAST : BEAT;
  if (now - phaseStart >= p[phase]) {
    phase = (phase + 1) & 3;
    phaseStart = now;
  }
  digitalWrite(LED_BUILTIN, (phase == 0 || phase == 2) ? HIGH : LOW);
}

static void ledTick() { updateLed(millis()); }

// --------------------------------------------------------- sensor state
//
// Every field is <= 32 bits and aligned so that the Bluetooth task writing it
// and the Arduino task reading it never see a half-written value.

struct SensorState {
  volatile bool seen;
  volatile uint32_t last_seen_ms;
  volatile int rssi;
  volatile uint32_t packets;
  volatile bool warned_truncated;

  volatile bool has_temp;
  volatile float temp;
  volatile bool has_hum;
  volatile float hum;
  volatile bool has_battery;
  volatile uint8_t battery;
  volatile bool has_voltage;
  volatile float voltage;
  volatile bool has_packet_id;
  volatile uint8_t packet_id;
  volatile bool truncated;
};

static SensorState s_state[kSensorCount];

static volatile uint32_t g_adv_total = 0;
static volatile uint32_t g_adv_bthome = 0;
static volatile uint32_t g_adv_ours = 0;

// --------------------------------------------------------------- helpers

// Convert "AA:BB:CC:DD:EE:FF" into the byte order BLEAddress::getNative()
// returns, so it can be memcmp()'d against the address of an advertisement.
//
// Measured on this core (arduino-esp32 2.0.17 / Bluedroid): the stack keeps
// the address in the same order as the printed MAC, i.e. m_address[0] is the
// first octet of "AA:BB:...". Reversing the octets here (as an earlier
// version did) silently never matches, and every configured sensor then shows
// up as "not configured" with its MAC byte-swapped.
static bool mac_to_native(const char *s, uint8_t *out) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
             &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    if (v[i] > 0xFF) {
      return false;
    }
  }
  for (int i = 0; i < 6; i++) {
    out[i] = static_cast<uint8_t>(v[i]);
  }
  return true;
}

static uint32_t age_seconds(uint32_t then_ms, uint32_t now_ms) {
  return (uint32_t)(now_ms - then_ms) / 1000u;
}

static void update_cache(int idx, const bthome::Reading &rd, int rssi) {
  SensorState &st = s_state[idx];
  if (rd.has_temp || rd.has_hum || rd.has_battery) {
    // Merge, don't replace: a packet carrying only temperature should not
    // clear a good battery value from an earlier packet.
    if (rd.has_temp) {
      st.temp = rd.temp;
      st.has_temp = true;
    }
    if (rd.has_hum) {
      st.hum = rd.hum;
      st.has_hum = true;
    }
    if (rd.has_battery) {
      st.battery = rd.battery;
      st.has_battery = true;
    }
    if (rd.has_voltage) {
      st.voltage = rd.voltage;
      st.has_voltage = true;
    }
    if (rd.has_packet_id) {
      st.packet_id = rd.packet_id;
      st.has_packet_id = true;
    }
    st.truncated = rd.truncated;
    st.last_seen_ms = millis();
    st.rssi = rssi;
    st.packets++;
    st.seen = true;
  }

  st.truncated = rd.truncated;

  if (rd.truncated && !st.warned_truncated) {
    st.warned_truncated = true;
    Serial.printf("[warn] %s: payload truncated (encrypted, wrong version or "
                  "unknown object id)\n",
                  kSensors[idx].label);
  }
}

// One-shot diagnostics, reported once per MAC so a typo in kSensors shows up
// as a MAC instead of as "never seen".
static uint8_t g_foreign[8][6];
static size_t g_foreign_count = 0;
static uint8_t g_xiaomi[8][6];
static size_t g_xiaomi_count = 0;

static bool note_mac(uint8_t (&table)[8][6], size_t &count,
                     const uint8_t *native) {
  for (size_t i = 0; i < count; i++) {
    if (memcmp(table[i], native, 6) == 0) return false;
  }
  if (count >= sizeof(table) / sizeof(table[0])) return false;
  memcpy(table[count], native, 6);
  count++;
  return true;
}

static void print_mac(const uint8_t *native) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", native[0], native[1],
                native[2], native[3], native[4], native[5]);
}

static void note_foreign(const uint8_t *native, int rssi) {
  if (!note_mac(g_foreign, g_foreign_count, native)) return;
  Serial.print(F("[info] BTHome device not configured: "));
  print_mac(native);
  Serial.printf("  rssi %d\n", rssi);
}

// LYWSD03MMC shares the Xiaomi OUI A4:C1:38 across all three sensors. Any
// such advertiser is reported once, with or without a BTHome payload, so a
// sensor sitting in another format (ATC / MiBeacon) or out of range is
// distinguishable from a wrong MAC in kSensors.
static void note_xiaomi(const uint8_t *native, int rssi, bool has_bthome) {
  if (native[0] != 0xA4 || native[1] != 0xC1 || native[2] != 0x38) return;
  if (!note_mac(g_xiaomi, g_xiaomi_count, native)) return;
  Serial.print(F("[info] xiaomi device seen: "));
  print_mac(native);
  Serial.printf("  rssi %d  bthome %s\n", rssi, has_bthome ? "yes" : "no");
}

// Index of the BTHome service-data entry, or -1. The Arduino wrapper exposes
// service data as parallel arrays: iterate them by the same index and match
// the pair -- the no-argument getServiceData() / getServiceDataUUID() both
// return only the *first* entry, which is not necessarily a BTHome entry if a
// device advertises several services.
static int bthome_index(BLEAdvertisedDevice &dev) {
  const int n_data = dev.getServiceDataCount();
  const int n_uuid = dev.getServiceDataUUIDCount();
  const int n = (n_data < n_uuid) ? n_data : n_uuid;
  for (int i = 0; i < n; i++) {
    if (dev.getServiceDataUUID(i).equals(
            BLEUUID(static_cast<uint16_t>(bthome::kServiceUuid)))) {
      return i;
    }
  }
  return -1;
}

static void handle_device(BLEAdvertisedDevice dev) {
  g_adv_total++;

  uint8_t native[6];
  BLEAddress addr = dev.getAddress();
  memcpy(native, addr.getNative(), ESP_BD_ADDR_LEN);

  int idx = -1;
  for (size_t s = 0; s < kSensorCount; s++) {
    uint8_t want[6];
    if (!mac_to_native(kSensors[s].mac, want)) {
      continue;
    }
    if (memcmp(native, want, ESP_BD_ADDR_LEN) == 0) {
      idx = static_cast<int>(s);
      break;
    }
  }

  const int i = bthome_index(dev);
  note_xiaomi(native, dev.getRSSI(), i >= 0);
  if (i < 0) {
    return;  // no BTHome payload (most advertisers)
  }
  g_adv_bthome++;

  if (idx < 0) {
    note_foreign(native, dev.getRSSI());
    return;
  }
  g_adv_ours++;

  // The Arduino wrapper strips the 2-byte UUID, so this buffer starts at the
  // BTHome device-info byte -- exactly what parse_service_data() expects.
  //
  // std::string, not String: BLEAdvertisedDevice::getServiceData(int)
  // returns std::string on this core (2.0.17) and Arduino's String has no
  // constructor that accepts one.
  std::string svc = dev.getServiceData(i);
  if (svc.length() < 1) {
    return;
  }

  bthome::Reading rd = bthome::parse_service_data(
      reinterpret_cast<const uint8_t *>(svc.c_str()), svc.length());
  update_cache(idx, rd, dev.getRSSI());
}

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    handle_device(advertisedDevice);
  }
};

static BLEScan *g_scan = nullptr;

// This BLE library has no BLEScan::isScanning(), and the two-argument
// start(duration, is_continue) blocks the Arduino task on an internal
// semaphore for the whole scan (forever, for duration 0). So the scan is
// started with the callback overload -- it returns immediately -- and
// g_scanning mirrors what the completion callback reports.
static volatile bool g_scanning = false;
static uint32_t g_last_start_ms = 0;

static void on_scan_complete(BLEScanResults) { g_scanning = false; }

static void start_scan() {
  g_last_start_ms = millis();
  if (g_scan->start(0, on_scan_complete, /*is_continue=*/false)) {
    g_scanning = true;
  }
}

static void ble_service() {
  if (!g_scanning && (millis() - g_last_start_ms) >= 1000) {
    start_scan();
  }
}

// ----------------------------------------------------------- report text

struct Snapshot {
  bool seen;
  bool stale;
  bool has_temp;
  bool has_hum;
  bool has_battery;
  uint32_t age_s;
  int rssi;
  uint32_t packets;
  float temp;
  float hum;
  uint8_t battery;
};

static Snapshot take_snapshot(size_t s, uint32_t now) {
  Snapshot sn;
  memset(&sn, 0, sizeof sn);

  SensorState st = s_state[s];
  if (!st.seen) return sn;

  sn.seen = true;
  sn.age_s = age_seconds(st.last_seen_ms, now);
  sn.stale = sn.age_s > kMaxAgeSeconds[s];
  sn.rssi = st.rssi;
  sn.packets = st.packets;
  sn.has_temp = st.has_temp;
  if (sn.has_temp) sn.temp = st.temp;
  sn.has_hum = st.has_hum;
  if (sn.has_hum) sn.hum = st.hum;
  sn.has_battery = st.has_battery;
  if (sn.has_battery) sn.battery = st.battery;
  return sn;
}

// The grid that goes to Telegram: a status line, a header row, one row per
// sensor and a counter footer. It is sent inside <pre> (parse_mode HTML) so
// the columns line up in Telegram's monospace block; the identical plain text
// goes to the serial console, so what is sent can be read back there.
static String build_report(uint32_t now) {
  String msg;
  msg.reserve(448);
  char line[192];

  const bool up = WiFi.isConnected();
  snprintf(line, sizeof line, "BTHome sensors  uptime %lus  ip %s  wifi %ddBm",
           (unsigned long)(now / 1000u),
           up ? WiFi.localIP().toString().c_str() : "down",
           up ? (int)WiFi.RSSI() : 0);
  msg = line;
  msg += '\n';

  snprintf(line, sizeof line, "%-10s %-8s %-7s %-4s %6s  %s", "sensor", "temp",
           "hum", "bat", "rssi", "age");
  msg += line;
  msg += '\n';

  for (size_t s = 0; s < kSensorCount; s++) {
    Snapshot sn = take_snapshot(s, now);
    char temp[12] = "--", hum[14] = "--", bat[8] = "--", rssi[8] = "--",
         age[24] = "never seen";

    if (sn.seen) {
      if (sn.has_temp) snprintf(temp, sizeof temp, "%.2fC", sn.temp);
      if (sn.has_hum) snprintf(hum, sizeof hum, "%.2f%%", sn.hum);
      if (sn.has_battery) snprintf(bat, sizeof bat, "%u%%", sn.battery);
      snprintf(rssi, sizeof rssi, "%d", sn.rssi);
      if (sn.stale) {
        snprintf(age, sizeof age, "stale %lus", (unsigned long)sn.age_s);
      } else {
        snprintf(age, sizeof age, "%lus", (unsigned long)sn.age_s);
      }
    }

    snprintf(line, sizeof line, "%-10s %-8s %-7s %-4s %6s  %s",
             kSensors[s].label, temp, hum, bat, rssi, age);
    msg += line;
    msg += '\n';
  }

  snprintf(line, sizeof line, "adv %lu  bthome %lu  ours %lu",
           (unsigned long)g_adv_total, (unsigned long)g_adv_bthome,
           (unsigned long)g_adv_ours);
  msg += line;
  return msg;
}

static void print_summary() {
  const uint32_t now = millis();
  for (size_t s = 0; s < kSensorCount; s++) {
    Snapshot sn = take_snapshot(s, now);
    char line[176];
    if (!sn.seen) {
      snprintf(line, sizeof line, "%-10s --  never seen", kSensors[s].label);
    } else if (sn.stale) {
      snprintf(line, sizeof line, "%-10s --  stale %lus", kSensors[s].label,
               (unsigned long)sn.age_s);
    } else {
      char t[16] = "--", h[16] = "--", b[16] = "";
      if (sn.has_temp) snprintf(t, sizeof t, "%.2fC", sn.temp);
      if (sn.has_hum) snprintf(h, sizeof h, "%.2f%%", sn.hum);
      if (sn.has_battery) snprintf(b, sizeof b, "(%u%%)", sn.battery);
      snprintf(line, sizeof line,
               "%-10s %-9s %-8s %-6s rssi %4d  pkts %lu  age %lus",
               kSensors[s].label, t, h, b, sn.rssi,
               (unsigned long)sn.packets, (unsigned long)sn.age_s);
    }
    Serial.println(line);
  }
  Serial.printf("adv %lu  bthome %lu  ours %lu  heap %u maxblk %u\n",
                (unsigned long)g_adv_total, (unsigned long)g_adv_bthome,
                (unsigned long)g_adv_ours, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
  Serial.println(F("--"));
}

// --------------------------------------------------------- telegram send

enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };
static WifiState wifiState = WIFI_IDLE;
static uint32_t connStartMs = 0;
static uint32_t retryAtMs = 0;
static bool linkLostPrinted = false;
static bool bootTasksDone = false;
static bool webhookCleared = false;

static uint32_t lastPollMs = 0;  // last getUpdates attempt
static uint32_t lastGridMs = 0;  // last /temp reply (rate limit)

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
          snprintf(buf, sizeof buf, "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Escape for the HTML parse mode Telegram understands. Only needed for the
// <pre> grid, but applied to every preformatted message.
static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') {
      out += F("&amp;");
    } else if (c == '<') {
      out += F("&lt;");
    } else if (c == '>') {
      out += F("&gt;");
    } else {
      out += c;
    }
  }
  return out;
}

// preformatted = wrap the text in <pre> and send with parse_mode HTML, so a
// column grid keeps its alignment (Telegram has no monospace without it).
static bool telegramSend(const String &text, bool preformatted = false) {
  if (wifiState != WIFI_CONNECTED) {
    Serial.println(F("telegram: wifi not connected - message skipped"));
    return false;
  }

  setBusy(true);  // heartbeat switches to the fast beat during the TLS call

  String url =
      String(F("https://api.telegram.org/bot")) + BOT_TOKEN + F("/sendMessage");
  String payload;
  if (preformatted) {
    payload = String(F("{\"chat_id\":\"")) + jsonEscape(CHAT_ID) +
              F("\",\"parse_mode\":\"HTML\",\"text\":\"<pre>") +
              jsonEscape(htmlEscape(text)) + F("</pre>\"}");
  } else {
    payload = String(F("{\"chat_id\":\"")) + jsonEscape(CHAT_ID) +
              F("\",\"text\":\"") + jsonEscape(text) + F("\"}");
  }

  WiFiClientSecure client;
  client.setInsecure();  // TLS without certificate pinning
  HTTPClient http;
  http.setTimeout(15000);
  bool ok = false;
  int code = -1;
  String body;
  if (!http.begin(client, url)) {
    Serial.println(F("telegram: HTTPClient begin failed"));
  } else {
    http.addHeader(F("Content-Type"), F("application/json"));
    code = http.POST(payload);
    body = http.getString();
    http.end();
    ok = (code == HTTP_CODE_OK) && body.startsWith(F("{\"ok\":true"));
  }

  // The loop only re-stamps g_loop_ms on its next pass (~50 ms). Without this
  // the Ticker sees a stamp as old as the send and reports a bogus alarm the
  // instant the busy flag drops.
  g_loop_ms = millis();
  setBusy(false);
  Serial.printf("telegram send -> HTTP %d %s (heap free=%u min=%u maxblk=%u)\n",
                code, ok ? "ok" : "FAIL", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  if (!ok || ESP.getMaxAllocHeap() < HEAP_WARN_BYTES) {
    Serial.printf("WARN: send %s, largest free block %u (limit %u)\n",
                  ok ? "ok" : "FAILED", (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)HEAP_WARN_BYTES);
  }
  Serial.printf("telegram body: %.300s\n", body.c_str());
  return ok;
}

// ------------------------------------------------------ telegram receive
//
// getUpdates long poll (04TelegramRXTX's transport, verbatim where it
// matters): the TLS session dies inside telegramApi() *before* any JSON is
// parsed, and the filter keeps the document tiny.

static uint32_t updateOffset = 0;  // next getUpdates offset (server confirms)
static uint32_t updatesSeen = 0;

static int telegramApi(const String &args, String &body) {
  String url = String(F("https://api.telegram.org/bot")) + BOT_TOKEN + args;
  WiFiClientSecure client;
  client.setInsecure();  // TLS without certificate pinning
  HTTPClient http;
  http.setTimeout(20000);  // must exceed the long-poll hold time

  // Polling parks the loop for seconds - that is normal work, so tell the
  // heartbeat not to read it as a hang while it runs.
  g_quiet_until = millis() + 8000;

  int code = -1;
  body.clear();
  if (!http.begin(client, url)) {
    body = "HTTPClient begin failed";
  } else {
    code = http.GET();
    body = http.getString();
    http.end();
  }
  // Deliberately *not* clearing g_quiet_until here: it stays at entry+8 s.
  // Clearing it let the Ticker see the whole poll duration as a stuck loop
  // and print a bogus ALARM before loop() got its next stamp. If the loop
  // really hangs, no further poll extends the horizon, so it expires 8 s
  // after the last poll and the alarm still fires.
  g_loop_ms = millis();
  return code;
}

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

static void handleCommand(const String &raw, long chatId);

static void handleUpdate(JsonObject upd) {
  uint32_t uid = upd["update_id"] | 0U;
  if (uid) updateOffset = uid + 1;  // server confirms everything below this
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

    if (text && text[0]) handleCommand(String(text), chatId);
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

// The grid is sent *only* from here, and only for /temp from the owner.
// Nothing else in the firmware puts a table on the channel - there is no
// timer left, so a silent bot means nobody asked.
static void handleCommand(const String &raw, long chatId) {
  String cmd = raw;
  cmd.trim();
  if (cmd.length() == 0 || cmd[0] != '/') return;  // plain chatter, not a command

  // /temp@<bot-username> is what Telegram delivers in groups.
  const int at = cmd.indexOf('@');
  if (at > 0) cmd = cmd.substring(0, at);

  if (String(chatId) != CHAT_ID) {
    Serial.printf("TG: ignored - chat %ld is not the owner\n", chatId);
    return;
  }

  if (cmd != "/temp") {
    Serial.printf("TG: unknown command %.40s (try /temp)\n", cmd.c_str());
    return;
  }

  const uint32_t now = millis();
  if (now - lastGridMs < COMMAND_GAP_MS) {
    Serial.println(F("TG: /temp ignored - replied less than 5 s ago"));
    return;
  }
  lastGridMs = now;

  const String msg = build_report(now);
  Serial.print(F("TG TX:\n"));
  Serial.print(msg);
  Serial.println();
  telegramSend(msg, /*preformatted=*/true);
}

static void servicePoll() {
  if (wifiState != WIFI_CONNECTED) return;

  const uint32_t now = millis();
  if (now - lastPollMs < 1000) return;  // pace fast failures; a real poll
                                         // cycle already takes ~3 s
  lastPollMs = now;

  String args = String(F("/getUpdates?timeout=")) + POLL_TIMEOUT_S +
                F("&limit=") + POLL_LIMIT + F("&offset=") + updateOffset;
  String body;  // bounded by POLL_LIMIT, freed on return
  int code = telegramApi(args, body);

  if (code != HTTP_CODE_OK) {
    Serial.printf("poll failed: HTTP %d %.200s\n", code, body.c_str());
    if (ESP.getMaxAllocHeap() < HEAP_WARN_BYTES) {
      Serial.printf("WARN: largest free block %u (limit %u)\n",
                    (unsigned)ESP.getMaxAllocHeap(), (unsigned)HEAP_WARN_BYTES);
    }
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

  // One line per update, plus a heap sample roughly once a minute (20 polls *
  // ~3 s) - enough to see that polling is alive and whether the heap drifts,
  // without printing every empty poll.
  static uint32_t pollCount = 0;
  pollCount++;
  if (n || (pollCount % 20 == 0)) {
    Serial.printf(
        "poll: %u new (total %u seen, offset %u, #%u) heap free=%u min=%u "
        "maxblk=%u\n",
        (unsigned)n, (unsigned)updatesSeen, (unsigned)updateOffset,
        (unsigned)pollCount, (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  }
}

// ------------------------------------------------------------------ wifi
static void connectNow() {
  Serial.printf("connecting to \"%s\" ...\n", WIFI_SSID);
  setBusy(true);  // fast beat while associating (cleared on connect/timeout)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiState = WIFI_CONNECTING;
  connStartMs = millis();
}

static void onConnected() {
  wifiState = WIFI_CONNECTED;
  linkLostPrinted = false;
  setBusy(false);
  Serial.printf("connected to \"%s\" ip=%s rssi=%d dBm\n", WIFI_SSID,
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

  // getUpdates only works when no webhook is registered - clear it once.
  if (!webhookCleared) {
    webhookCleared = true;
    String body;
    int code = telegramApi(F("/deleteWebhook"), body);
    if (code == HTTP_CODE_OK && body.indexOf(F("\"ok\":true")) >= 0) {
      Serial.println(F("webhook cleared - getUpdates polling enabled"));
    } else {
      Serial.printf("deleteWebhook -> HTTP %d %.200s\n", code, body.c_str());
    }
  }

  if (!bootTasksDone) {
    bootTasksDone = true;
    String hello = String(F("08BlueToothTelegram online: ssid=")) + WIFI_SSID +
                   F(" ip=") + WiFi.localIP().toString() +
                   F(" rssi=") + WiFi.RSSI() + F(" dBm");
    telegramSend(hello);
  }
}

// -------------------------------------------------------------- serial
// The console mirrors the two inputs Telegram has, so the receive path can be
// exercised without picking up the phone:
//   /temp  -> the very handler the bot runs for a Telegram message
//   <text> -> sent to the channel as-is (ad-hoc test message)
static void handleSerial() {
  static char buf[TX_LINE_MAX];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      if (len > 0) {
        String line(buf);
        if (line[0] == '/') {
          Serial.printf("local: %s\n", buf);
          handleCommand(line, atol(CHAT_ID));
        } else {
          Serial.printf("sending: %s\n", buf);
          telegramSend(line);
        }
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

// ------------------------------------------------------------ scheduler
static void serviceWifi() {
  const uint32_t now = millis();
  if (wifiState == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onConnected();
    } else if ((int32_t)(now - connStartMs) > (int32_t)CONNECT_TIMEOUT_MS) {
      Serial.println(F("connect timeout - retrying"));
      setBusy(false);
      wifiState = WIFI_IDLE;
      retryAtMs = now + RETRY_DELAY_MS;
    }
  } else if (wifiState == WIFI_IDLE) {
    if (retryAtMs && (int32_t)(now - retryAtMs) >= 0) {
      retryAtMs = 0;
      connectNow();
    }
  } else {  // WIFI_CONNECTED
    if (WiFi.status() != WL_CONNECTED) {
      if (!linkLostPrinted) {
        Serial.println(F("WiFi link lost - reconnecting ..."));
        linkLostPrinted = true;
        setBusy(true);
        wifiState = WIFI_CONNECTING;
        connStartMs = now;
      }
    }
  }
}

// ---------------------------------------------------------------- arduino
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=== 08BlueToothTelegram ==="));
  Serial.println(F("LED GPIO2 heartbeat: lub-dub 1.1s pause = healthy,"));
  Serial.println(
      F("  fast lub-dub = wifi/telegram busy, 100ms blink = loop stuck"));
  Serial.println(
      F("telegram: /temp -> sensor table (type it here too, same handler)"));
  Serial.println(F("other typed lines are sent to the channel as plain text"));

  // Attached after Serial: the heartbeat logs each pattern change as [hb].
  pinMode(LED_BUILTIN, OUTPUT);
  g_loop_ms = millis();
  g_ledTicker.attach_ms(10, ledTick);

  // Last resort: if loop() ever stops for longer than any legitimate blocking
  // call (send 15 s, wifi connect 20 s, poll 10 s), the task watchdog panics
  // and reboots - the LED starts beating again and the boot banner reappears
  // on the serial console. loop() feeds it on every pass.
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);  // setup() and loop() run in the same task

  Serial.println(F("BTHome sensor reader starting..."));
  BLEDevice::init("");
  g_scan = BLEDevice::getScan();

  // wantDuplicates = true: report every advertisement, not just the first
  // sighting, so cached timestamps stay fresh.
  g_scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  g_scan->setActiveScan(true);
  g_scan->setInterval(50);  // ms
  g_scan->setWindow(30);    // ms -> 60% duty cycle
  start_scan();
  Serial.println(F("scanning..."));

  connectNow();
}

void loop() {
  g_loop_ms = millis();  // heartbeat "loop is alive" stamp
  esp_task_wdt_reset();  // ... and prove it to the task watchdog

  handleSerial();
  serviceWifi();
  ble_service();

  static uint32_t last_print = 0;
  const uint32_t now = millis();
  if (now - last_print >= PRINT_INTERVAL_MS) {
    last_print = now;
    print_summary();
  }

  servicePoll();  // getUpdates - the only thing that triggers a grid

#ifdef HANG_TEST
  // Verification only (built with PLATFORMIO_BUILD_FLAGS=-DHANG_TEST):
  // a fake hang, once WiFi is up, so the "loop stuck" alarm and the watchdog
  // reboot are exercised end to end on a real board. Never in a normal build.
  if (wifiState == WIFI_CONNECTED) delay(35000);
#endif

  delay(50);
}
