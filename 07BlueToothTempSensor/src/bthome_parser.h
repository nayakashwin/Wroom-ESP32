// Minimal BTHome v2 advertisement parser.
//
// Format: https://bthome.io/format/  - service data = 16-bit UUID (0xFCD2,
// little endian in the packet) + 1 device-information byte + measurements.
// The BLE wrapper already strips the UUID, so the buffer handed to
// parse_service_data() starts at the device-information byte.
//
// Values are decoded into Reading; an object id we do not know, a payload
// that runs out of bytes, an unsupported BTHome version or the encryption
// flag all end parsing and set Reading::truncated.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bthome {

// 16-bit service UUID, little endian in the packet (D2 FC).
static const uint16_t kServiceUuid = 0xFCD2;

struct Reading {
  bool has_temp = false;
  float temp = 0.0f;          // degC
  bool has_hum = false;
  float hum = 0.0f;           // %
  bool has_battery = false;
  uint8_t battery = 0;        // %
  bool has_voltage = false;
  float voltage = 0.0f;       // V
  bool has_packet_id = false;
  uint8_t packet_id = 0;
  bool truncated = false;     // encrypted / wrong version / unknown id
};

Reading parse_service_data(const uint8_t *data, size_t len);

}  // namespace bthome
