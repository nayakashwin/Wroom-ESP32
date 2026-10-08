#include "bthome_parser.h"

namespace bthome {

namespace {

// How many bytes an object carries and how the integer is interpreted.
// The float factor is applied after reading (see the table on bthome.io).
enum Kind : uint8_t {
  K_U8, K_S8, K_U16, K_S16, K_U24, K_U32, K_S32,
  K_SKIP2,     // fixed 2 bytes we do not use (0x3C dimmer)
  K_VAR_TEXT,  // length byte + N bytes (0x53 text, 0x54 raw)
  K_VAR_CMD,   // length byte (low 5 bits = arg length) + opcode + args (0x3B)
};

struct Obj {
  uint8_t id;
  uint8_t kind;
  float factor;
};

// Every fixed-size object id of BTHome v2. Object ids must arrive in
// ascending order, so this table is kept sorted and parsing stops at the
// first id that is not listed (spec: receivers stop at unknown ids).
const Obj kObjects[] = {
    {0x00, K_U8, 1.0f},        // packet id
    {0x01, K_U8, 1.0f},        // battery
    {0x02, K_S16, 0.01f},      // temperature
    {0x03, K_U16, 0.01f},      // humidity
    {0x04, K_U24, 0.01f},      // pressure
    {0x05, K_U24, 0.01f},      // illuminance
    {0x06, K_U16, 0.01f},      // mass (kg)
    {0x07, K_U16, 0.01f},      // mass (lb)
    {0x08, K_S16, 0.01f},      // dew point
    {0x09, K_U8, 1.0f},        // count
    {0x0A, K_U24, 0.001f},     // energy
    {0x0B, K_U24, 0.01f},      // power
    {0x0C, K_U16, 0.001f},     // voltage
    {0x0D, K_U16, 1.0f},       // pm2.5
    {0x0E, K_U16, 1.0f},       // pm10
    {0x0F, K_U8, 1.0f},        // generic boolean
    {0x10, K_U8, 1.0f},        // power (binary)
    {0x11, K_U8, 1.0f},        // opening
    {0x12, K_U16, 1.0f},       // co2
    {0x13, K_U16, 1.0f},       // tvoc
    {0x14, K_U16, 0.01f},      // moisture
    {0x15, K_U8, 1.0f},        // battery low
    {0x16, K_U8, 1.0f},        // battery charging
    {0x17, K_U8, 1.0f},        // carbon monoxide
    {0x18, K_U8, 1.0f},        // cold
    {0x19, K_U8, 1.0f},        // connectivity
    {0x1A, K_U8, 1.0f},        // door
    {0x1B, K_U8, 1.0f},        // garage door
    {0x1C, K_U8, 1.0f},        // gas
    {0x1D, K_U8, 1.0f},        // heat
    {0x1E, K_U8, 1.0f},        // light
    {0x1F, K_U8, 1.0f},        // lock
    {0x20, K_U8, 1.0f},        // moisture (binary)
    {0x21, K_U8, 1.0f},        // motion
    {0x22, K_U8, 1.0f},        // moving
    {0x23, K_U8, 1.0f},        // occupancy
    {0x24, K_U8, 1.0f},        // plug
    {0x25, K_U8, 1.0f},        // presence
    {0x26, K_U8, 1.0f},        // problem
    {0x27, K_U8, 1.0f},        // running
    {0x28, K_U8, 1.0f},        // safety
    {0x29, K_U8, 1.0f},        // smoke
    {0x2A, K_U8, 1.0f},        // sound
    {0x2B, K_U8, 1.0f},        // tamper
    {0x2C, K_U8, 1.0f},        // vibration
    {0x2D, K_U8, 1.0f},        // window
    {0x2E, K_U8, 1.0f},        // humidity (uint8)
    {0x2F, K_U8, 1.0f},        // moisture (uint8)
    {0x3A, K_U8, 1.0f},        // button event
    {0x3B, K_VAR_CMD, 1.0f},   // command (variable)
    {0x3C, K_SKIP2, 1.0f},     // dimmer event (2 bytes)
    {0x3D, K_U16, 1.0f},       // count
    {0x3E, K_U32, 1.0f},       // count
    {0x3F, K_S16, 0.1f},       // rotation
    {0x40, K_U16, 1.0f},       // distance (mm)
    {0x41, K_U16, 0.1f},       // distance (m)
    {0x42, K_U24, 0.001f},     // duration
    {0x43, K_U16, 0.001f},     // current
    {0x44, K_U16, 0.01f},      // speed
    {0x45, K_S16, 0.1f},       // temperature (0.1)
    {0x46, K_U8, 0.1f},        // UV index
    {0x47, K_U16, 0.1f},       // volume
    {0x48, K_U16, 1.0f},       // volume (mL)
    {0x49, K_U16, 0.001f},     // volume flow rate
    {0x4A, K_U16, 0.1f},       // voltage (0.1)
    {0x4B, K_U24, 0.001f},     // gas
    {0x4C, K_U32, 0.001f},     // gas
    {0x4D, K_U32, 0.001f},     // energy
    {0x4E, K_U32, 0.001f},     // volume
    {0x4F, K_U32, 0.001f},     // water
    {0x50, K_U32, 1.0f},       // timestamp
    {0x51, K_U16, 0.001f},     // acceleration
    {0x52, K_U16, 0.001f},     // gyroscope
    {0x53, K_VAR_TEXT, 1.0f},  // text (variable)
    {0x54, K_VAR_TEXT, 1.0f},  // raw (variable)
    {0x55, K_U32, 0.001f},     // volume storage
    {0x56, K_U16, 1.0f},       // conductivity
    {0x57, K_S8, 1.0f},        // temperature (int8)
    {0x58, K_S8, 0.35f},       // temperature (int8, 0.35)
    {0x59, K_S8, 1.0f},        // count (int8)
    {0x5A, K_S16, 1.0f},       // count (int16)
    {0x5B, K_S32, 1.0f},       // count (int32)
    {0x5C, K_S32, 0.01f},      // power (signed)
    {0x5D, K_S16, 0.001f},     // current (signed)
    {0x5E, K_U16, 0.01f},      // direction
    {0x5F, K_U16, 0.1f},       // precipitation
    {0x60, K_U8, 1.0f},        // channel
    {0x61, K_U16, 1.0f},       // rotational speed
    {0x62, K_S32, 0.000001f},  // speed (signed)
    {0x63, K_S32, 0.000001f},  // acceleration (signed)
    {0x64, K_U8, 1.0f},        // light level
    {0x65, K_U8, 1.0f},        // settings revision
    {0xF0, K_U16, 1.0f},       // device type id
    {0xF1, K_U32, 1.0f},       // firmware version
    {0xF2, K_U24, 1.0f},       // firmware version
};

const Obj *findObject(uint8_t id) {
  for (size_t i = 0; i < sizeof(kObjects) / sizeof(kObjects[0]); i++) {
    if (kObjects[i].id == id) return &kObjects[i];
  }
  return nullptr;
}

int widthOf(uint8_t kind) {
  switch (kind) {
    case K_U8:
    case K_S8:
      return 1;
    case K_U16:
    case K_S16:
    case K_SKIP2:
      return 2;
    case K_U24:
      return 3;
    case K_U32:
    case K_S32:
      return 4;
    default:
      return -1;  // variable length, handled separately
  }
}

// Little-endian read of 1..4 bytes.
uint32_t readLE(const uint8_t *p, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
  return v;
}

int32_t signExtend(uint32_t v, int n) {
  if (n >= 4) return (int32_t)v;
  const uint32_t msb = 1u << (8 * n - 1);
  if (v & msb) v |= ~((1u << (8 * n)) - 1u);
  return (int32_t)v;
}

bool isSignedKind(uint8_t kind) {
  return kind == K_S8 || kind == K_S16 || kind == K_S32;
}

}  // namespace

Reading parse_service_data(const uint8_t *data, size_t len) {
  Reading rd;
  if (data == nullptr || len < 1) {
    rd.truncated = true;
    return rd;
  }

  const uint8_t info = data[0];
  const bool encrypted = (info & 0x01) != 0;
  const uint8_t version = (info >> 5) & 0x07;
  if (encrypted || version != 2) {
    // Nothing here can be trusted: encrypted payload we have no key for, or
    // a BTHome version we do not speak.
    rd.truncated = true;
    return rd;
  }

  size_t i = 1;  // byte 0 is the device-information byte
  while (i < len) {
    const uint8_t id = data[i++];
    const Obj *obj = findObject(id);
    if (obj == nullptr) {
      rd.truncated = true;  // unsupported object id: stop, keep what we have
      break;
    }

    if (obj->kind == K_VAR_TEXT || obj->kind == K_VAR_CMD) {
      if (i >= len) {
        rd.truncated = true;
        break;
      }
      const size_t payload = (obj->kind == K_VAR_CMD) ? (size_t)(data[i] & 0x1F) + 1
                                                      : (size_t)data[i];
      const size_t total = 1 + payload;  // length byte + payload
      if (i + total > len) {
        rd.truncated = true;
        break;
      }
      i += total;
      continue;
    }

    const int w = widthOf(obj->kind);
    if (w < 0 || i + (size_t)w > len) {
      rd.truncated = true;
      break;
    }

    const uint8_t *p = data + i;
    const uint32_t raw = readLE(p, w);
    const float value =
        isSignedKind(obj->kind) ? (float)signExtend(raw, w) * obj->factor
                                : (float)raw * obj->factor;
    i += (size_t)w;

    switch (id) {
      case 0x00:
        rd.has_packet_id = true;
        rd.packet_id = (uint8_t)raw;
        break;
      case 0x01:
        rd.has_battery = true;
        rd.battery = (uint8_t)raw;
        break;
      case 0x02:
      case 0x45:
      case 0x57:
      case 0x58:
        rd.has_temp = true;
        rd.temp = value;
        break;
      case 0x03:
      case 0x2E:
        rd.has_hum = true;
        rd.hum = value;
        break;
      case 0x0C:
      case 0x4A:
        rd.has_voltage = true;
        rd.voltage = value;
        break;
      default:
        break;  // parsed and skipped: not one of the fields we keep
    }
  }

  return rd;
}

}  // namespace bthome
