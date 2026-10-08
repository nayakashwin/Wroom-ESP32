#include <Arduino.h>

#include <BLEAddress.h>
#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUUID.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "bthome_parser.h"

// ---------------------------------------------------------------- config

struct SensorDef {
  const char *key;
  const char *label;
  const char *mac;
};

// The three sensors, same as atc.sh / bthome-mon.sh on the Debian side.
static const SensorDef kSensors[] = {
    {"ria", "Rias Room", "A4:C1:38:3B:E2:F6"},
    {"kitchen", "Kitchen", "A4:C1:38:D9:0C:0C"},
    {"basement", "Basement", "A4:C1:38:10:49:7C"},
};
static const size_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

// Seconds a cached advertisement may be before it is shown as stale. Kitchen is
// a marginal link (~-92 dBm) and advertises far less often, hence 1800.
static const uint32_t kMaxAgeSeconds[] = {600, 1800, 600};

static const uint32_t PRINT_INTERVAL_MS = 5000;

// ------------------------------------------------------------- state
//
// Every field is <= 32 bits and aligned so that the Bluetooth task writing it
// and the Arduino task reading it never see a half-written value.

struct SensorState {
  volatile bool seen;
  volatile uint32_t last_seen_ms;  // millis() at reception
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

// ------------------------------------------------------------- helpers

// Convert "AA:BB:CC:DD:EE:FF" into the byte order BLEAddress::getNative()
// returns, so it can be memcmp()'d against the address of an advertisement.
//
// Measured on this core (arduino-esp32 2.0.17 / Bluedroid): the stack keeps
// the address in the same order as the printed MAC, i.e. m_address[0] is the
// first octet of "AA:BB:...", and BLEAddress::toString() prints m_address[0..5]
// as-is. The bytes are therefore copied straight across: reversing them here
// (as an earlier version did) silently never matches, and every configured
// sensor then shows up as "not configured" with its MAC byte-swapped.
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

// Age in seconds, wrap-safe: unsigned subtraction tolerates the 49.7-day
// millis() rollover, provided the real age is well under that.
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

  // Publish "seen" last so a reader never sees a half-updated record.
  st.truncated = rd.truncated;

  if (rd.truncated && !st.warned_truncated) {
    st.warned_truncated = true;
    Serial.printf("[warn] %s: payload truncated (encrypted, wrong version or "
                  "unknown object id)\n",
                  kSensors[idx].label);
  }
}

// Advertisement counters: they tell "no BTHome device in range" apart from
// "the scan is not receiving anything" (printed with every summary).
static volatile uint32_t g_adv_total = 0;    // every advertisement
static volatile uint32_t g_adv_bthome = 0;   // carrying the BTHome service
static volatile uint32_t g_adv_ours = 0;     // ... from one of our three MACs

// Unconfigured advertisers of interest, reported once each so a typo in
// kSensors shows up as a MAC instead of as "never seen".
static uint8_t g_foreign[8][6];
static size_t g_foreign_count = 0;
static uint8_t g_xiaomi[8][6];
static size_t g_xiaomi_count = 0;

// Reserve a "printed already" slot for a MAC; false if it was reported
// before or if the table is full.
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

// m_address[0] is the first octet of the printed MAC (see mac_to_native).
static void print_mac(const uint8_t *native) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", native[0], native[1],
                native[2], native[3], native[4], native[5]);
}

// A BTHome advertiser that is not in kSensors.
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

// Decode one advertisement if it came from one of our sensors.
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
    return;  // BTHome, but not one of ours
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

// ---------------------------------------------------------- printer

static void print_summary() {
  const uint32_t now = millis();

  for (size_t s = 0; s < kSensorCount; s++) {
    // Snapshot into a plain local so the formatter works with non-volatile
    // values and the format string never reads a field twice.
    SensorState st = s_state[s];
    char line[160];

    if (!st.seen) {
      snprintf(line, sizeof line, "%-10s --  never seen", kSensors[s].label);
    } else {
      const uint32_t age_s = age_seconds(st.last_seen_ms, now);
      if (age_s > kMaxAgeSeconds[s]) {
        snprintf(line, sizeof line, "%-10s --  stale %lus", kSensors[s].label,
                 static_cast<unsigned long>(age_s));
      } else {
        char t[16] = "--", h[16] = "--", b[16] = "";
        if (st.has_temp) {
          snprintf(t, sizeof t, "%.2fC", st.temp);
        }
        if (st.has_hum) {
          snprintf(h, sizeof h, "%.2f%%", st.hum);
        }
        if (st.has_battery) {
          snprintf(b, sizeof b, "(%u%%)", st.battery);
        }
        snprintf(line, sizeof line,
                 "%-10s %-9s %-8s %-6s rssi %4d  pkts %lu  age %lus",
                 kSensors[s].label, t, h, b, st.rssi,
                 static_cast<unsigned long>(st.packets),
                 static_cast<unsigned long>(age_s));
      }
    }
    Serial.println(line);
  }
  Serial.printf("adv %lu  bthome %lu  ours %lu\n",
                (unsigned long)g_adv_total, (unsigned long)g_adv_bthome,
                (unsigned long)g_adv_ours);
  Serial.println("--");
}

// ------------------------------------------------------------ Arduino

void setup() {
  Serial.begin(115200);
  delay(200);  // let the USB CDC/UART settle

  Serial.println("BTHome sensor reader starting...");

  BLEDevice::init("");
  g_scan = BLEDevice::getScan();

  // wantDuplicates = true: report every advertisement, not just the first
  // sighting, so cached timestamps stay fresh. This is what makes the staleness
  // reporting meaningful.
  g_scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), true);
  g_scan->setActiveScan(true);
  g_scan->setInterval(50);  // ms
  g_scan->setWindow(30);    // ms -> 60% duty cycle

  // duration 0 = scan indefinitely (asynchronous, so loop() keeps running).
  start_scan();
  Serial.println("scanning...");
}

void loop() {
  // The scan is asynchronous; this keeps it alive and paces the printing.
  if (!g_scanning && (millis() - g_last_start_ms) >= 1000) {
    start_scan();
  }

  static uint32_t last_print = 0;
  const uint32_t now = millis();
  if (now - last_print >= PRINT_INTERVAL_MS) {
    last_print = now;
    print_summary();
  }

  delay(50);
}
